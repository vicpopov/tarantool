#ifndef TARANTOOL_BOX_ENGINE_H_INCLUDED
#define TARANTOOL_BOX_ENGINE_H_INCLUDED
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
#include "index.h"

struct request;
struct space;
struct tuple;
struct tuple_format_vtab;
struct relay;

extern struct rlist engines;

typedef int
engine_backup_cb(const char *path, void *arg);

#if defined(__cplusplus)

struct Handler;

/** Engine instance */
class Engine {
public:
	Engine(const char *engine_name, struct tuple_format_vtab *format);

	Engine(const Engine &) = delete;
	Engine& operator=(const Engine&) = delete;
	virtual ~Engine() {}
	/** Called once at startup. */
	virtual void init();
	/** Create a new engine instance for a space. */
	virtual Handler *createSpace() = 0;
	/**
	 * Write statements stored in checkpoint @vclock to @stream.
	 */
	virtual void join(struct vclock *vclock, struct xstream *stream);
	/**
	 * Begin a new single or multi-statement transaction.
	 * Called on first statement in a transaction, not when
	 * a user said begin(). Effectively it means that
	 * transaction in the engine begins with the first
	 * statement.
	 */
	virtual void begin(struct txn *);
	/**
	 * Begine one statement in existing transaction.
	 */
	virtual void beginStatement(struct txn *);
	/**
	 * Called before a WAL write is made to prepare
	 * a transaction for commit in the engine.
	 */
	virtual void prepare(struct txn *);
	/**
	 * End the transaction in the engine, the transaction
	 * has been successfully written to the WAL.
	 * This method can't throw: if any error happens here,
	 * there is no better option than panic.
	 */
	virtual void commit(struct txn *);
	/*
	 * Called to roll back effects of a statement if an
	 * error happens, e.g., in a trigger.
	 */
	virtual void rollbackStatement(struct txn *, struct txn_stmt *);
	/*
	 * Roll back and end the transaction in the engine.
	 */
	virtual void rollback(struct txn *);
	/**
	 * Bootstrap an empty data directory
	 */
	virtual void bootstrap();
	/**
	 * Begin initial recovery from snapshot or dirty disk data.
	 * On local recovery @recovery_vclock points to the vclock
	 * used for assigning LSNs to statements replayed from WAL.
	 * On remote recovery, it is set to NULL.
	 */
	virtual void beginInitialRecovery(const struct vclock *recovery_vclock);
	/**
	 * Notify engine about a start of recovering from WALs
	 * that could be local WALs during local recovery
	 * of WAL catch up durin join on slave side
	 */
	virtual void beginFinalRecovery();
	/**
	 * Inform the engine about the end of recovery from the
	 * binary log.
	 */
	virtual void endRecovery();
	/**
	 * Begin a two-phase snapshot creation in this
	 * engine (snapshot is a memtx idea of a checkpoint).
	 * Must not yield.
	 */
	virtual int beginCheckpoint();
	/**
	 * Wait for a checkpoint to complete.
	 */
	virtual int waitCheckpoint(struct vclock *vclock);
	/**
	 * All engines prepared their checkpoints,
	 * fix up the changes.
	 */
	virtual void commitCheckpoint(struct vclock *vclock);
	/**
	 * An error in one of the engines, abort checkpoint.
	 */
	virtual void abortCheckpoint();
	/**
	 * Remove files that are not needed to recover
	 * from snapshot with @lsn or newer.
	 *
	 * If this function returns a non-zero value, garbage
	 * collection is aborted, i.e. this method isn't called
	 * for other engines and xlog files aren't deleted.
	 *
	 * Used to abort garbage collection in case MemtxEngine
	 * failes to delete a snapshot file, because we recover
	 * checkpoint list by scanning snapshot directory.
	 */
	virtual int collectGarbage(int64_t lsn);
	/**
	 * Backup callback. It is supposed to call @cb for each file
	 * that needs to be backed up in order to restore from the
	 * checkpoint @vclock.
	 */
	virtual int backup(struct vclock *vclock,
			   engine_backup_cb cb, void *cb_arg);

	/**
	 * Check definition of a new space for engine-specific
	 * limitations. E.g. not all engines support temporary
	 * tables.
	 */
	virtual void checkSpaceDef(struct space_def *def);
public:
	/** Name of the engine. */
	const char *name;
	/** Engine id. */
	uint32_t id;
	/** Used for search for engine by name. */
	struct rlist link;
	struct tuple_format_vtab *format;
};

/** Engine handle - an operator of a space */

struct Handler {
public:
	Handler(Engine *f);
	virtual ~Handler() {}
	Handler(const Handler &) = delete;
	Handler& operator=(const Handler&) = delete;

	virtual void
	applyInitialJoinRow(struct space *space, struct request *);

	virtual struct tuple *
	executeReplace(struct txn *, struct space *,
		       struct request *);
	virtual struct tuple *
	executeDelete(struct txn *, struct space *,
		      struct request *);
	virtual struct tuple *
	executeUpdate(struct txn *, struct space *,
		      struct request *);
	virtual void
	executeUpsert(struct txn *, struct space *,
		      struct request *);

	virtual void
	executeSelect(struct txn *, struct space *,
		      uint32_t index_id, uint32_t iterator,
		      uint32_t offset, uint32_t limit,
		      const char *key, const char *key_end,
		      struct port *);

	virtual void initSystemSpace(struct space *space);
	/**
	 * Check an index definition for violation of
	 * various limits.
	 */
	virtual void checkIndexDef(struct space *new_space, struct index_def *);
	/**
	 * Create an instance of space index. Used in alter
	 * space before commit to WAL. The created index
	 * is deleted with delete operator.
	 */
	virtual Index *createIndex(struct space *new_space, struct index_def *) = 0;
	/**
	 * Called by alter when a primary key added,
	 * after createIndex is invoked for the new
	 * key and before the write to WAL.
	 */
	virtual void addPrimaryKey(struct space *new_space);
	/**
	 * Called by alter when the primary key is dropped.
	 * Do whatever is necessary with space/handler object,
	 * to not crash in DML.
	 */
	virtual void dropPrimaryKey(struct space *new_space);
	/**
	 * Called with the new empty secondary index. Fill the new index
	 * with data from the primary key of the space.
	 */
	virtual void buildSecondaryKey(struct space *old_space,
				       struct space *new_space,
				       Index *new_index);
	/**
	 * Notify the enigne about upcoming space truncation
	 * so that it can prepare new_space object.
	 */
	virtual void prepareTruncateSpace(struct space *old_space,
					  struct space *new_space);
	/**
	 * Commit space truncation. Called after space truncate
	 * record was written to WAL hence must not fail.
	 *
	 * The old_space is the space that was replaced with the
	 * new_space as a result of truncation. The callback is
	 * supposed to release resources associated with the
	 * old_space and commit the new_space.
	 */
	virtual void commitTruncateSpace(struct space *old_space,
					 struct space *new_space);
	/**
	 * Notify the engine about the changed space,
	 * before it's done, to prepare 'new_space'
	 * object.
	 */
	virtual void prepareAlterSpace(struct space *old_space,
				       struct space *new_space);

	/**
	 * Notify the engine engine after altering a space and
	 * replacing old_space with new_space in the space cache,
	 * to, e.g., update all references to struct space
	 * and replace old_space with new_space.
	 */
	virtual void commitAlterSpace(struct space *old_space,
				      struct space *new_space);
	Engine *engine;
};

/** Register engine engine instance. */
void engine_register(Engine *engine);

/** Call a visitor function on every registered engine. */
#define engine_foreach(engine) rlist_foreach_entry(engine, &engines, link)

/** Find engine engine by name. */
Engine *engine_find(const char *name);

/** Shutdown all engine factories. */
void engine_shutdown();

static inline uint32_t
engine_id(Handler *space)
{
	return space->engine->id;
}

/**
 * Initialize an empty data directory
 */
void
engine_bootstrap();

/**
 * Called at the start of recovery.
 */
void
engine_begin_initial_recovery(const struct vclock *recovery_vclock);

/**
 * Called in the middle of JOIN stage,
 * when xlog catch-up process is started
 */
void
engine_begin_final_recovery();

/**
 * Called at the end of recovery.
 * Build secondary keys in all spaces.
 */
void
engine_end_recovery();

/**
 * Feed snapshot data as join events to the replicas.
 * (called on the master).
 */
void
engine_join(struct vclock *vclock, struct xstream *stream);

extern "C" {
#endif /* defined(__cplusplus) */

int
engine_begin_checkpoint();

/**
 * Save a snapshot.
 */
int
engine_commit_checkpoint(struct vclock *vclock);

void
engine_abort_checkpoint();

int
engine_collect_garbage(int64_t lsn);

int
engine_backup(struct vclock *vclock, engine_backup_cb cb, void *cb_arg);

#if defined(__cplusplus)
} /* extern "C" */
#endif /* defined(__cplusplus) */

#endif /* TARANTOOL_BOX_ENGINE_H_INCLUDED */
