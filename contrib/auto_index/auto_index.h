#ifndef AUTO_INDEX_H
#define AUTO_INDEX_H

#include "postgres.h"
#include "access/attnum.h"
#include "storage/lwlock.h"

#define AUTO_INDEX_MAX_TABLES		256
#define AUTO_INDEX_MAX_COLS			32
#define AUTO_INDEX_MAX_COLSETS		16
#define AUTO_INDEX_COLSET_MAX_COLS	3

typedef struct AutoIndexColumnStats
{
	AttrNumber	attribute_number;
	int64		equality_hits;			/* current interval */
	int64		range_hits;				/* current interval */
	int64		cumulative_benefit;		/* ski rental: accumulated across intervals */
} AutoIndexColumnStats;

/*
 * A column set observed together in a single query's predicate list.
 * The attnos[] are sorted ascending — that gives a stable lookup key.
 * Column ORDER for the actual CREATE INDEX is decided at create time
 * (equality columns first, then range, by attno within group).
 */
typedef struct AutoIndexColSet
{
	int8		n_cols;								/* 0 = empty slot, 2 or 3 used */
	AttrNumber	attnos[AUTO_INDEX_COLSET_MAX_COLS];	/* sorted ascending */
	int8		ops[AUTO_INDEX_COLSET_MAX_COLS];	/* 0=eq, 1=range, parallel to attnos */
	uint64		last_access_tick;
	int64		hits;								/* current interval */
	int64		cumulative_benefit;					/* ski rental accumulator */
} AutoIndexColSet;

typedef struct AutoIndexTableStats
{
	Oid						relation_id;
	uint64					last_access_tick;
	int64					insert_count;			/* current interval */
	int64					update_count;			/* current interval */
	int64					delete_count;			/* current interval */
	int64					cumulative_write_cost;	/* accumulated across intervals */
	AutoIndexColumnStats	columns[AUTO_INDEX_MAX_COLS];
	AutoIndexColSet			colsets[AUTO_INDEX_MAX_COLSETS];
} AutoIndexTableStats;

typedef struct AutoIndexSharedState
{
	LWLock				   *lock;
	uint64					access_counter;
	uint32					generation;
	AutoIndexTableStats		tables[AUTO_INDEX_MAX_TABLES];
} AutoIndexSharedState;

/* Set in shmem_startup_hook; valid in every backend after that */
extern AutoIndexSharedState *auto_index_state;

/* GUC variables */
extern double	auto_index_threshold;
extern int		auto_index_check_interval;
extern int		auto_index_max_indexes_per_table;

/* Background worker entry point — must be exported so bgworker machinery can find it */
extern PGDLLEXPORT void auto_index_main(Datum main_arg);

#endif							/* AUTO_INDEX_H */
