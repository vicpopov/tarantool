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
#include "relay.h"

#include "trivia/config.h"
#include "trivia/util.h"
#include "cbus.h"
#include "cfg.h"
#include "errinj.h"
#include "fiber.h"
#include "say.h"

#include "coio.h"
#include "coio_task.h"
#include "engine.h"
#include "gc.h"
#include "iproto_constants.h"
#include "recovery.h"
#include "replication.h"
#include "trigger.h"
#include "vclock.h"
#include "xrow.h"
#include "xrow_io.h"
#include "xstream.h"
#include "wal.h"

/** Report relay status to tx thread at least once per this interval */
static const int RELAY_REPORT_INTERVAL = 1;

/**
 * Cbus message to send status updates from relay to tx thread.
 */
struct relay_status_msg {
	/** Parent */
	struct cmsg msg;
	/** Relay instance */
	struct relay *relay;
	/** New sent vclock. */
	struct vclock sent_vclock;
	/** New recieved vclock. */
	struct vclock recv_vclock;
};

/**
 * Cbus message to update replica gc state in tx thread.
 */
struct relay_gc_msg {
	/** Parent */
	struct cmsg msg;
	/** Relay instance */
	struct relay *relay;
	/** Vclock signature to advance to */
	int64_t signature;
};

/** State of a replication relay. */
struct relay {
	/** The thread in which we relay data to the replica. */
	struct cord cord;
	/** Replica connection */
	struct ev_io io;
	/** Request sync */
	uint64_t sync;
	/** Recovery instance to read xlog from the disk */
	struct recovery *r;
	/** Xstream argument to recovery */
	struct xstream stream;
	/** Vclock to stop playing xlogs */
	struct vclock stop_vclock;
	/** Remote replica */
	struct replica *replica;
	/** WAL event watcher. */
	struct wal_watcher wal_watcher;
	/** Set before exiting the relay loop. */
	bool exiting;
	/** Set on relay error. */
	bool failed;
	/** Relay reader cond. */
	struct fiber_cond reader_cond;
	/** Diag for reader fiber. */
	struct diag reader_diag;
	/** Vclock recieved from replica. */
	struct vclock recv_vclock;

	/** Relay endpoint */
	struct cbus_endpoint endpoint;
	/** A pipe from 'relay' thread to 'tx' */
	struct cpipe tx_pipe;
	/** A pipe from 'tx' thread to 'relay' */
	struct cpipe relay_pipe;
	/** Status message */
	struct relay_status_msg status_msg;

	struct {
		/* Align to prevent false-sharing with tx thread */
		alignas(CACHELINE_SIZE)
		/** Current vclock sent by relay */
		struct vclock sent_vclock;
		/** Current vclock recieved from replica */
		struct vclock recv_vclock;
	} tx;
};

const struct vclock *
relay_vclock(const struct relay *relay)
{
	if (vclock_sum(&relay->tx.recv_vclock) != 0)
		return &relay->tx.recv_vclock;
	return &relay->tx.sent_vclock;
}

static void
relay_send_initial_join_row(struct xstream *stream, struct xrow_header *row);
static void
relay_send_row(struct xstream *stream, struct xrow_header *row);

static inline void
relay_init(struct relay *relay, int fd, uint64_t sync,
	   void (*stream_write)(struct xstream *, struct xrow_header *))
{
	memset(relay, 0, sizeof(*relay));
	xstream_create(&relay->stream, stream_write);
	coio_create(&relay->io, fd);
	relay->sync = sync;
	fiber_cond_create(&relay->reader_cond);
	diag_create(&relay->reader_diag);
}

static inline void
relay_set_cord_name(int fd)
{
	char name[FIBER_NAME_MAX];
	struct sockaddr_storage peer;
	socklen_t addrlen = sizeof(peer);
	if (getpeername(fd, ((struct sockaddr*)&peer), &addrlen) == 0) {
		snprintf(name, sizeof(name), "relay/%s",
			 sio_strfaddr((struct sockaddr *)&peer, addrlen));
	} else {
		snprintf(name, sizeof(name), "relay/<unknown>");
	}
	cord_set_name(name);
}

void
relay_initial_join(int fd, uint64_t sync, struct vclock *vclock)
{
	struct relay relay;
	relay_init(&relay, fd, sync, relay_send_initial_join_row);
	assert(relay.stream.write != NULL);
	engine_join(vclock, &relay.stream);
}

int
relay_final_join_f(va_list ap)
{
	struct relay *relay = va_arg(ap, struct relay *);
	coio_enable();
	relay_set_cord_name(relay->io.fd);

	/* Send all WALs until stop_vclock */
	assert(relay->stream.write != NULL);
	recover_remaining_wals(relay->r, &relay->stream,
			       &relay->stop_vclock, true);
	assert(vclock_compare(&relay->r->vclock, &relay->stop_vclock) == 0);
	return 0;
}

void
relay_final_join(int fd, uint64_t sync, struct vclock *start_vclock,
	         struct vclock *stop_vclock)
{
	struct relay relay;
	relay_init(&relay, fd, sync, relay_send_row);
	relay.r = recovery_new(cfg_gets("wal_dir"),
			       cfg_geti("force_recovery"),
			       start_vclock);
	vclock_copy(&relay.stop_vclock, stop_vclock);

	int rc = cord_costart(&relay.cord, "final_join",
			      relay_final_join_f, &relay);
	if (rc == 0)
		rc = cord_cojoin(&relay.cord);

	recovery_delete(relay.r);

	if (rc != 0)
		diag_raise();

	ERROR_INJECT(ERRINJ_RELAY_FINAL_SLEEP, {
		while (vclock_compare(stop_vclock, &replicaset_vclock) == 0)
			fiber_sleep(0.001);
	});
}

/**
 * The message which updated tx thread with a new vclock has returned back
 * to the relay.
 */
static void
relay_status_update(struct cmsg *msg)
{
	msg->route = NULL;
}

/**
 * Deliver a fresh relay vclock to tx thread.
 */
static void
tx_status_update(struct cmsg *msg)
{
	struct relay_status_msg *status = (struct relay_status_msg *)msg;
	vclock_copy(&status->relay->tx.sent_vclock, &status->sent_vclock);
	vclock_copy(&status->relay->tx.recv_vclock, &status->recv_vclock);
	static const struct cmsg_hop route[] = {
		{relay_status_update, NULL}
	};
	cmsg_init(msg, route);
	cpipe_push(&status->relay->relay_pipe, msg);
}

/**
 * Update replica gc state in tx thread.
 */
static void
tx_gc_advance(struct cmsg *msg)
{
	struct relay_gc_msg *m = (struct relay_gc_msg *)msg;
	gc_consumer_advance(m->relay->replica->gc, m->signature);
	free(m);
}

static void
relay_on_close_log_f(struct trigger *trigger, void * /* event */)
{
	static const struct cmsg_hop route[] = {
		{tx_gc_advance, NULL}
	};
	struct relay *relay = (struct relay *)trigger->data;
	struct relay_gc_msg *m = (struct relay_gc_msg *)malloc(sizeof(*m));
	if (m == NULL) {
		say_warn("failed to allocate relay gc message");
		return;
	}
	cmsg_init(&m->msg, route);
	m->relay = relay;
	m->signature = vclock_sum(&relay->r->vclock);
	cpipe_push(&relay->tx_pipe, &m->msg);
}

static void
relay_process_wal_event(struct wal_watcher *watcher, unsigned events)
{
	struct relay *relay = container_of(watcher, struct relay, wal_watcher);
	if (relay->exiting) {
		/*
		 * Do not try to send anything to the replica
		 * if it already closed its socket.
		 */
		return;
	}
	try {
		recover_remaining_wals(relay->r, &relay->stream, NULL,
				       (events & WAL_EVENT_ROTATE) != 0);
	} catch (Exception *e) {
		relay->failed = true;
	}
}

/*
 * Relay reader fiber function.
 * Read xrow encloded vclocks from slave side.
 */
static int
relay_reader_f(va_list ap)
{
	struct relay *relay = va_arg(ap, struct relay *);
	(void) relay;
	struct ibuf ibuf;
	struct ev_io io;
	coio_create(&io, relay->io.fd);
	ibuf_create(&ibuf, &cord()->slabc, 1024);
	try {
		while (!fiber_is_cancelled()) {
			struct xrow_header xrow;
			coio_read_xrow(&io, &ibuf, &xrow);
			/* vclock is followed while decoding, zeroing it. */
			vclock_create(&relay->recv_vclock);
			xrow_decode_vclock_xc(&xrow, &relay->recv_vclock);
			fiber_cond_signal(&relay->reader_cond);
		}
	} catch (Exception *e) {
		relay->failed = true;
		diag_move(diag_get(), &relay->reader_diag);
	}
	fiber_cond_signal(&relay->reader_cond);
	ibuf_destroy(&ibuf);
	return 0;
}

/**
 * A libev callback invoked when a relay client socket is ready
 * for read. This currently only happens when the client closes
 * its socket, and we get an EOF.
 */
static int
relay_subscribe_f(va_list ap)
{
	struct relay *relay = va_arg(ap, struct relay *);
	struct recovery *r = relay->r;

	coio_enable();
	cbus_endpoint_create(&relay->endpoint, cord_name(cord()),
			     fiber_schedule_cb, fiber());
	cbus_pair("tx", cord_name(cord()), &relay->tx_pipe, &relay->relay_pipe,
		  NULL, NULL, cbus_process);
	/* Setup garbage collection trigger. */
	struct trigger on_close_log = {
		RLIST_LINK_INITIALIZER, relay_on_close_log_f, relay, NULL
	};
	trigger_add(&r->on_close_log, &on_close_log);
	wal_set_watcher(&relay->wal_watcher, cord_name(cord()),
			relay_process_wal_event, cbus_process);

	relay_set_cord_name(relay->io.fd);

	char name[FIBER_NAME_MAX];
	snprintf(name, sizeof(name), "%s:%s", fiber()->name, "monitor");
	struct fiber *reader = fiber_new_xc(name, relay_reader_f);
	fiber_start(reader, relay);

	while (relay->failed == false) {
		double timeout = RELAY_REPORT_INTERVAL;
		struct errinj *inj = errinj(ERRINJ_RELAY_REPORT_INTERVAL,
					    ERRINJ_DOUBLE);
		if (inj != NULL && inj->dparam != 0)
			timeout = inj->dparam;

		/*
		 * The fiber can be woken by IO cancel, by the timeout of
		 * status messaging or by an acknowledge to status message.
		 * Handle cbus messages first.
		 */
		fiber_cond_wait_timeout(&relay->reader_cond, timeout);
		cbus_process(&relay->endpoint);

		/*
		 * Check that the vclock has been updated and the previous
		 * status message is delivered
		 */
		if (relay->status_msg.msg.route != NULL)
			continue;
		static const struct cmsg_hop route[] = {
			{tx_status_update, NULL}
		};
		cmsg_init(&relay->status_msg.msg, route);
		vclock_copy(&relay->status_msg.sent_vclock, &r->vclock);
		vclock_copy(&relay->status_msg.recv_vclock, &relay->recv_vclock);
		relay->status_msg.relay = relay;
		cpipe_push(&relay->tx_pipe, &relay->status_msg.msg);
	}

	say_crit("exiting the relay loop");
	relay->exiting = true;
	trigger_clear(&on_close_log);
	wal_clear_watcher(&relay->wal_watcher, cbus_process);
	cbus_unpair(&relay->tx_pipe, &relay->relay_pipe,
		    NULL, NULL, cbus_process);
	cbus_endpoint_destroy(&relay->endpoint, cbus_process);
	if (relay->failed && diag_is_empty(diag_get()))
		diag_move(&relay->reader_diag, diag_get());

	return relay->failed ? -1 : 0;
}

/** Replication acceptor fiber handler. */
void
relay_subscribe(int fd, uint64_t sync, struct replica *replica,
		struct vclock *replica_clock)
{
	assert(replica->id != REPLICA_ID_NIL);
	/* Don't allow multiple relays for the same replica */
	if (replica->relay != NULL) {
		tnt_raise(ClientError, ER_CFG, "replication",
			  "duplicate connection with the same replica UUID");
	}

	/*
	 * Register the replica with the garbage collector
	 * unless it has already been registered by initial
	 * join.
	 */
	if (replica->gc == NULL) {
		replica->gc = gc_consumer_register(
			tt_sprintf("replica %s", tt_uuid_str(&replica->uuid)),
			vclock_sum(replica_clock));
		if (replica->gc == NULL)
			diag_raise();
	}

	struct relay relay;
	relay_init(&relay, fd, sync, relay_send_row);
	relay.r = recovery_new(cfg_gets("wal_dir"),
			       cfg_geti("force_recovery"),
			       replica_clock);
	vclock_copy(&relay.tx.sent_vclock, replica_clock);
	vclock_create(&relay.tx.recv_vclock);
	relay.replica = replica;
	replica_set_relay(replica, &relay);

	int rc = cord_costart(&relay.cord, tt_sprintf("relay_%p", &relay),
			      relay_subscribe_f, &relay);
	if (rc == 0)
		rc = cord_cojoin(&relay.cord);

	replica_clear_relay(replica);
	recovery_delete(relay.r);

	fiber_cond_destroy(&relay.reader_cond);
	diag_destroy(&relay.reader_diag);
	if (rc != 0)
		diag_raise();
}

static void
relay_send(struct relay *relay, struct xrow_header *packet)
{
	packet->sync = relay->sync;
	coio_write_xrow(&relay->io, packet);
	fiber_gc();

	struct errinj *inj = errinj(ERRINJ_RELAY_TIMEOUT, ERRINJ_DOUBLE);
	if (inj != NULL && inj->dparam > 0)
		fiber_sleep(inj->dparam);
}

static void
relay_send_initial_join_row(struct xstream *stream, struct xrow_header *row)
{
	struct relay *relay = container_of(stream, struct relay, stream);
	relay_send(relay, row);
}

/** Send a single row to the client. */
static void
relay_send_row(struct xstream *stream, struct xrow_header *packet)
{
	struct relay *relay = container_of(stream, struct relay, stream);
	assert(iproto_type_is_dml(packet->type));
	/*
	 * We're feeding a WAL, thus responding to SUBSCRIBE request.
	 * In that case, only send a row if it is not from the same replica
	 * (i.e. don't send replica's own rows back).
	 */
	if (relay->replica == NULL ||
	    packet->replica_id != relay->replica->id) {
		relay_send(relay, packet);
	}
}
