#ifndef TARANTOOL_APPLIER_H_INCLUDED
#define TARANTOOL_APPLIER_H_INCLUDED
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

#include <netinet/in.h>
#include <sys/socket.h>
#include <tarantool_ev.h>

#include "fiber_cond.h"
#include "fiber_channel.h"
#include "trigger.h"
#include "trivia/util.h"
#include "tt_uuid.h"
#include "uri.h"

#include "vclock.h"

struct xstream;

enum { APPLIER_SOURCE_MAXLEN = 1024 }; /* enough to fit URI with passwords */

#define applier_STATE(_)                                             \
	_(APPLIER_OFF, 0)                                            \
	_(APPLIER_CONNECT, 1)                                        \
	_(APPLIER_CONNECTED, 2)                                      \
	_(APPLIER_AUTH, 3)                                           \
	_(APPLIER_READY, 4)                                          \
	_(APPLIER_INITIAL_JOIN, 5)                                   \
	_(APPLIER_FINAL_JOIN, 6)                                     \
	_(APPLIER_JOINED, 7)                                         \
	_(APPLIER_FOLLOW, 8)                                         \
	_(APPLIER_STOPPED, 9)                                        \
	_(APPLIER_DISCONNECTED, 10)                                  \

/** States for the applier */
ENUM(applier_state, applier_STATE);
extern const char *applier_state_strs[];

/**
 * State of a replication connection to the master
 */
struct applier {
	/** Background fiber */
	struct fiber *reader;
	/** Background fiber to reply with vclock */
	struct fiber *writer;
	/** Writer cond. */
	struct fiber_cond writer_cond;
	/** Finite-state machine */
	enum applier_state state;
	/** Local time of this replica when the last row has been received */
	ev_tstamp last_row_time;
	/** Number of seconds this replica is behind the remote master */
	ev_tstamp lag;
	/** The last box_error_code() logged to avoid log flooding */
	uint32_t last_logged_errcode;
	/** Remote UUID */
	struct tt_uuid uuid;
	/** Remote URI (string) */
	char source[APPLIER_SOURCE_MAXLEN];
	/** Remote URI (parsed) */
	struct uri uri;
	/** Remote version encoded as a number, see version_id() macro */
	uint32_t version_id;
	/** Remote address */
	union {
		struct sockaddr addr;
		struct sockaddr_storage addrstorage;
	};
	/** Length of addr */
	socklen_t addr_len;
	/** EV watcher for I/O */
	struct ev_io io;
	/** Input/output buffer for buffered IO */
	struct iobuf *iobuf;
	/** Triggers invoked on state change */
	struct rlist on_state;
	/** Channel used by applier_connect_all() and applier_resume() */
	struct fiber_channel pause;
	/** xstream to process rows during initial JOIN */
	struct xstream *join_stream;
	/** xstream to process rows during final JOIN and SUBSCRIBE */
	struct xstream *subscribe_stream;
};

/**
 * Start a client to a remote master using a background fiber.
 *
 * If recovery is finalized (i.e. r->writer != NULL) then the client
 * connect to a master and follow remote updates using SUBSCRIBE command.
 *
 * If recovery is not finalized (i.e. r->writer == NULL) then the client
 * connect to a master, download and process snapshot using JOIN command
 * and then switch to follow mode.
 *
 * \sa fiber_start()
 */
void
applier_start(struct applier *applier);

/**
 * Stop a client.
 */
void
applier_stop(struct applier *applier);

/**
 * Allocate an instance of applier object, create applier and initialize
 * remote uri (copied to struct applier).
 *
 * @pre     the uri is a valid and checked one
 * @error   throws OutOfMemory exception if out of memory.
 */
struct applier *
applier_new(const char *uri, struct xstream *join_stream,
	    struct xstream *subscribe_stream);

/**
 * Destroy and delete a applier.
 */
void
applier_delete(struct applier *applier);

/*
 * Connect all appliers to remote peer and receive UUID
 * \post appliers are connected to remote hosts and paused.
 * Use applier_resume(applier) to resume applier.
 *
 * \param appliers the array of appliers
 * \param count size of appliers array
 * \param timeout connection timeout
 *
 */
void
applier_connect_all(struct applier **appliers, int count,
		    double timeout);

/*
 * Resume execution of applier until \a state.
 */
void
applier_resume_to_state(struct applier *applier, enum applier_state state,
			double timeout);

/*
 * Resume execution of applier.
 */
void
applier_resume(struct applier *applier);

#endif /* TARANTOOL_APPLIER_H_INCLUDED */
