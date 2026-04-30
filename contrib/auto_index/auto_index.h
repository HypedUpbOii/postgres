#ifndef AUTO_INDEX_H
#define AUTO_INDEX_H

#include "postgres.h"
#include "access/attnum.h"
#include "storage/lwlock.h"

#define AUTO_INDEX_MAX_TABLES	256
#define AUTO_INDEX_MAX_COLS		16

typedef struct AutoIndexColumnStats
{
	AttrNumber	attribute_number;
	int64		equality_hits;
	int64		range_hits;
} AutoIndexColumnStats;

typedef struct AutoIndexTableStats
{
	Oid						relation_id;
	uint64					last_access_tick;
	int64					insert_count;
	int64					update_count;
	int64					delete_count;
	AutoIndexColumnStats	columns[AUTO_INDEX_MAX_COLS];
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
