#include "postgres.h"

#include "access/xact.h"
#include "executor/executor.h"
#include "executor/spi.h"
#include "fmgr.h"
#include "miscadmin.h"
#include "nodes/execnodes.h"
#include "nodes/parsenodes.h"
#include "nodes/plannodes.h"
#include "nodes/primnodes.h"
#include "postmaster/bgworker.h"
#include "postmaster/interrupt.h"
#include "storage/ipc.h"
#include "storage/latch.h"
#include "storage/lwlock.h"
#include "storage/shmem.h"
#include "tcop/tcopprot.h"
#include "utils/guc.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/wait_event.h"
#include "funcapi.h"

#include "auto_index.h"

PG_MODULE_MAGIC;

AutoIndexSharedState *auto_index_state = NULL;

/* GUC variables */
double	auto_index_threshold			= 10.0;
int		auto_index_check_interval		= 300;
int		auto_index_max_indexes_per_table = 3;

/* Saved previous hook values — always chain, never discard */
static ExecutorEnd_hook_type	prev_ExecutorEnd	= NULL;
static shmem_request_hook_type	prev_shmem_request	= NULL;
static shmem_startup_hook_type	prev_shmem_startup	= NULL;

/* Forward declarations */
static void auto_index_shmem_request(void);
static void auto_index_shmem_startup(void);
static void auto_index_executor_end(QueryDesc *queryDesc);
static void walk_plan_state(PlanState *node);
static void process_seqscan(SeqScanState *node);
static int	find_table_slot(Oid relid);
static int	find_col_slot(int tslot, AttrNumber attno);
static void evaluate_and_manage_indexes(AutoIndexTableStats *snapshot);

void _PG_init(void);


/* -------------------------------------------------------------------------
 * Module init
 * -------------------------------------------------------------------------
 */

void
_PG_init(void)
{
	BackgroundWorker worker;

	DefineCustomRealVariable(
		"auto_index.threshold",
		"Benefit/cost ratio above which an index is created.",
		NULL,
		&auto_index_threshold,
		10.0, 1.0, 1000.0,
		PGC_SIGHUP,
		0, NULL, NULL, NULL);

	DefineCustomIntVariable(
		"auto_index.check_interval",
		"Seconds between background worker evaluation passes.",
		NULL,
		&auto_index_check_interval,
		300, 1, 3600,
		PGC_SIGHUP,
		0, NULL, NULL, NULL);

	DefineCustomIntVariable(
		"auto_index.max_indexes_per_table",
		"Maximum number of auto-created indexes per table.",
		NULL,
		&auto_index_max_indexes_per_table,
		3, 1, 10,
		PGC_SIGHUP,
		0, NULL, NULL, NULL);

	MarkGUCPrefixReserved("auto_index");

	prev_shmem_request = shmem_request_hook;
	shmem_request_hook = auto_index_shmem_request;

	prev_shmem_startup = shmem_startup_hook;
	shmem_startup_hook = auto_index_shmem_startup;

	prev_ExecutorEnd = ExecutorEnd_hook;
	ExecutorEnd_hook = auto_index_executor_end;

	memset(&worker, 0, sizeof(worker));
	snprintf(worker.bgw_name,		  BGW_MAXLEN, "auto_index worker");
	snprintf(worker.bgw_type,		  BGW_MAXLEN, "auto_index");
	snprintf(worker.bgw_library_name, MAXPGPATH,  "auto_index");
	snprintf(worker.bgw_function_name,BGW_MAXLEN, "auto_index_main");
	worker.bgw_flags		= BGWORKER_SHMEM_ACCESS | BGWORKER_BACKEND_DATABASE_CONNECTION;
	worker.bgw_start_time	= BgWorkerStart_RecoveryFinished;
	worker.bgw_restart_time = 10;
	RegisterBackgroundWorker(&worker);
}


/* -------------------------------------------------------------------------
 * Shared memory
 * -------------------------------------------------------------------------
 */

static void
auto_index_shmem_request(void)
{
	if (prev_shmem_request)
		prev_shmem_request();

	RequestAddinShmemSpace(sizeof(AutoIndexSharedState));
	RequestNamedLWLockTranche("auto_index", 1);
}

static void
auto_index_shmem_startup(void)
{
	bool	found;

	if (prev_shmem_startup)
		prev_shmem_startup();

	auto_index_state = ShmemInitStruct("auto_index",
									   sizeof(AutoIndexSharedState),
									   &found);
	if (!found)
	{
		memset(auto_index_state, 0, sizeof(AutoIndexSharedState));
		auto_index_state->lock = &GetNamedLWLockTranche("auto_index")[0].lock;
	}
}


/* -------------------------------------------------------------------------
 * LRU slot management — both functions must be called under LW_EXCLUSIVE
 * -------------------------------------------------------------------------
 */

/*
 * Find or allocate a table slot for relid.  On a miss, evicts the LRU
 * entry when all slots are full.  Returns the slot index.
 */
static int
find_table_slot(Oid relid)
{
	int		i;
	int		empty_slot = -1;
	int		lru_slot   = -1;
	uint64	lru_time   = UINT64_MAX;

	for (i = 0; i < AUTO_INDEX_MAX_TABLES; i++)
	{
		AutoIndexTableStats *e = &auto_index_state->tables[i];

		if (e->relation_id == relid)
		{
			e->last_access_tick = ++auto_index_state->access_counter;
			return i;
		}
		if (e->relation_id == 0 && empty_slot == -1)
			empty_slot = i;
		if (e->last_access_tick < lru_time)
		{
			lru_time = e->last_access_tick;
			lru_slot = i;
		}
	}

	i = (empty_slot != -1) ? empty_slot : lru_slot;
	memset(&auto_index_state->tables[i], 0, sizeof(AutoIndexTableStats));
	auto_index_state->tables[i].relation_id		  = relid;
	auto_index_state->tables[i].last_access_tick = ++auto_index_state->access_counter;
	return i;
}

/*
 * Find or allocate a column slot for attno inside table slot tslot.
 * Returns the column slot index, or -1 if all column slots are full.
 */
static int
find_col_slot(int tslot, AttrNumber attno)
{
	int		i;
	int		empty_slot = -1;

	for (i = 0; i < AUTO_INDEX_MAX_COLS; i++)
	{
		AutoIndexColumnStats *c = &auto_index_state->tables[tslot].columns[i];

		if (c->attribute_number == attno)
			return i;
		if (c->attribute_number == 0 && empty_slot == -1)
			empty_slot = i;
	}

	if (empty_slot == -1)
		return -1;				/* all column slots occupied — drop this stat */

	auto_index_state->tables[tslot].columns[empty_slot].attribute_number = attno;
	return empty_slot;
}


/* -------------------------------------------------------------------------
 * Executor hook — SeqScan qual extraction
 * -------------------------------------------------------------------------
 */

/*
 * Process one SeqScan node: walk its qual list and record per-column
 * predicate hits in shared memory.
 */
static void
process_seqscan(SeqScanState *node)
{
	Relation	rel;
	Oid			relid;
	int			tslot;
	List	   *qual;
	ListCell   *lc;

	rel = node->ss.ss_currentRelation;
	if (rel == NULL)
		return;

	relid = RelationGetRelid(rel);
	if (!OidIsValid(relid))
		return;

	/* Uncompiled qual list lives on the static Plan node */
	qual = node->ss.ps.plan->qual;
	if (qual == NIL)
		return;

	LWLockAcquire(auto_index_state->lock, LW_EXCLUSIVE);

	tslot = find_table_slot(relid);

	foreach(lc, qual)
	{
		Expr	   *expr = (Expr *) lfirst(lc);
		OpExpr	   *op;
		Expr	   *left,
				   *right;
		AttrNumber	attno = InvalidAttrNumber;
		char	   *opname;
		int			cslot;

		if (!IsA(expr, OpExpr))
			continue;

		op    = (OpExpr *) expr;
		left  = linitial(op->args);
		right = lsecond(op->args);

		if (IsA(left, Var) && IsA(right, Const))
			attno = ((Var *) left)->varattno;
		else if (IsA(left, Const) && IsA(right, Var))
			attno = ((Var *) right)->varattno;

		if (!AttrNumberIsForUserDefinedAttr(attno))
			continue;

		cslot = find_col_slot(tslot, attno);
		if (cslot < 0)
			continue;

		opname = get_opname(op->opno);
		if (opname == NULL)
			continue;

		if (strcmp(opname, "=") == 0)
			auto_index_state->tables[tslot].columns[cslot].equality_hits++;
		else if (strcmp(opname, "<")  == 0 || strcmp(opname, ">")  == 0 ||
				 strcmp(opname, "<=") == 0 || strcmp(opname, ">=") == 0)
			auto_index_state->tables[tslot].columns[cslot].range_hits++;

		pfree(opname);
	}

	LWLockRelease(auto_index_state->lock);
}

/*
 * Recursively walk a PlanState tree, calling process_seqscan on every
 * SeqScanState node found.
 */
static void
walk_plan_state(PlanState *node)
{
	ListCell   *lc;

	if (node == NULL)
		return;

	if (IsA(node, SeqScanState))
		process_seqscan((SeqScanState *) node);

	walk_plan_state(node->lefttree);
	walk_plan_state(node->righttree);

	/* Correlated subqueries live in subPlan, not left/right trees */
	foreach(lc, node->subPlan)
		walk_plan_state(((SubPlanState *) lfirst(lc))->planstate);
}

static void
auto_index_executor_end(QueryDesc *queryDesc)
{
	/* Write tracking */
	if (auto_index_state != NULL &&
		(queryDesc->operation == CMD_INSERT ||
		 queryDesc->operation == CMD_UPDATE ||
		 queryDesc->operation == CMD_DELETE))
	{
		PlannedStmt *pstmt = queryDesc->plannedstmt;

		if (pstmt->resultRelationRelids != NULL)
		{
			int				rtindex = bms_next_member(pstmt->resultRelationRelids, -1);
			RangeTblEntry  *rte = (rtindex > 0)
				? (RangeTblEntry *) list_nth(pstmt->rtable, rtindex - 1)
				: NULL;
			Oid				relid = (rte != NULL) ? rte->relid : InvalidOid;

			if (OidIsValid(relid))
			{
				int tslot;

				LWLockAcquire(auto_index_state->lock, LW_EXCLUSIVE);
				tslot = find_table_slot(relid);
				if (queryDesc->operation == CMD_INSERT)
					auto_index_state->tables[tslot].insert_count++;
				else if (queryDesc->operation == CMD_UPDATE)
					auto_index_state->tables[tslot].update_count++;
				else
					auto_index_state->tables[tslot].delete_count++;
				LWLockRelease(auto_index_state->lock);
			}
		}
	}

	/* Read tracking */
	if (auto_index_state != NULL && queryDesc->planstate != NULL)
		walk_plan_state(queryDesc->planstate);

	if (prev_ExecutorEnd)
		prev_ExecutorEnd(queryDesc);
	else
		standard_ExecutorEnd(queryDesc);
}


/* -------------------------------------------------------------------------
 * SQL-callable functions
 * -------------------------------------------------------------------------
 */

PG_FUNCTION_INFO_V1(auto_index_reset);
Datum
auto_index_reset(PG_FUNCTION_ARGS)
{
	if (auto_index_state == NULL)
		ereport(ERROR,
				(errmsg("auto_index shared memory not initialised — "
						"is auto_index in shared_preload_libraries?")));

	LWLockAcquire(auto_index_state->lock, LW_EXCLUSIVE);
	memset(auto_index_state->tables, 0,
		   sizeof(auto_index_state->tables));
	auto_index_state->access_counter = 0;
	auto_index_state->generation++;
	LWLockRelease(auto_index_state->lock);

	PG_RETURN_VOID();
}

/*
 * auto_index_stats() — SRF returning one row per tracked (table, column) pair.
 *
 * We snapshot shared memory on the first call, store it in FuncCallContext,
 * then iterate across all table×column combinations across subsequent calls.
 */

typedef struct StatsScanState
{
	/* local snapshot taken under the lock */
	AutoIndexTableStats tables[AUTO_INDEX_MAX_TABLES];
	/* current position */
	int		tslot;
	int		cslot;
} StatsScanState;

PG_FUNCTION_INFO_V1(auto_index_stats);
Datum
auto_index_stats(PG_FUNCTION_ARGS)
{
	FuncCallContext *funcctx;
	StatsScanState  *scan;

	if (SRF_IS_FIRSTCALL())
	{
		MemoryContext oldctx;

		funcctx = SRF_FIRSTCALL_INIT();
		oldctx  = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

		if (auto_index_state == NULL)
			ereport(ERROR,
					(errmsg("auto_index shared memory not initialised — "
							"is auto_index in shared_preload_libraries?")));

		scan = palloc(sizeof(StatsScanState));
		LWLockAcquire(auto_index_state->lock, LW_SHARED);
		memcpy(scan->tables, auto_index_state->tables,
			   sizeof(scan->tables));
		LWLockRelease(auto_index_state->lock);
		scan->tslot = 0;
		scan->cslot = 0;

		funcctx->user_fctx = scan;

		/* Derive tuple descriptor from the function's declared OUT parameters */
		{
			TupleDesc tupdesc;

			if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
				ereport(ERROR,
						(errmsg("auto_index_stats: return type must be composite")));
			funcctx->tuple_desc = BlessTupleDesc(tupdesc);
		}

		MemoryContextSwitchTo(oldctx);
	}

	funcctx = SRF_PERCALL_SETUP();
	scan    = (StatsScanState *) funcctx->user_fctx;

	/* Advance to the next non-empty column slot */
	while (scan->tslot < AUTO_INDEX_MAX_TABLES)
	{
		AutoIndexTableStats  *t = &scan->tables[scan->tslot];
		AutoIndexColumnStats *c;

		if (!OidIsValid(t->relation_id))
		{
			scan->tslot++;
			scan->cslot = 0;
			continue;
		}

		if (scan->cslot >= AUTO_INDEX_MAX_COLS)
		{
			scan->tslot++;
			scan->cslot = 0;
			continue;
		}

		c = &t->columns[scan->cslot];
		scan->cslot++;

		if (!AttrNumberIsForUserDefinedAttr(c->attribute_number))
			continue;
		if (c->equality_hits == 0 && c->range_hits == 0)
			continue;

		{
			Datum		values[4];
			bool		nulls[4] = {false, false, false, false};
			HeapTuple	tuple;

			values[0] = ObjectIdGetDatum(t->relation_id);
			values[1] = Int16GetDatum(c->attribute_number);
			values[2] = Int64GetDatum(c->equality_hits);
			values[3] = Int64GetDatum(c->range_hits);

			tuple = heap_form_tuple(funcctx->tuple_desc, values, nulls);
			SRF_RETURN_NEXT(funcctx, HeapTupleGetDatum(tuple));
		}
	}

	SRF_RETURN_DONE(funcctx);
}


/* -------------------------------------------------------------------------
 * Background worker
 * -------------------------------------------------------------------------
 */

/*
 * Decision records built in Phase 1 (SELECT), consumed in Phase 2 (DDL).
 */
#define MAX_DECISIONS 64

typedef struct CreateDecision
{
	Oid			relid;
	AttrNumber	attno;
	char		schema[NAMEDATALEN];
	char		relname[NAMEDATALEN];
	char		attname[NAMEDATALEN];
	char		idxname[NAMEDATALEN];
} CreateDecision;

typedef struct DropDecision
{
	Oid		indexrelid;
	char	schema[NAMEDATALEN];
	char	idxname[NAMEDATALEN];
} DropDecision;

/*
 * Query helpers — all must be called inside an active SPI connection.
 */

/* Returns true if an index already covers attno on relid. */
static bool
index_exists_for_column(Oid relid, AttrNumber attno)
{
	Oid		argtypes[2] = {OIDOID, INT2OID};
	Datum	args[2]		= {ObjectIdGetDatum(relid), Int16GetDatum(attno)};
	char	nulls[2]	= {' ', ' '};
	int		ret;

	ret = SPI_execute_with_args(
		"SELECT 1 FROM pg_index "
		"WHERE indrelid = $1 AND ($2::int2 = ANY(indkey)) AND indisvalid "
		"LIMIT 1",
		2, argtypes, args, nulls, true, 1);

	return (ret == SPI_OK_SELECT && SPI_processed > 0);
}

/* Returns the number of auto-created indexes already on relid. */
static int
count_auto_indexes(Oid relid)
{
	Oid		argtypes[1] = {OIDOID};
	Datum	args[1]		= {ObjectIdGetDatum(relid)};
	char	nulls[1]	= {' '};
	int		ret;
	bool	isnull;

	/* Skip if catalog table doesn't exist yet */
	ret = SPI_execute(
		"SELECT 1 FROM pg_tables "
		"WHERE schemaname = 'public' AND tablename = 'auto_index_catalog' LIMIT 1",
		true, 1);
	if (ret != SPI_OK_SELECT || SPI_processed == 0)
		return 0;

	ret = SPI_execute_with_args(
		"SELECT count(*)::int FROM auto_index_catalog WHERE relid = $1",
		1, argtypes, args, nulls, true, 1);

	if (ret != SPI_OK_SELECT || SPI_processed == 0)
		return 0;

	return DatumGetInt32(SPI_getbinval(SPI_tuptable->vals[0],
									   SPI_tuptable->tupdesc, 1, &isnull));
}

/*
 * Core evaluation loop.  snapshot is a local copy of the shared memory
 * tables array taken before any SPI work.
 *
 * Phase 1: run SELECTs inside a normal transaction to gather decisions.
 * Phase 2: execute DDL in a non-atomic SPI context.
 */
static void
evaluate_and_manage_indexes(AutoIndexTableStats *snapshot)
{
	CreateDecision	to_create[MAX_DECISIONS];
	DropDecision	to_drop[MAX_DECISIONS];
	int				n_create = 0,
					n_drop   = 0;
	int				i,
					c;

	/* ------------------------------------------------------------------
	 * Phase 1: gather decisions inside a regular transaction
	 * ------------------------------------------------------------------
	 */
	SetCurrentStatementStartTimestamp();
	StartTransactionCommand();
	SPI_connect();
	PushActiveSnapshot(GetTransactionSnapshot());

	for (i = 0; i < AUTO_INDEX_MAX_TABLES && n_create + n_drop < MAX_DECISIONS; i++)
	{
		AutoIndexTableStats *t = &snapshot[i];
		int64	write_cost;
		int		ret;

		if (!OidIsValid(t->relation_id))
			continue;

		write_cost = t->insert_count + t->update_count + t->delete_count;

		/* CREATE candidates: columns with predicate hits and no existing index */
		for (c = 0; c < AUTO_INDEX_MAX_COLS && n_create < MAX_DECISIONS; c++)
		{
			AutoIndexColumnStats *col = &t->columns[c];
			int64	benefit;
			double	ratio;
			int		ret2;

			if (!AttrNumberIsForUserDefinedAttr(col->attribute_number))
				continue;
			if (col->equality_hits == 0 && col->range_hits == 0)
				continue;

			benefit = col->equality_hits * 2 + col->range_hits;
			ratio   = (double) benefit / Max(1, write_cost);

			if (ratio <= auto_index_threshold)
				continue;
			if (index_exists_for_column(t->relation_id, col->attribute_number))
				continue;
			if (count_auto_indexes(t->relation_id) >= auto_index_max_indexes_per_table)
				continue;

			/* Resolve schema, table, column names */
			{
				Oid		argtypes2[2] = {OIDOID, INT2OID};
				Datum	args2[2]	 = {ObjectIdGetDatum(t->relation_id),
									    Int16GetDatum(col->attribute_number)};
				char	nulls2[2]	 = {' ', ' '};
				char   *schema, *relname, *attname;
				CreateDecision *d = &to_create[n_create];

				ret2 = SPI_execute_with_args(
					"SELECT n.nspname, c.relname "
					"FROM pg_class c JOIN pg_namespace n ON n.oid = c.relnamespace "
					"WHERE c.oid = $1",
					1, argtypes2, args2, nulls2, true, 1);
				if (ret2 != SPI_OK_SELECT || SPI_processed == 0)
					continue;

				schema  = SPI_getvalue(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 1);
				relname = SPI_getvalue(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 2);

				ret2 = SPI_execute_with_args(
					"SELECT attname FROM pg_attribute "
					"WHERE attrelid = $1 AND attnum = $2 AND NOT attisdropped",
					2, argtypes2, args2, nulls2, true, 1);
				if (ret2 != SPI_OK_SELECT || SPI_processed == 0)
					continue;

				attname = SPI_getvalue(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 1);

				d->relid = t->relation_id;
				d->attno = col->attribute_number;
				strlcpy(d->schema,  schema,  NAMEDATALEN);
				strlcpy(d->relname, relname, NAMEDATALEN);
				strlcpy(d->attname, attname, NAMEDATALEN);
				snprintf(d->idxname, NAMEDATALEN, "auto_idx_%u_%d",
						 t->relation_id, col->attribute_number);
				n_create++;
			}
		}

		/* DROP candidates: auto-created indexes with no recent usage */
		{
			Oid		argtypes[1] = {OIDOID};
			Datum	args[1]		= {ObjectIdGetDatum(t->relation_id)};
			char	nulls[1]	= {' '};

			/* catalog may not exist yet */
			ret = SPI_execute(
				"SELECT 1 FROM pg_tables "
				"WHERE schemaname = 'public' AND tablename = 'auto_index_catalog' LIMIT 1",
				true, 1);
			if (ret != SPI_OK_SELECT || SPI_processed == 0)
				continue;

			ret = SPI_execute_with_args(
				"SELECT a.indexrelid, a.last_checked_idx_scan, "
				"       n.nspname, c.relname "
				"FROM auto_index_catalog a "
				"JOIN pg_class c ON c.oid = a.indexrelid "
				"JOIN pg_namespace n ON n.oid = c.relnamespace "
				"WHERE a.relid = $1",
				1, argtypes, args, nulls, true, 0);

			if (ret == SPI_OK_SELECT)
			{
				uint64	nrows = SPI_processed;
				SPITupleTable *tbl = SPI_tuptable;

				for (uint64 r = 0; r < nrows && n_drop < MAX_DECISIONS; r++)
				{
					Oid		indexrelid;
					int64	last_scan, cur_scan;
					char   *schema, *idxname;
					bool	isnull2;
					int		ret3;
					Oid		scan_argtypes[1] = {OIDOID};
					Datum	scan_args[1];
					char	scan_nulls[1]	 = {' '};

					indexrelid = DatumGetObjectId(
						SPI_getbinval(tbl->vals[r], tbl->tupdesc, 1, &isnull2));
					last_scan = DatumGetInt64(
						SPI_getbinval(tbl->vals[r], tbl->tupdesc, 2, &isnull2));
					schema	= SPI_getvalue(tbl->vals[r], tbl->tupdesc, 3);
					idxname = SPI_getvalue(tbl->vals[r], tbl->tupdesc, 4);

					scan_args[0] = ObjectIdGetDatum(indexrelid);
					ret3 = SPI_execute_with_args(
						"SELECT idx_scan FROM pg_stat_user_indexes "
						"WHERE indexrelid = $1",
						1, scan_argtypes, scan_args, scan_nulls, true, 1);

					if (ret3 != SPI_OK_SELECT || SPI_processed == 0)
						continue;

					cur_scan = DatumGetInt64(
						SPI_getbinval(SPI_tuptable->vals[0],
									  SPI_tuptable->tupdesc, 1, &isnull2));

					/* Update baseline regardless of decision */
					{
						Oid		upd_types[2]  = {INT8OID, OIDOID};
						Datum	upd_args[2]   = {Int64GetDatum(cur_scan),
											     ObjectIdGetDatum(indexrelid)};
						char	upd_nulls[2]  = {' ', ' '};

						SPI_execute_with_args(
							"UPDATE auto_index_catalog "
							"SET last_checked_idx_scan = $1, last_checked_at = now() "
							"WHERE indexrelid = $2",
							2, upd_types, upd_args, upd_nulls, false, 0);
					}

					/* Drop if unused since last check */
					if (cur_scan == last_scan && n_drop < MAX_DECISIONS)
					{
						DropDecision *d = &to_drop[n_drop];

						d->indexrelid = indexrelid;
						strlcpy(d->schema,  schema,  NAMEDATALEN);
						strlcpy(d->idxname, idxname, NAMEDATALEN);
						n_drop++;
					}
				}
			}
		}
	}

	PopActiveSnapshot();
	SPI_finish();
	CommitTransactionCommand();

	/* ------------------------------------------------------------------
	 * Phase 2: one transaction per DDL statement.
	 *
	 * CREATE/DROP INDEX CONCURRENTLY cannot run inside any SPI context
	 * (atomic or non-atomic) in a bgworker because the CONCURRENTLY path
	 * internally commits and restarts transactions, which confuses the SPI
	 * snapshot stack.  Regular CREATE/DROP INDEX is used instead; it takes
	 * a short ShareLock during the build which is acceptable for the tables
	 * and workloads this extension targets.
	 * ------------------------------------------------------------------
	 */
	if (n_create == 0 && n_drop == 0)
		return;

	for (i = 0; i < n_create; i++)
	{
		CreateDecision *d = &to_create[i];
		char	sql[1024];

		snprintf(sql, sizeof(sql),
				 "CREATE INDEX IF NOT EXISTS %s ON %s.%s (%s)",
				 d->idxname, d->schema, d->relname, d->attname);

		PG_TRY();
		{
			SetCurrentStatementStartTimestamp();
			StartTransactionCommand();
			SPI_connect();
			PushActiveSnapshot(GetTransactionSnapshot());

			SPI_execute(sql, false, 0);

			/* Record in catalog using INSERT...SELECT to avoid a separate
			 * OID lookup — the index is visible in pg_class within the same
			 * transaction so this finds it and inserts in one statement. */
			{
				Oid		argtypes[3] = {OIDOID, INT2OID, NAMEOID};
				Datum	args[3]		= {ObjectIdGetDatum(d->relid),
									   Int16GetDatum(d->attno),
									   DirectFunctionCall1(namein,
										   CStringGetDatum(d->idxname))};
				char	nulls[3]	= {' ', ' ', ' '};

				/* Use -1 as the initial baseline so the first DROP evaluation
				 * sees cur_scan(0) != last_scan(-1) and skips the drop.
				 * Only after one full interval with no usage will it drop. */
				SPI_execute_with_args(
					"INSERT INTO auto_index_catalog "
					"    (relid, indexrelid, attno, created_at, last_checked_idx_scan) "
					"SELECT $1, c.oid, $2, now(), -1 "
					"FROM   pg_class c "
					"JOIN   pg_namespace n ON n.oid = c.relnamespace "
					"WHERE  c.relname = $3 AND n.nspname = 'public' "
					"ON CONFLICT DO NOTHING",
					3, argtypes, args, nulls, false, 0);
			}

			PopActiveSnapshot();
			SPI_finish();
			CommitTransactionCommand();

			elog(LOG, "auto_index: created index %s on %s.%s (%s)",
				 d->idxname, d->schema, d->relname, d->attname);
		}
		PG_CATCH();
		{
			ErrorData  *edata;
			MemoryContext oldctx = MemoryContextSwitchTo(TopMemoryContext);

			edata = CopyErrorData();
			MemoryContextSwitchTo(oldctx);
			elog(WARNING, "auto_index: failed to create index %s: %s",
				 d->idxname, edata->message);
			FreeErrorData(edata);
			FlushErrorState();
			AbortCurrentTransaction();
		}
		PG_END_TRY();
	}

	for (i = 0; i < n_drop; i++)
	{
		DropDecision *d = &to_drop[i];
		char	sql[512];

		snprintf(sql, sizeof(sql),
				 "DROP INDEX IF EXISTS %s.%s",
				 d->schema, d->idxname);

		PG_TRY();
		{
			SetCurrentStatementStartTimestamp();
			StartTransactionCommand();
			SPI_connect();
			PushActiveSnapshot(GetTransactionSnapshot());

			SPI_execute(sql, false, 0);

			/* Remove from catalog */
			{
				Oid		argtypes[1] = {OIDOID};
				Datum	args[1]		= {ObjectIdGetDatum(d->indexrelid)};
				char	nulls[1]	= {' '};

				SPI_execute_with_args(
					"DELETE FROM auto_index_catalog WHERE indexrelid = $1",
					1, argtypes, args, nulls, false, 0);
			}

			PopActiveSnapshot();
			SPI_finish();
			CommitTransactionCommand();

			elog(LOG, "auto_index: dropped index %s.%s",
				 d->schema, d->idxname);
		}
		PG_CATCH();
		{
			ErrorData  *edata;
			MemoryContext oldctx = MemoryContextSwitchTo(TopMemoryContext);

			edata = CopyErrorData();
			MemoryContextSwitchTo(oldctx);
			elog(WARNING, "auto_index: failed to drop index %s: %s",
				 d->idxname, edata->message);
			FreeErrorData(edata);
			FlushErrorState();
			AbortCurrentTransaction();
		}
		PG_END_TRY();
	}
}

PGDLLEXPORT void
auto_index_main(Datum main_arg)
{
	BackgroundWorkerInitializeConnection("postgres", NULL, 0);
	BackgroundWorkerUnblockSignals();

	elog(LOG, "auto_index worker started");

	while (true)
	{
		AutoIndexTableStats *snapshot;
		int					 rc;

		rc = WaitLatch(MyLatch,
					   WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH,
					   auto_index_check_interval * 1000L,
					   PG_WAIT_EXTENSION);
		ResetLatch(MyLatch);

		if (rc & WL_EXIT_ON_PM_DEATH)
			break;

		CHECK_FOR_INTERRUPTS();

		/* Snapshot shared memory while holding the lock as briefly as possible */
		snapshot = palloc(sizeof(auto_index_state->tables));
		LWLockAcquire(auto_index_state->lock, LW_SHARED);
		memcpy(snapshot, auto_index_state->tables, sizeof(auto_index_state->tables));
		LWLockRelease(auto_index_state->lock);

		PG_TRY();
		{
			evaluate_and_manage_indexes(snapshot);
		}
		PG_CATCH();
		{
			/* Log error and continue — the worker should not crash */
			elog(WARNING, "auto_index: evaluation pass failed: %m");
			FlushErrorState();
		}
		PG_END_TRY();

		pfree(snapshot);

		/* Reset interval counters so the next pass measures only what happens
		 * after this point.  Slot identity (relation_id, last_access_tick) and
		 * column attribute numbers are bookkeeping, not measurements — leave them. */
		{
			int		si,
					ci;

			LWLockAcquire(auto_index_state->lock, LW_EXCLUSIVE);
			for (si = 0; si < AUTO_INDEX_MAX_TABLES; si++)
			{
				AutoIndexTableStats *t = &auto_index_state->tables[si];

				t->insert_count = 0;
				t->update_count = 0;
				t->delete_count = 0;
				for (ci = 0; ci < AUTO_INDEX_MAX_COLS; ci++)
				{
					t->columns[ci].equality_hits = 0;
					t->columns[ci].range_hits    = 0;
				}
			}
			LWLockRelease(auto_index_state->lock);
		}
	}
}
