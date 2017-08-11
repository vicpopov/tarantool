#ifndef TARANTOOL_BOX_MEMTX_ENGINE_H_INCLUDED
#define TARANTOOL_BOX_MEMTX_ENGINE_H_INCLUDED
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
#include "engine.h"
#include "xlog.h"

/**
 * The state of memtx recovery process.
 * There is a global state of the entire engine state of each
 * space. The state of a space is initialized from the engine
 * state when the space is created. The exception is system
 * spaces, which are always created in the final (OK) state.
 *
 * The states exist to speed up recovery: initial state
 * assumes write-only flow of sorted rows from a snapshot.
 * It's followed by a state for read-write recovery
 * of rows from the write ahead log; these rows are
 * inserted only into the primary key. The final
 * state is for a fully functional space.
 */
enum memtx_recovery_state {
	/** The space has no indexes. */
	MEMTX_INITIALIZED,
	/**
	 * The space has only the primary index, which is in
	 * write-only bulk insert mode.
	 */
	MEMTX_INITIAL_RECOVERY,
	/**
	 * The space has the primary index, which can be
	 * used for reads and writes, but secondary indexes are
	 * empty. The will be built at the end of recovery.
	 */
	MEMTX_FINAL_RECOVERY,
	/**
	 * The space and all its indexes are fully built.
	 */
	MEMTX_OK,
};

/** Memtx extents pool, available to statistics. */
extern struct mempool memtx_index_extent_pool;

struct MemtxEngine: public Engine {
	MemtxEngine(const char *snap_dirname, bool force_recovery,
		    uint64_t tuple_arena_max_size,
		    uint32_t objsize_min, float alloc_factor);
	~MemtxEngine();
	virtual Handler *createSpace(struct rlist *key_list,
				     uint32_t index_count,
				     uint32_t exact_field_count) override;
	virtual void begin(struct txn *txn) override;
	virtual void rollbackStatement(struct txn *,
				       struct txn_stmt *stmt) override;
	virtual void rollback(struct txn *txn) override;
	virtual void prepare(struct txn *txn) override;
	virtual void commit(struct txn *txn) override;
	virtual void bootstrap() override;
	virtual void beginInitialRecovery(const struct vclock *) override;
	virtual void beginFinalRecovery() override;
	virtual void endRecovery() override;
	virtual void join(struct vclock *vclock,
			  struct xstream *stream) override;
	virtual int beginCheckpoint() override;
	virtual int waitCheckpoint(struct vclock *vclock) override;
	virtual void commitCheckpoint(struct vclock *vclock) override;
	virtual void abortCheckpoint() override;
	virtual int collectGarbage(int64_t lsn) override;
	virtual int backup(struct vclock *vclock,
			   engine_backup_cb cb, void *arg) override;
	/* Update snap_io_rate_limit. */
	void setSnapIoRateLimit(double new_limit)
	{
		m_snap_io_rate_limit = new_limit * 1024 * 1024;
	}
	void setMaxTupleSize(size_t max_size);
	/**
	 * Return LSN and vclock of the most recent snapshot
	 * or -1 if there is no snapshot.
	 */
	int64_t lastSnapshot(struct vclock *vclock)
	{
		return xdir_last_vclock(&m_snap_dir, vclock);
	}
	/**
	 * Return the vclock of the snapshot following the
	 * given one or NULL if the given snapshot is last.
	 * Pass NULL to get the oldest snapshot.
	 */
	const struct vclock *nextSnapshot(const struct vclock *vclock)
	{
		if (vclock == NULL)
			return vclockset_first(&m_snap_dir.index);
		return vclockset_next(&m_snap_dir.index,
				      (struct vclock *)vclock);
	}
	/**
	 * Return the vclock of the snapshot preceding the
	 * given one or NULL if the given snapshot is oldest.
	 * Pass NULL to get the newest snapshot.
	 */
	const struct vclock *prevSnapshot(const struct vclock *vclock)
	{
		if (vclock == NULL)
			return vclockset_last(&m_snap_dir.index);
		return vclockset_prev(&m_snap_dir.index,
				      (struct vclock *)vclock);
	}
	void recoverSnapshot(const struct vclock *vclock);
public:
	/** Engine recovery state */
	enum memtx_recovery_state m_state;
private:
	void
	recoverSnapshotRow(struct xrow_header *row);
	/** Non-zero if there is a checkpoint (snapshot) in progress. */
	struct checkpoint *m_checkpoint;
	/** The directory where to store snapshots. */
	struct xdir m_snap_dir;
	/** Limit disk usage of checkpointing (bytes per second). */
	uint64_t m_snap_io_rate_limit;
	bool m_force_recovery;
};

enum {
	MEMTX_EXTENT_SIZE = 16 * 1024,
	MEMTX_SLAB_SIZE = 4 * 1024 * 1024
};

/**
 * Initialize arena for indexes.
 * The arena is used for memtx_index_extent_alloc
 *  and memtx_index_extent_free.
 * Can be called several times, only first call do the work.
 */
void
memtx_index_arena_init();

/**
 * Allocate a block of size MEMTX_EXTENT_SIZE for memtx index
 */
void *
memtx_index_extent_alloc(void *ctx);

/**
 * Free a block previously allocated by memtx_index_extent_alloc
 */
void
memtx_index_extent_free(void *ctx, void *extent);

/**
 * Reserve num extents in pool.
 * Ensure that next num extent_alloc will succeed w/o an error
 */
void
memtx_index_extent_reserve(int num);

#endif /* TARANTOOL_BOX_MEMTX_ENGINE_H_INCLUDED */
