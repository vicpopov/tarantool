/*
 * Copyright 2010-2016, Tarantool AUTHORS, please see AUTHORS file.
 *
 * Redistribution and use in source and binary forms, with or
 * without modification, are permitted provided that the following
 * conditions are met:
 *
 * 1. Redistributions of source code must retain the above
 *    copyright notice, this list of conditions and the
 *    following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above
 *    copyright notice, this list of conditions and the following
 *    disclaimer in the documentation and/or other materials
 *    provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY <COPYRIGHT HOLDER> ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL
 * <COPYRIGHT HOLDER> OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
 * THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */
#include "iproto.h"
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>

#include <msgpuck.h>
#include "third_party/base64.h"

#include "version.h"
#include "fiber.h"
#include "cbus.h"
#include "say.h"
#include "sio.h"
#include "evio.h"
#include "scoped_guard.h"

#include "port.h"
#include "iobuf.h"
#include "box.h"
#include "call.h"
#include "tuple_convert.h"
#include "session.h"
#include "xrow.h"
#include "schema.h" /* schema_version */
#include "replication.h" /* instance_uuid */
#include "iproto_constants.h"
#include "rmean.h"
#include "iproto_buffer.h"
#include "errinj.h"

/* The number of iproto messages in flight */
enum { IPROTO_MSG_MAX = 768 };

/* {{{ iproto_msg - declaration */

/**
 * A single msg from io thread. All requests
 * from all connections are queued into a single queue
 * and processed in FIFO order.
 */
struct iproto_msg
{
	/**
	 * Message to deliver request data to tx thread and back.
	 */
	struct cmsg request_msg;
	/**
	 * Message to discard request data, stored in ibuf when it
	 * is not needed anymore. Call and eval discard input data
	 * before lua_call. It allows to reuse ibuf for newer
	 * requests while call/eval is working.
	 */
	struct cmsg discard_ibuf_msg;
	struct iproto_connection *connection;

	/* --- Box msgs - actual requests for the transaction processor --- */
	/* Request message code and sync. */
	struct xrow_header header;
	union {
		/* Box request, if this is a DML */
		struct request dml_request;
		/* Box request, if this is misc (call, eval). */
		struct call_request call_request;
		/* Authentication request. */
		struct auth_request auth_request;
	};
	/*
	 * Remember the ibuf of the connection, in which the
	 * request is stored. Used to discard request data when
	 * it is not needed anymore.
	 */
	struct ibuf *ibuf;
	/**
	 * How much space the request takes in the
	 * input buffer (len, header and body - all of it)
	 * This also works as a reference counter to ibuf object.
	 */
	size_t len;
	/**
	 * Used in "connect" msgs, true if connect trigger failed
	 * and the connection must be closed.
	 */
	bool close_connection;
	/**
	 * List of flushed empty buffers to send them back to tx
	 * thread and utilize.
	 */
	struct rlist flushed_buffers;
	/**
	 * List of full buffers, received from tx thread to flush
	 * into sockets.
	 */
	struct rlist buffers_to_flush;
};

static struct mempool iproto_msg_pool;

static struct iproto_msg *
iproto_msg_new(struct iproto_connection *con)
{
	struct iproto_msg *msg =
		(struct iproto_msg *) mempool_alloc_xc(&iproto_msg_pool);
	msg->connection = con;
	rlist_create(&msg->flushed_buffers);
	rlist_create(&msg->buffers_to_flush);
	return msg;
}

/**
 * Resume stopped connections, if any.
 */
static void
iproto_resume();

static inline void
iproto_msg_delete(struct iproto_msg *msg)
{
	assert(rlist_empty(&msg->flushed_buffers));
	assert(rlist_empty(&msg->buffers_to_flush));
	mempool_free(&iproto_msg_pool, msg);
	iproto_resume();
}

/* }}} */

/* {{{ iproto connection and requests */

/**
 * A single global queue for all requests in all connections. All
 * requests from all connections are processed concurrently.
 * Is also used as a queue for just established connections and to
 * execute disconnect triggers. A few notes about these triggers:
 * - they need to be run in a fiber
 * - unlike an ordinary request failure, on_connect trigger
 *   failure must lead to connection close.
 * - on_connect trigger must be processed before any other
 *   request on this connection.
 */
static struct cpipe tx_pipe;
static struct cpipe net_pipe;
/* A pointer to the transaction processor cord. */
struct cord *tx_cord;

struct rmean *rmean_net;

enum rmean_net_name {
	IPROTO_SENT,
	IPROTO_RECEIVED,
	IPROTO_LAST,
};

const char *rmean_net_strings[IPROTO_LAST] = { "SENT", "RECEIVED" };

/**
 * Context of a single client connection.
 * Interaction scheme:
 *
 *  Receive from the network.
 *     |
 * +---|---------------------+   +----------------------+
 * |   |      IPROTO thread  |   | TX thread            |
 * |   v                     |   |                      |
 * |ibuf[k] - - - - - - - - -|- -|- - >+ create obuf  <-|- - +
 * |                         |   |     |                |    |
 * |          ibuf[(k+1)%2]  |   |     | write response |    |
 * |       pending responses |   |     | write response |    |
 * |                         |   |     | ...            |    |
 * |iproto_buffer[N] <-+     |   |     | write response |    |
 * |iproto_buffer[N-1] |     |   |     |                |    |
 * |     ...           +< - -|- -|- - -+ send obuf in   |    |
 * |iproto_buffer[0]         |   |       batch          |    |
 * +------|------------------+   +----------------------+    |
 *        v                                                  |
 *  Send to network and return to Tx  - - - - - - - - - - - -+
 *
 * ibuf structure:
 *                   rpos             wpos           end
 * +-------------------|----------------|-------------+
 * \________/\________/ \________/\____/
 *  \  msg       msg /    msg     parse
 *   \______________/             size
 *   response is sent,
 *     messages are
 *      discarded
 */
struct iproto_connection
{
	/**
	 * Two rotating input buffers. As soon as active buffer
	 * becomes full the another ibuf becomes active, and old
	 * one is waited for discarding all its requests. Each
	 * request is discarded when its response is wrote by tx
	 * thread in an output buffer.
	 */
	struct ibuf ibuf[2];
	struct ibuf *active_ibuf;
	/*
	 * Size of readahead which is not parsed yet, i.e. size of
	 * a piece of request which is not fully read. Is always
	 * relative to active_ibuf->wpos. In other words,
	 * active_ibuf->wpos - parse_size gives the start of the
	 * unparsed request. A size rather than a pointer is used
	 * to be safe in case ibuf.buf is reallocated. Being
	 * relative to ibuf.wpos, rather than to ibuf.rpos is
	 * helpful to make sure ibuf_reserve() or ibuf rotation
	 * don't make the value meaningless.
	 */
	size_t parse_size;
	struct ev_io input;
	struct ev_io output;
	/** Logical session. */
	struct session *session;
	ev_loop *loop;
	/* Pre-allocated disconnect msg. */
	struct iproto_msg *disconnect;
	struct rlist in_stop_list;
	/**
	 * Buffer used by tx thread for a current tx event loop.
	 * All responses for this connection during the event loop
	 * are wrote into this buffer. Then the buffer is sent to
	 * iproto and a pointer is removed from active_obuf.
	 */
	struct iproto_buffer *active_obuf;
	/**
	 * List of buffers to flush into socket. The newer buffer,
	 * the closer it is to the list tail.
	 */
	struct rlist buffers_to_flush;
};

/**
 * Get not active ibuf. Used to rotate buffers by assigning
 * next_ibuf() to the connection.active_ibuf.
 * @param conn Connection to get ibuf.
 * @retval Not active ibuf.
 */
static inline struct ibuf *
iproto_connection_next_ibuf(struct iproto_connection *conn)
{
	if (conn->active_ibuf == &conn->ibuf[0])
		return &conn->ibuf[1];
	else
		return &conn->ibuf[0];
}

/**
 * Try to start input if it is stopped. Used when an ibuf
 * request is discarded. It could allow to reset ibuf and read new
 * requests.
 * @param conn Connection to resume input.
 */
static inline void
iproto_connection_resume_input(struct iproto_connection *conn)
{
	if (!ev_is_active(&conn->input) && rlist_empty(&conn->in_stop_list))
		ev_feed_event(conn->loop, &conn->input, EV_READ);
}

static struct mempool iproto_connection_pool;
static RLIST_HEAD(stopped_connections);

/**
 * Return true if we have not enough spare messages
 * in the message pool. Disconnect messages are
 * discounted: they are mostly reserved and idle.
 */
static inline bool
iproto_must_stop_input()
{
	size_t connection_count = mempool_count(&iproto_connection_pool);
	size_t request_count = mempool_count(&iproto_msg_pool);
	return request_count > connection_count + IPROTO_MSG_MAX;
}

/**
 * Throttle the queue to the tx thread and ensure the fiber pool
 * in tx thread is not depleted by a flood of incoming requests:
 * resume a stopped connection only if there is a spare message
 * object in the message pool.
 */
static void
iproto_resume()
{
	/*
	 * Most of the time we have nothing to do here: throttling
	 * is not active.
	 */
	if (rlist_empty(&stopped_connections))
		return;
	if (iproto_must_stop_input())
		return;

	struct iproto_connection *con;
	con = rlist_first_entry(&stopped_connections, struct iproto_connection,
				in_stop_list);
	ev_feed_event(con->loop, &con->input, EV_READ);
}

/**
 * A connection is idle when the client is gone
 * and there are no outstanding msgs in the msg queue.
 * An idle connection can be safely garbage collected.
 * Note: a connection only becomes idle after iproto_connection_close(),
 * which closes the fd.  This is why here the check is for
 * evio_has_fd(), not ev_is_active()  (false if event is not
 * started).
 *
 * ibuf_size() provides an effective reference counter
 * on connection use in the tx request queue. Any request
 * in the request queue has a non-zero len, and ibuf_size()
 * is therefore non-zero as long as there is at least
 * one request in the tx queue.
 */
static inline bool
iproto_connection_is_idle(struct iproto_connection *con)
{
	return ibuf_used(&con->ibuf[0]) == 0 && ibuf_used(&con->ibuf[1]) == 0;
}

static inline void
iproto_connection_stop(struct iproto_connection *con)
{
	assert(rlist_empty(&con->in_stop_list));
	ev_io_stop(con->loop, &con->input);
	rlist_add_tail(&stopped_connections, &con->in_stop_list);
}

/**
 * Try to write an iproto error to a socket in the blocking mode.
 * It is useful, when a connection is going to be closed and it is
 * neccessary to response any error information to the user before
 * closing.
 * @param sock Socket to write to.
 * @param error Error to write.
 * @param sync Request sync.
 */
static inline void
iproto_write_error_blocking(int sock, const struct error *e, uint64_t sync)
{
	/* Set to blocking to write the error. */
	int flags = fcntl(sock, F_GETFL, 0);
	if (flags < 0)
		return;
	(void) fcntl(sock, F_SETFL, flags & (~O_NONBLOCK));
	iproto_write_error(sock, e, ::schema_version, sync);
	(void) fcntl(sock, F_SETFL, flags);
}

static void
iproto_connection_on_input(ev_loop * /* loop */, struct ev_io *watcher,
			   int /* revents */);
static void
iproto_connection_on_output(ev_loop * /* loop */, struct ev_io *watcher,
			    int /* revents */);

/** Recycle a connection. Never throws. */
static inline void
iproto_connection_delete(struct iproto_connection *con)
{
	assert(iproto_connection_is_idle(con));
	assert(!evio_has_fd(&con->output));
	assert(!evio_has_fd(&con->input));
	assert(con->session == NULL);
	/*
	 * The output buffers must have been deleted in tx thread.
	 */
	ibuf_destroy(&con->ibuf[0]);
	ibuf_destroy(&con->ibuf[1]);
	/* All iproto_buffers must be returned to tx thread. */
	assert(con->active_obuf == NULL);
	assert(rlist_empty(&con->buffers_to_flush));
	if (con->disconnect)
		iproto_msg_delete(con->disconnect);
	mempool_free(&iproto_connection_pool, con);
}

static void
tx_process_misc(struct cmsg *msg);
static void
tx_process1(struct cmsg *msg);
static void
tx_process_select(struct cmsg *msg);
static void
net_send_msg(struct cmsg *msg);

static void
tx_process_join_subscribe(struct cmsg *msg);
static void
net_end_join_subscribe(struct cmsg *msg);

static void
tx_fiber_init(struct session *session, uint64_t sync)
{
	session->sync = sync;
	/*
	 * We do not cleanup fiber keys at the end of each request.
	 * This does not lead to privilege escalation as long as
	 * fibers used to serve iproto requests never mingle with
	 * fibers used to serve background tasks without going
	 * through the purification of fiber_recycle(), which
	 * resets the fiber local storage. Fibers, used to run
	 * background tasks clean up their session in on_stop
	 * trigger as well.
	 */
	fiber_set_session(fiber(), session);
	fiber_set_user(fiber(), &session->credentials);
}

/**
 * Fire on_disconnect triggers in the tx
 * thread and destroy the session object,
 * as well as output buffers of the connection.
 */
static void
tx_process_disconnect(struct cmsg *m)
{
	struct iproto_msg *msg = (struct iproto_msg *) m;
	struct iproto_connection *con = msg->connection;
	if (con->session) {
		tx_fiber_init(con->session, 0);
		if (! rlist_empty(&session_on_disconnect))
			session_run_on_disconnect_triggers(con->session);
		session_destroy(con->session);
		con->session = NULL; /* safety */
	}
	/*
	 * Got to be done in iproto thread since
	 * that's where the memory is allocated.
	 */
	if (con->active_obuf != NULL) {
		iproto_buffer_delete(con->active_obuf);
		con->active_obuf = NULL;
	}
	iproto_buffer_delete_list(&con->buffers_to_flush);
}

/**
 * Cleanup the net thread resources of a connection
 * and close the connection.
 */
static void
net_finish_disconnect(struct cmsg *m)
{
	struct iproto_msg *msg = (struct iproto_msg *) m;
	/* Runs the trigger, which may yield. */
	iproto_connection_delete(msg->connection);
	iproto_msg_delete(msg);
}

/**
 * Discard the ibuffered data of the iproto msg.
 * @param m iproto_msg.discard_ibuf_msg message.
 */
static inline void
net_gc_ibuf(struct cmsg *m)
{
	struct iproto_msg *msg =
		container_of(m, struct iproto_msg, discard_ibuf_msg);
	/* Discard request (see iproto_enqueue_batch()). */
	msg->ibuf->rpos += msg->len;
	/* Do not discard twice in net_send_msg. */
	msg->len = 0;
	iproto_connection_resume_input(msg->connection);
}

static const struct cmsg_hop disconnect_route[] = {
	{ tx_process_disconnect, &net_pipe },
	{ net_finish_disconnect, NULL },
};

static const struct cmsg_hop misc_route[] = {
	{ tx_process_misc, &net_pipe },
	{ net_send_msg, NULL },
};

static const struct cmsg_hop select_route[] = {
	{ tx_process_select, &net_pipe },
	{ net_send_msg, NULL },
};

static const struct cmsg_hop process1_route[] = {
	{ tx_process1, &net_pipe },
	{ net_send_msg, NULL },
};

static const struct cmsg_hop ibuf_gc_route = { net_gc_ibuf, NULL };

static const struct cmsg_hop *dml_route[IPROTO_TYPE_STAT_MAX] = {
	NULL,                                   /* IPROTO_OK */
	select_route,                           /* IPROTO_SELECT */
	process1_route,                         /* IPROTO_INSERT */
	process1_route,                         /* IPROTO_REPLACE */
	process1_route,                         /* IPROTO_UPDATE */
	process1_route,                         /* IPROTO_DELETE */
	misc_route,                             /* IPROTO_CALL_16 */
	misc_route,                             /* IPROTO_AUTH */
	misc_route,                             /* IPROTO_EVAL */
	process1_route,                         /* IPROTO_UPSERT */
	misc_route                              /* IPROTO_CALL */
};

static const struct cmsg_hop sync_route[] = {
	{ tx_process_join_subscribe, &net_pipe },
	{ net_end_join_subscribe, NULL },
};

static struct iproto_connection *
iproto_connection_new(const char *name, int fd)
{
	(void) name;
	struct iproto_connection *con = (struct iproto_connection *)
		mempool_alloc_xc(&iproto_connection_pool);
	con->input.data = con->output.data = con;
	con->loop = loop();
	ev_io_init(&con->input, iproto_connection_on_input, fd, EV_READ);
	ev_io_init(&con->output, iproto_connection_on_output, fd, EV_WRITE);
	ibuf_create(&con->ibuf[0], cord_slab_cache(), iobuf_readahead);
	ibuf_create(&con->ibuf[1], cord_slab_cache(), iobuf_readahead);
	con->active_ibuf = &con->ibuf[0];
	con->parse_size = 0;
	con->session = NULL;
	con->active_obuf = NULL;
	rlist_create(&con->buffers_to_flush);
	rlist_create(&con->in_stop_list);
	/* It may be very awkward to allocate at close. */
	con->disconnect = iproto_msg_new(con);
	cmsg_init(&con->disconnect->request_msg, disconnect_route);
	return con;
}

/**
 * Global list of not empty iproto buffers to send them to iproto
 * thread. The batch can be changed only by tx thread. Each
 * connection can have only one active obuf during an tx event
 * loop iteration. Tx thread after processing each iproto request
 * searches active obuf of a connection. If it absenses, then
 * it is created and appended to this batch. One time per tx
 * thread's event loop iteration this batch is sent to iproto
 * thread. Then this global variable is set to NULL to start
 * collecting a new batch on next event loop iteration. Tx event
 * loop example:
 *
 *  Create     +       <------<+ <- Write response to a new batch.
 * obuf and -> |               |
 *  write      |     Event     |
 * response    v     Loop      ^ <- Flush buffers_to_iproto to
 *             |               |    iproto thread.
 *  Write      |               |
 * response    +>------>------>+ <- Write response.
 *                  ^
 *                  |
 *                Write
 *               response
 */
static struct rlist *buffers_to_iproto = NULL;
/**
 * List of flushed buffers for tx thread. Only tx thread can
 * utilize these buffers.
 */
static RLIST_HEAD(buffers_to_tx);

/**
 * Get output buffer used by a specified iproto connection. The
 * result buffer can be used until a next yield. Entire response
 * must be wrote before yield, because after yield the buffer
 * could be sent to iproto thread for flushing.
 * @param conn Connection to get obuf.
 *
 * @retval not NULL Output buffer, valid until yield.
 * @retval     NULL Memory error.
 */
static struct obuf *
iproto_connection_active_obuf(struct iproto_connection *conn)
{
	assert(cord() == tx_cord);
	if (conn->active_obuf == NULL) {
		conn->active_obuf = iproto_buffer_new(conn);
		if (conn->active_obuf == NULL)
			return NULL;
	}
	return &conn->active_obuf->obuf;
}

extern "C" struct obuf *
call_request_obuf(struct call_request *request)
{
	struct iproto_msg *msg =
		container_of(request, struct iproto_msg, call_request);
	return iproto_connection_active_obuf(msg->connection);
}

/**
 * Send message to iproto thread to discard input of a specified
 * call request. @sa net_gc_ibuf().
 * @param call_request Call request, pointer to
 *        iproto_msg.call_request.
 */
extern "C" void
call_request_discard_input(struct call_request *request)
{
#ifndef NDEBUG
	struct errinj *inj = errinj(ERRINJ_DELAY_NET_GC_IBUF, ERRINJ_BOOL);
	if (inj != NULL)
		while(inj->bparam) fiber_sleep(0.00001);
#endif
	struct iproto_msg *msg =
		container_of(request, struct iproto_msg, call_request);
	cmsg_init(&msg->discard_ibuf_msg, &ibuf_gc_route);
	cpipe_push(&net_pipe, &msg->discard_ibuf_msg);
	cpipe_flush_input(&net_pipe);
#ifndef NDEBUG
	/* The following data is discarded in iproto thread. */
	request->name = NULL;
	request->expr = NULL;
	request->args = NULL;
	request->args_end = NULL;
#endif
}

/**
 * Reset active obufs of all connections which wrote someting
 * during the current tx event loop. And reset global list of
 * buffers to send to iproto thread.
 */
static void
tx_reset_batch()
{
	assert(cord() == tx_cord);
	struct iproto_buffer *next;
	rlist_foreach_entry(next, buffers_to_iproto, in_batch) {
		assert(next == next->connection->active_obuf);
		next->connection->active_obuf = NULL;
	}
	buffers_to_iproto = NULL;
}

/**
 * Initiate a connection shutdown. This method may
 * be invoked many times, and does the internal
 * bookkeeping to only cleanup resources once.
 */
static inline void
iproto_connection_close(struct iproto_connection *con)
{
	if (evio_has_fd(&con->input)) {
		/* Clears all pending events. */
		ev_io_stop(con->loop, &con->input);
		ev_io_stop(con->loop, &con->output);

		int fd = con->input.fd;
		/* Make evio_has_fd() happy */
		con->input.fd = con->output.fd = -1;
		close(fd);
		/*
		 * Discard unparsed data, to recycle the
		 * connection in net_send_msg() as soon as all
		 * parsed data is processed.  It's important this
		 * is done only once.
		 */
		con->active_ibuf->wpos -= con->parse_size;
	}
	/*
	 * If the connection has no outstanding requests in the
	 * input buffer, then no one (e.g. tx thread) is referring
	 * to it, so it must be destroyed at once. Queue a msg to
	 * run on_disconnect() trigger and destroy the connection.
	 *
	 * Otherwise, it will be destroyed by the last request on
	 * this connection that has finished processing.
	 *
	 * The check is mandatory to not destroy a connection
	 * twice.
	 */
	if (iproto_connection_is_idle(con)) {
		assert(con->disconnect != NULL);
		struct iproto_msg *msg = con->disconnect;
		con->disconnect = NULL;
		cpipe_push(&tx_pipe, &msg->request_msg);
	}
	rlist_del(&con->in_stop_list);
}

/**
 * If there is no space for reading input, we can do one of the
 * following:
 * - try to get a new ibuf, so that it can fit the request.
 *   Always getting a new input buffer when there is no space
 *   makes the instance susceptible to input-flood attacks.
 *   Therefore, at most 2 iobufs are used in a single connection,
 *   one is "open", receiving input, and the  other is closed,
 *   flushing output.
 * - stop input and wait until the client reads piled up output,
 *   so the input buffer can be reused. This complements
 *   the previous strategy. It is only safe to stop input if it
 *   is known that there is output. In this case input event
 *   flow will be resumed when all replies to previous requests
 *   are sent. Since there are two buffers, the input is only
 *   stopped when both of them are fully used up.
 *
 * To make this strategy work, each ibuf in use must fit at
 * least one request. Otherwise, ibuf != active_ibuf may end
 * up having no data to flush, while active_ibuf is too small to
 * fit a big incoming request.
 */
static struct ibuf *
iproto_connection_input_ibuf(struct iproto_connection *con)
{
	struct ibuf *oldbuf = con->active_ibuf;

	size_t to_read = 3; /* Smallest possible valid request. */

	/* The type code is checked in iproto_enqueue_batch() */
	if (con->parse_size) {
		const char *pos = oldbuf->wpos - con->parse_size;
		if (mp_check_uint(pos, oldbuf->wpos) <= 0)
			to_read = mp_decode_uint(&pos);
	}

	/**
	 * Reuse the buffer if:
	 * - it has enough space to fit new data;
	 * - it contains only unparsed data. In such a case it
	 *   must fit a new request, else the request can not be
	 *   read in second buffer too.
	 */
	if (ibuf_unused(oldbuf) >= to_read)
		return oldbuf;
	if (oldbuf->wpos == oldbuf->buf + con->parse_size) {
		ibuf_reserve_xc(oldbuf, to_read);
		return oldbuf;
	}

	struct ibuf *newbuf = iproto_connection_next_ibuf(con);
	if (ibuf_used(newbuf) != 0) {
		/*
		 * Wait until the second buffer is flushed
		 * and becomes available for reuse.
		 */
		return NULL;
	}

	ibuf_reserve_xc(newbuf, to_read + con->parse_size);
	/*
	 * Discard unparsed data in the old buffer, otherwise it
	 * won't be recycled when all parsed requests are processed.
	 */
	oldbuf->wpos -= con->parse_size;
	if (con->parse_size != 0) {
		/* Move the cached request prefix to the new buffer. */
		memcpy(newbuf->rpos, oldbuf->wpos, con->parse_size);
		newbuf->wpos += con->parse_size;
		/*
		 * We made ibuf idle. If obuf was already idle it makes the whole
		 * iobuf idle, time to trim buffers.
		 */
		if (ibuf_used(oldbuf) == 0)
			ibuf_reset_mt(oldbuf);
	}
	/* Rotate ibufs. */
	con->active_ibuf = newbuf;
	return newbuf;
}

static void
iproto_decode_msg(struct iproto_msg *msg, const char **pos, const char *reqend,
		  bool *stop_input)
{
	xrow_header_decode_xc(&msg->header, pos, reqend);
	assert(*pos == reqend);
	uint8_t type = msg->header.type;

	/*
	 * Parse request before putting it into the queue
	 * to save tx some CPU. More complicated requests are
	 * parsed in tx thread into request type-specific objects.
	 */
	switch (type) {
	case IPROTO_SELECT:
	case IPROTO_INSERT:
	case IPROTO_REPLACE:
	case IPROTO_UPDATE:
	case IPROTO_DELETE:
	case IPROTO_UPSERT:
		xrow_decode_dml_xc(&msg->header, &msg->dml_request,
				   dml_request_key_map(type));
		assert(type < sizeof(dml_route)/sizeof(*dml_route));
		cmsg_init(&msg->request_msg, dml_route[type]);
		break;
	case IPROTO_CALL_16:
	case IPROTO_CALL:
	case IPROTO_EVAL:
		xrow_decode_call_xc(&msg->header, &msg->call_request);
		cmsg_init(&msg->request_msg, misc_route);
		break;
	case IPROTO_PING:
		cmsg_init(&msg->request_msg, misc_route);
		break;
	case IPROTO_JOIN:
	case IPROTO_SUBSCRIBE:
		cmsg_init(&msg->request_msg, sync_route);
		*stop_input = true;
		break;
	case IPROTO_AUTH:
		xrow_decode_auth_xc(&msg->header, &msg->auth_request);
		cmsg_init(&msg->request_msg, misc_route);
		break;
	default:
		tnt_raise(ClientError, ER_UNKNOWN_REQUEST_TYPE,
			  (uint32_t) type);
		break;
	}
	return;
}

/** Enqueue all requests which were read up. */
static inline void
iproto_enqueue_batch(struct iproto_connection *con, struct ibuf *in)
{
	int n_requests = 0;
	bool stop_input = false;
	struct iproto_msg *first_msg = NULL;
	while (con->parse_size && stop_input == false) {
		const char *reqstart = in->wpos - con->parse_size;
		const char *pos = reqstart;
		/* Read request length. */
		if (mp_typeof(*pos) != MP_UINT) {
			tnt_raise(ClientError, ER_INVALID_MSGPACK,
				  "packet length");
		}
		if (mp_check_uint(pos, in->wpos) >= 0)
			break;
		uint32_t len = mp_decode_uint(&pos);
		const char *reqend = pos + len;
		if (reqend > in->wpos)
			break;
		struct iproto_msg *msg = iproto_msg_new(con);
		msg->ibuf = in;
		auto guard = make_scoped_guard([=] { iproto_msg_delete(msg); });

		msg->len = reqend - reqstart; /* total request length */

		try {
			iproto_decode_msg(msg, &pos, reqend, &stop_input);
			/*
			 * This can't throw, but should not be
			 * done in case of exception.
			 */
			cpipe_push_input(&tx_pipe, &msg->request_msg);
			guard.is_active = false;
			n_requests++;
			if (first_msg == NULL)
				first_msg = msg;
		} catch (Exception *e) {
			/*
			 * Advance read position right away: the
			 * message is so no need to hold the input
			 * buffer.
			 */
			in->rpos += msg->len;
			e->log();
			iproto_write_error_blocking(con->input.fd, e,
						    msg->header.sync);
		}

		/* Request is parsed */
		assert(reqend > reqstart);
		assert(con->parse_size >= (size_t) (reqend - reqstart));
		/*
		 * sic: in case of exception con->parse_size
		 * must not be advanced to stay in sync with
		 * in->rpos.
		 */
		con->parse_size -= reqend - reqstart;
	}
	if (stop_input) {
		/**
		 * Don't mess with the file descriptor
		 * while join is running. ev_io_stop()
		 * also clears any pending events, which
		 * is good, since their invocation may
		 * re-start the watcher, ruining our
		 * efforts.
		 */
		ev_io_stop(con->loop, &con->output);
		ev_io_stop(con->loop, &con->input);
	} else if (n_requests != 1 || con->parse_size != 0) {
		assert(rlist_empty(&con->in_stop_list));
		/*
		 * Keep reading input, as long as the socket
		 * supplies data, but don't waste CPU on an extra
		 * read() if dealing with a blocking client, it
		 * has nothing in the socket for us.
		 *
		 * We look at the amount of enqueued requests
		 * and presence of a partial request in the
		 * input buffer as hints to distinguish
		 * blocking and non-blocking clients:
		 *
		 * For blocking clients, a request typically
		 * is fully read and enqueued.
		 * If there is unparsed data, or 0 queued
		 * requests, keep reading input, if only to avoid
		 * a deadlock on this connection.
		 */
		ev_feed_event(con->loop, &con->input, EV_READ);
	}
	/*
	 * Send flushed buffers to tx thread with the first
	 * client request of the batch.
	 */
	if (first_msg != NULL)
		rlist_splice(&first_msg->flushed_buffers, &buffers_to_tx);
	cpipe_flush_input(&tx_pipe);
}

static void
iproto_connection_on_input(ev_loop *loop, struct ev_io *watcher,
			   int /* revents */)
{
	struct iproto_connection *con =
		(struct iproto_connection *) watcher->data;
	int fd = con->input.fd;
	assert(fd >= 0);
	if (! rlist_empty(&con->in_stop_list)) {
		/* Resumed stopped connection. */
		rlist_del(&con->in_stop_list);
		/*
		 * This connection may have no input, so
		 * resume one more connection which might have
		 * input.
		 */
		iproto_resume();
	}
	/*
	 * Throttle if there are too many pending requests,
	 * otherwise we might deplete the fiber pool and
	 * deadlock (e.g. WAL writer needs a fiber to wake
	 * another fiber waiting for write to complete).
	 * Ignore iproto_connection->disconnect messages.
	 */
	if (iproto_must_stop_input()) {
		iproto_connection_stop(con);
		return;
	}

	try {
		/* Ensure we have sufficient space for the next round.  */
		struct ibuf *in = iproto_connection_input_ibuf(con);
		if (in == NULL) {
			ev_io_stop(loop, &con->input);
			return;
		}
		/* Read input. */
		int nrd = sio_read(fd, in->wpos, ibuf_unused(in));
		if (nrd < 0) {                  /* Socket is not ready. */
			ev_io_start(loop, &con->input);
			return;
		}
		if (nrd == 0) {                 /* EOF */
			iproto_connection_close(con);
			return;
		}
		/* Count statistics */
		rmean_collect(rmean_net, IPROTO_RECEIVED, nrd);

		/* Update the read position and connection state. */
		in->wpos += nrd;
		con->parse_size += nrd;
		/* Enqueue all requests which are fully read up. */
		iproto_enqueue_batch(con, in);
	} catch (Exception *e) {
		/* Best effort at sending the error message to the client. */
		iproto_write_error_blocking(fd, e, 0);
		e->log();
		iproto_connection_close(con);
	}
}

/** writev() to the socket and handle the result. */
static int
iproto_flush(struct obuf *obuf, struct iproto_connection *con)
{
	struct obuf_svp *begin = &obuf->wpos;
	struct obuf_svp *end = &obuf->wend;
	assert(begin->used < end->used);
	struct iovec iov[SMALL_OBUF_IOV_MAX+1];
	int iovcnt = end->pos - begin->pos + 1;
	memcpy(iov, obuf->iov + begin->pos, iovcnt * sizeof(struct iovec));
	sio_add_to_iov(iov, -begin->iov_len);

#ifndef NDEBUG
	struct errinj *inj = errinj(ERRINJ_SIOWRITEV_PARTIAL, ERRINJ_BOOL);
	size_t old_len = 0;
	if (inj != NULL && inj->bparam) {
		old_len = iov[0].iov_len;
		if (iovcnt > 1)
			iovcnt -= 1;
		else
			iov[0].iov_len /= 2;
       }
#endif
	ssize_t nwr = sio_writev(con->output.fd, iov, iovcnt);
#ifndef NDEBUG
	if (old_len != 0 && iovcnt == 1)
		iov[0].iov_len = old_len;
#endif

	/* Count statistics */
	rmean_collect(rmean_net, IPROTO_SENT, nwr);
	if (nwr > 0) {
		if (begin->used + nwr == end->used) {
			/* Advance write position. */
			*begin = *end;
			return 0;
		}
		size_t offset = 0;
		int advance = 0;
		advance = sio_move_iov(iov, nwr, &offset);
		begin->used += nwr;             /* advance write position */
		begin->iov_len = advance == 0 ? begin->iov_len + offset: offset;
		begin->pos += advance;
		assert(begin->pos <= end->pos);
	}
	return -1;
}

static void
iproto_connection_on_output(ev_loop *loop, struct ev_io *watcher,
			    int /* revents */)
{
	struct iproto_connection *con = (struct iproto_connection *) watcher->data;

	try {
		struct iproto_buffer *next, *tmp;
		rlist_foreach_entry_safe(next, &con->buffers_to_flush,
					 in_batch, tmp) {
			if (iproto_flush(&next->obuf, con) != 0) {
#ifndef NDEBUG
				struct errinj *inj =
					errinj(ERRINJ_SIOWRITEV_PARTIAL,
					       ERRINJ_BOOL);
				if (inj != NULL && inj->bparam) {
					inj->bparam = false;
					break;
				}
#endif
				ev_io_start(loop, &con->output);
				return;
			}
			/*
			 * Move flushed buffers to the global list
			 * to send them back to tx with a next
			 * client request.
			 */
			rlist_move_tail_entry(&buffers_to_tx, next, in_batch);
		}
		iproto_connection_resume_input(con);
		if (ev_is_active(&con->output))
			ev_io_stop(con->loop, &con->output);
	} catch (Exception *e) {
		e->log();
		iproto_connection_close(con);
	}
}

/**
 * Spread buffers received from tx to their connection buffer
 * queues.
 */
static void
net_flush_obufs(struct rlist *batch)
{
	struct iproto_buffer *next, *tmp;
	rlist_foreach_entry_safe(next, batch, in_batch, tmp) {
		struct iproto_connection *conn = next->connection;
		rlist_move_tail_entry(&conn->buffers_to_flush, next, in_batch);
		if (evio_has_fd(&conn->output) && !ev_is_active(&conn->output))
			ev_feed_event(conn->loop, &conn->output, EV_WRITE);
	}
}

/**
 * Wait end of a current event loop iteration and prepare obufs
 * batch for sending to tx thread.
 */
static inline void
tx_schedule_batch_dump()
{
	assert(cord() == tx_cord);
	assert(buffers_to_iproto != NULL);
	fiber_reschedule();
	tx_reset_batch();
}

/**
 * Update end position of the active obuf of a current connection.
 * Current connection is defined in @a request->connection.
 * If the current obuf is first in a batch of a current event loop
 * iteration, then save it as head of a batch and wait until
 * event loop iteration end. At the end prepare current batch to
 * be sent to iproto.
 * @param request Request to get connection. If the obuf is first
 *        in the batch, then @a request becames a transport for
 *        the batch and stores its head in
 *        iproto_msg.buffers_to_flush. This way allows to avoid
 *        sending a special message to iproto thread with the
 *        batch. We simply use existing message to deliver the
 *        batch.
 */
static inline void
tx_finish_response(struct iproto_msg *request)
{
	assert(cord() == tx_cord);
	struct iproto_buffer *new_buffer = request->connection->active_obuf;
	assert(new_buffer != NULL);
	new_buffer->obuf.wend = obuf_create_svp(&new_buffer->obuf);
	if (buffers_to_iproto == NULL) {
		buffers_to_iproto = &request->buffers_to_flush;
		rlist_add_tail_entry(buffers_to_iproto, new_buffer, in_batch);
		tx_schedule_batch_dump();
		return;
	}
	if (rlist_empty(&new_buffer->in_batch))
		rlist_add_tail_entry(buffers_to_iproto, new_buffer, in_batch);
}

static int
tx_check_schema(uint32_t new_schema_version)
{
	if (new_schema_version && new_schema_version != schema_version) {
		diag_set(ClientError, ER_WRONG_SCHEMA_VERSION,
			 new_schema_version, schema_version);
		return -1;
	}
	return 0;
}

static void
tx_process1(struct cmsg *m)
{
	struct iproto_msg *msg = (struct iproto_msg *) m;
	struct obuf *out;
	tx_fiber_init(msg->connection->session, msg->header.sync);
	iproto_buffer_delete_list(&msg->flushed_buffers);

	if (tx_check_schema(msg->header.schema_version))
		goto error;

	struct tuple *tuple;
	struct obuf_svp svp;
	if (box_process1(&msg->dml_request, &tuple) != 0)
		goto error;
	out = iproto_connection_active_obuf(msg->connection);
	if (out == NULL)
		return;
	if (iproto_prepare_select(out, &svp) != 0)
		goto error;
	if (tuple && tuple_to_obuf(tuple, out)) {
		obuf_rollback_to_svp(out, &svp);
		goto error;
	}
	iproto_reply_select(out, &svp, msg->header.sync, ::schema_version,
			    tuple != 0);
	tx_finish_response(msg);
	return;
error:
	out = iproto_connection_active_obuf(msg->connection);
	if (out == NULL)
		return;
	iproto_reply_error(out, diag_last_error(&fiber()->diag),
			   msg->header.sync, ::schema_version);
	tx_finish_response(msg);
}

static void
tx_process_select(struct cmsg *m)
{
	struct iproto_msg *msg = (struct iproto_msg *) m;
	struct obuf *out;
	struct obuf_svp svp;
	struct port port;
	struct request *req = &msg->dml_request;
	tx_fiber_init(msg->connection->session, msg->header.sync);
	iproto_buffer_delete_list(&msg->flushed_buffers);

	port_create(&port);
	auto port_guard = make_scoped_guard([&](){ port_destroy(&port); });

	if (tx_check_schema(msg->header.schema_version))
		goto error;

	if (box_select(&port, req->space_id, req->index_id, req->iterator,
		       req->offset, req->limit, req->key, req->key_end) != 0)
		goto error;
	out = iproto_connection_active_obuf(msg->connection);
	if (out == NULL)
		return;
	if (iproto_prepare_select(out, &svp) != 0)
		goto error;
	if (port_dump(&port, out) != 0) {
		/* Discard the prepared select. */
		obuf_rollback_to_svp(out, &svp);
		goto error;
	}
	iproto_reply_select(out, &svp, msg->header.sync, ::schema_version,
			    port.size);
	tx_finish_response(msg);
	return;
error:
	out = iproto_connection_active_obuf(msg->connection);
	if (out == NULL)
		return;
	iproto_reply_error(out, diag_last_error(&fiber()->diag),
			   msg->header.sync, ::schema_version);
	tx_finish_response(msg);
}

static void
tx_process_misc(struct cmsg *m)
{
	struct iproto_msg *msg = (struct iproto_msg *) m;
	struct iproto_connection *conn = msg->connection;
	struct obuf *out;
	tx_fiber_init(msg->connection->session, msg->header.sync);
	iproto_buffer_delete_list(&msg->flushed_buffers);

	if (tx_check_schema(msg->header.schema_version))
		goto error;

	try {
		switch (msg->header.type) {
		case IPROTO_CALL:
		case IPROTO_CALL_16:
			box_process_call(&msg->call_request);
			break;
		case IPROTO_EVAL:
			box_process_eval(&msg->call_request);
			break;
		case IPROTO_AUTH:
			out = iproto_connection_active_obuf(conn);
			if (out == NULL)
				return;
			box_process_auth(&msg->auth_request, out);
			break;
		case IPROTO_PING:
			out = iproto_connection_active_obuf(conn);
			if (out == NULL)
				return;
			iproto_reply_ok_xc(out, msg->header.sync,
					   ::schema_version);
			break;
		default:
			unreachable();
		}
	} catch (Exception *e) {
		goto error;
	}
	tx_finish_response(msg);
	return;
error:
	out = iproto_connection_active_obuf(conn);
	if (out == NULL)
		return;
	iproto_reply_error(out, diag_last_error(&fiber()->diag),
			   msg->header.sync, ::schema_version);
	tx_finish_response(msg);
}

static void
tx_process_join_subscribe(struct cmsg *m)
{
	struct iproto_msg *msg = (struct iproto_msg *) m;
	struct iproto_connection *con = msg->connection;
	iproto_buffer_delete_list(&msg->flushed_buffers);

	tx_fiber_init(con->session, msg->header.sync);

	try {
		switch (msg->header.type) {
		case IPROTO_JOIN:
			/*
			 * As soon as box_process_subscribe() returns
			 * the lambda in the beginning of the block
			 * will re-activate the watchers for us.
			 */
			box_process_join(&con->input, &msg->header);
			break;
		case IPROTO_SUBSCRIBE:
			/*
			 * Subscribe never returns - unless there
			 * is an error/exception. In that case
			 * the write watcher will be re-activated
			 * the same way as for JOIN.
			 */
			box_process_subscribe(&con->input, &msg->header);
			break;
		default:
			unreachable();
		}
	} catch (SocketError *e) {
		throw; /* don't write error response to prevent SIGPIPE */
	} catch (Exception *e) {
		iproto_write_error_blocking(con->input.fd, e, msg->header.sync);
	}
}

static void
net_send_msg(struct cmsg *m)
{
	struct iproto_msg *msg = (struct iproto_msg *) m;
	struct iproto_connection *con = msg->connection;
	struct ibuf *ibuf = msg->ibuf;
	/* Discard request (see iproto_enqueue_batch()) */
	ibuf->rpos += msg->len;
	net_flush_obufs(&msg->buffers_to_flush);

	if (!evio_has_fd(&con->output) && iproto_connection_is_idle(con))
		iproto_connection_close(con);
	iproto_connection_resume_input(con);
	iproto_msg_delete(msg);
}

static void
net_end_join_subscribe(struct cmsg *m)
{
	struct iproto_msg *msg = (struct iproto_msg *) m;
	struct iproto_connection *con = msg->connection;
	struct ibuf *ibuf = msg->ibuf;

	ibuf->rpos += msg->len;
	iproto_msg_delete(msg);

	assert(! ev_is_active(&con->input));
	/*
	 * Enqueue any messages if they are in the readahead
	 * queue. Will simply start input otherwise.
	 */
	iproto_enqueue_batch(con, ibuf);
}

/**
 * Handshake a connection: invoke the on-connect trigger
 * and possibly authenticate. Try to send the client an error
 * upon a failure.
 */
static void
tx_process_connect(struct cmsg *m)
{
	struct iproto_msg *msg = (struct iproto_msg *) m;
	struct iproto_connection *con = msg->connection;
	struct obuf *out = iproto_connection_active_obuf(con);
	if (out == NULL)
		return;
	try {              /* connect. */
		con->session = session_create(con->input.fd, SESSION_TYPE_BINARY);
		if (con->session == NULL)
			diag_raise();
		tx_fiber_init(con->session, 0);
		char greeting[IPROTO_GREETING_SIZE];
		/* TODO: dirty read from tx thread */
		struct tt_uuid uuid = INSTANCE_UUID;
		greeting_encode(greeting, tarantool_version_id(),
				&uuid, con->session->salt, SESSION_SEED_SIZE);
		obuf_dup_xc(out, greeting, IPROTO_GREETING_SIZE);
		if (! rlist_empty(&session_on_connect)) {
			if (session_run_on_connect_triggers(con->session) != 0)
				diag_raise();
		}
		con->active_obuf->obuf.wend =
			obuf_create_svp(&con->active_obuf->obuf);
	} catch (Exception *e) {
		/* zero sync for connect errors */
		iproto_reply_error(out, e, 0, ::schema_version);
		msg->close_connection = true;
	}
}

/**
 * Send a response to connect to the client or close the
 * connection in case on_connect trigger failed.
 */
static void
net_send_greeting(struct cmsg *m)
{
	struct iproto_msg *msg = (struct iproto_msg *) m;
	struct iproto_connection *con = msg->connection;
	struct obuf *out = &con->active_obuf->obuf;
	if (msg->close_connection) {
		try {
			int64_t nwr = sio_writev(con->output.fd, out->iov,
						 obuf_iovcnt(out));

			/* Count statistics */
			rmean_collect(rmean_net, IPROTO_SENT, nwr);
		} catch (Exception *e) {
			e->log();
		}
		assert(iproto_connection_is_idle(con));
		iproto_connection_close(con);
		iproto_msg_delete(msg);
		return;
	}
	rlist_move_tail_entry(&con->buffers_to_flush,
			      con->active_obuf, in_batch);
	con->active_obuf = NULL;
	/*
	 * Connect is synchronous, so no one could have been
	 * messing up with the connection while it was in
	 * progress.
	 */
	assert(evio_has_fd(&con->output));
	/* Handshake OK, start reading input. */
	ev_feed_event(con->loop, &con->output, EV_WRITE);
	iproto_msg_delete(msg);
}

static const struct cmsg_hop connect_route[] = {
	{ tx_process_connect, &net_pipe },
	{ net_send_greeting, NULL },
};

/** }}} */

/**
 * Create a connection and start input.
 */
static void
iproto_on_accept(struct evio_service * /* service */, int fd,
		 struct sockaddr *addr, socklen_t addrlen)
{
	char name[SERVICE_NAME_MAXLEN];
	snprintf(name, sizeof(name), "%s/%s", "iobuf",
		sio_strfaddr(addr, addrlen));

	struct iproto_connection *con;

	con = iproto_connection_new(name, fd);
	/*
	 * Ignore msg allocation failure - the queue size is
	 * fixed so there is a limited number of msgs in
	 * use, all stored in just a few blocks of the memory pool.
	 */
	struct iproto_msg *msg = iproto_msg_new(con);
	cmsg_init(&msg->request_msg, connect_route);
	msg->ibuf = con->active_ibuf;
	msg->close_connection = false;
	cpipe_push(&tx_pipe, &msg->request_msg);
}

static struct evio_service binary; /* iproto binary listener */

/**
 * The network io thread main function:
 * begin serving the message bus.
 */
static int
net_cord_f(va_list /* ap */)
{
	/* Got to be called in every thread using iobuf */
	mempool_create(&iproto_msg_pool, &cord()->slabc,
		       sizeof(struct iproto_msg));
	mempool_create(&iproto_connection_pool, &cord()->slabc,
		       sizeof(struct iproto_connection));

	evio_service_init(loop(), &binary, "binary",
			  iproto_on_accept, NULL);


	/* Init statistics counter */
	rmean_net = rmean_new(rmean_net_strings, IPROTO_LAST);

	if (rmean_net == NULL) {
		tnt_raise(OutOfMemory, sizeof(struct rmean),
			  "rmean", "struct rmean");
	}

	struct cbus_endpoint endpoint;
	/* Create "net" endpoint. */
	cbus_endpoint_create(&endpoint, "net", fiber_schedule_cb, fiber());
	/* Create a pipe to "tx" thread. */
	cpipe_create(&tx_pipe, "tx");
	cpipe_set_max_input(&tx_pipe, IPROTO_MSG_MAX/2);
	/* Process incomming messages. */
	cbus_loop(&endpoint);

	cpipe_destroy(&tx_pipe);
	/*
	 * Nothing to do in the fiber so far, the service
	 * will take care of creating events for incoming
	 * connections.
	 */
	if (evio_service_is_active(&binary))
		evio_service_stop(&binary);

	rmean_delete(rmean_net);
	return 0;
}

/** Initialize the iproto subsystem and start network io thread */
void
iproto_init()
{
	tx_cord = cord();

	static struct cord net_cord;
	if (cord_costart(&net_cord, "iproto", net_cord_f, NULL))
		panic("failed to initialize iproto thread");

	/* Create a pipe to "net" thread. */
	cpipe_create(&net_pipe, "net");
	cpipe_set_max_input(&net_pipe, IPROTO_MSG_MAX/2);
}

/**
 * Since there is no way to "synchronously" change the
 * state of the io thread, to change the listen port
 * we need to bounce a couple of messages to and
 * from this thread.
 */
struct iproto_bind_msg: public cbus_call_msg
{
	const char *uri;
};

static int
iproto_do_bind(struct cbus_call_msg *m)
{
	const char *uri  = ((struct iproto_bind_msg *) m)->uri;
	try {
		if (evio_service_is_active(&binary))
			evio_service_stop(&binary);
		if (uri != NULL)
			evio_service_bind(&binary, uri);
	} catch (Exception *e) {
		return -1;
	}
	return 0;
}

static int
iproto_do_listen(struct cbus_call_msg *m)
{
	(void) m;
	try {
		if (evio_service_is_active(&binary))
			evio_service_listen(&binary);
	} catch (Exception *e) {
		return -1;
	}
	return 0;
}

void
iproto_bind(const char *uri)
{
	static struct iproto_bind_msg m;
	m.uri = uri;
	if (cbus_call(&net_pipe, &tx_pipe, &m, iproto_do_bind,
		      NULL, TIMEOUT_INFINITY))
		diag_raise();
}

void
iproto_listen()
{
	/* Declare static to avoid stack corruption on fiber cancel. */
	static struct cbus_call_msg m;
	if (cbus_call(&net_pipe, &tx_pipe, &m, iproto_do_listen,
		      NULL, TIMEOUT_INFINITY))
		diag_raise();
}

/* vim: set foldmethod=marker */
