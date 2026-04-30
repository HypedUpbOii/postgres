#include "postgres.h"

#include <math.h>

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
#include "optimizer/cost.h"
#include "optimizer/optimizer.h"
#include "nodes/pathnodes.h"
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
int		auto_index_max_indexes_per_table = 6;

/*
 * Selectable create-decision strategy.  Default preserves the existing
 * ski-rental behaviour exactly; other values dispatch to the alternative
 * decide_create_* functions defined below.
 */
typedef enum AutoIndexCreateStrategy
{
	AI_CREATE_SKI_RENTAL = 0,		/* current default */
	AI_CREATE_SIMPLE_THRESHOLD,
	AI_CREATE_RATIO_ONLY,
	AI_CREATE_COST_GAIN,
	AI_CREATE_SIZE_GATED,
	AI_CREATE_ALWAYS,				/* fire on any cumulative_benefit > 0 */
} AutoIndexCreateStrategy;

int		auto_index_create_strategy		 = AI_CREATE_SKI_RENTAL;

/* Tunables for the alternative strategies — unused by ski_rental. */
double	auto_index_simple_threshold		 = 100.0;	/* "simple_threshold", "size_gated" */
double	auto_index_min_table_rows		 = 1000.0;	/* "size_gated" floor */

static const struct config_enum_entry auto_index_create_strategy_options[] = {
	{"ski_rental",		 AI_CREATE_SKI_RENTAL,		 false},
	{"simple_threshold", AI_CREATE_SIMPLE_THRESHOLD, false},
	{"ratio_only",		 AI_CREATE_RATIO_ONLY,		 false},
	{"cost_gain",		 AI_CREATE_COST_GAIN,		 false},
	{"size_gated",		 AI_CREATE_SIZE_GATED,		 false},
	{"always",			 AI_CREATE_ALWAYS,			 false},
	{NULL, 0, false},
};

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
static int	find_colset_slot(int tslot, const AttrNumber *attnos,
								 const int8 *ops, int n_cols);
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
		"Maximum number of auto-created indexes per table (singletons + composites combined).",
		NULL,
		&auto_index_max_indexes_per_table,
		6, 1, 32,
		PGC_SIGHUP,
		0, NULL, NULL, NULL);

	DefineCustomEnumVariable(
		"auto_index.create_strategy",
		"Policy used to decide when to create an index.",
		"ski_rental: cost-based ski rental (default).  "
		"simple_threshold: fire on cumulative_benefit >= auto_index.simple_threshold.  "
		"ratio_only: read/write ratio > auto_index.threshold.  "
		"cost_gain: projected savings - maintenance >= build_cost.  "
		"size_gated: simple_threshold gated by auto_index.min_table_rows.  "
		"always: fire on any cumulative_benefit > 0 (max aggressiveness).",
		&auto_index_create_strategy,
		AI_CREATE_SKI_RENTAL,
		auto_index_create_strategy_options,
		PGC_SIGHUP,
		0, NULL, NULL, NULL);

	DefineCustomRealVariable(
		"auto_index.simple_threshold",
		"Cumulative weighted hits required by the simple_threshold and "
		"size_gated create strategies.",
		NULL,
		&auto_index_simple_threshold,
		100.0, 1.0, 1.0e12,
		PGC_SIGHUP,
		0, NULL, NULL, NULL);

	DefineCustomRealVariable(
		"auto_index.min_table_rows",
		"Minimum reltuples for the size_gated create strategy to fire.",
		NULL,
		&auto_index_min_table_rows,
		1000.0, 0.0, 1.0e12,
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

/*
 * Find or allocate a colset slot for (sorted attnos, ops) inside table
 * slot tslot.  attnos must already be sorted ascending; ops must be in
 * the same order as attnos.  Returns -1 if all slots are full and no
 * LRU-evictable slot exists (shouldn't happen given LRU fallback, but
 * defensive).
 */
static int
find_colset_slot(int tslot, const AttrNumber *attnos,
				 const int8 *ops, int n_cols)
{
	AutoIndexColSet *slots = auto_index_state->tables[tslot].colsets;
	int		i, j;
	int		empty_slot = -1;
	int		lru_slot = -1;
	uint64	lru_time = UINT64_MAX;

	for (i = 0; i < AUTO_INDEX_MAX_COLSETS; i++)
	{
		AutoIndexColSet *cs = &slots[i];

		if (cs->n_cols == n_cols)
		{
			bool match = true;
			for (j = 0; j < n_cols; j++)
				if (cs->attnos[j] != attnos[j])
				{
					match = false;
					break;
				}
			if (match)
			{
				cs->last_access_tick = ++auto_index_state->access_counter;
				return i;
			}
		}
		if (cs->n_cols == 0 && empty_slot == -1)
			empty_slot = i;
		if (cs->last_access_tick < lru_time)
		{
			lru_time = cs->last_access_tick;
			lru_slot = i;
		}
	}

	i = (empty_slot != -1) ? empty_slot : lru_slot;
	if (i < 0)
		return -1;					/* defensive */

	memset(&slots[i], 0, sizeof(AutoIndexColSet));
	slots[i].n_cols = (int8) n_cols;
	for (j = 0; j < n_cols; j++)
	{
		slots[i].attnos[j] = attnos[j];
		slots[i].ops[j] = ops[j];
	}
	slots[i].last_access_tick = ++auto_index_state->access_counter;
	return i;
}


/* -------------------------------------------------------------------------
 * Executor hook — SeqScan qual extraction
 * -------------------------------------------------------------------------
 */

/*
 * Local accumulator: tracks the predicates observed in a single query's
 * qual list.  We collect first, then bump shared-memory counters under
 * the lock — that gives us a clean view of "these N columns appeared
 * together" which is what colset tracking needs.
 */
typedef struct LocalPred
{
	AttrNumber	attno;
	int8		op;					/* 0 = equality, 1 = range */
} LocalPred;

#define LOCAL_PRED_MAX	16			/* per-query distinct attrs we track */

typedef struct QualSet
{
	LocalPred	preds[LOCAL_PRED_MAX];
	int			n_preds;
} QualSet;

/* Add (attno, op) to qs, deduplicating by attno.  Equality wins over range
 * if the same column appears with both kinds of predicates. */
static void
qualset_add(QualSet *qs, AttrNumber attno, int8 op)
{
	int		i;

	if (!AttrNumberIsForUserDefinedAttr(attno))
		return;

	for (i = 0; i < qs->n_preds; i++)
	{
		if (qs->preds[i].attno == attno)
		{
			if (op == 0)
				qs->preds[i].op = 0;	/* upgrade range -> eq */
			return;
		}
	}
	if (qs->n_preds >= LOCAL_PRED_MAX)
		return;
	qs->preds[qs->n_preds].attno = attno;
	qs->preds[qs->n_preds].op = op;
	qs->n_preds++;
}

/*
 * Peel implicit-cast wrappers off an expression so the underlying Var
 * (if any) becomes visible.  RelabelType is the most common — it appears
 * around `col::text = 'x'` style predicates.
 */
static Expr *
strip_implicit_casts(Expr *e)
{
	while (e != NULL && IsA(e, RelabelType))
		e = ((RelabelType *) e)->arg;
	return e;
}

/*
 * Convert a postgres operator name to our 0=eq / 1=range encoding.
 * Returns -1 if the operator is something we don't track.
 */
static int8
opname_to_op(const char *opname)
{
	if (strcmp(opname, "=") == 0)
		return 0;
	if (strcmp(opname, "<")  == 0 || strcmp(opname, ">")  == 0 ||
		strcmp(opname, "<=") == 0 || strcmp(opname, ">=") == 0)
		return 1;
	return -1;
}

/*
 * Walk one expression node and accumulate every column predicate we can
 * recognise into qs.  Recurses into BoolExpr (AND/OR/NOT) so quals
 * nested inside disjunctions (e.g. TPC-H Q19) are still observed.
 */
static void
process_qual_node(QualSet *qs, Expr *expr)
{
	if (expr == NULL)
		return;

	/* AND / OR / NOT — recurse into each child. */
	if (IsA(expr, BoolExpr))
	{
		BoolExpr   *b = (BoolExpr *) expr;
		ListCell   *lc;

		foreach(lc, b->args)
			process_qual_node(qs, (Expr *) lfirst(lc));
		return;
	}

	/* col IN (...)  /  col = ANY(array)  — treat as equality observation. */
	if (IsA(expr, ScalarArrayOpExpr))
	{
		ScalarArrayOpExpr *saop = (ScalarArrayOpExpr *) expr;
		Expr	   *arg;
		char	   *opname;
		int8		op;

		if (list_length(saop->args) < 1)
			return;
		arg = strip_implicit_casts((Expr *) linitial(saop->args));
		if (!IsA(arg, Var))
			return;

		opname = get_opname(saop->opno);
		if (opname == NULL)
			return;
		op = opname_to_op(opname);
		pfree(opname);
		if (op < 0)
			return;

		qualset_add(qs, ((Var *) arg)->varattno, op);
		return;
	}

	/* Var <op> Const  /  Const <op> Var */
	if (IsA(expr, OpExpr))
	{
		OpExpr	   *opx = (OpExpr *) expr;
		Expr	   *left,
				   *right;
		AttrNumber	attno = InvalidAttrNumber;
		char	   *opname;
		int8		op;

		if (list_length(opx->args) < 2)
			return;					/* defensive: unary OpExpr */

		left  = strip_implicit_casts((Expr *) linitial(opx->args));
		right = strip_implicit_casts((Expr *) lsecond(opx->args));

		if (IsA(left, Var) && IsA(right, Const))
			attno = ((Var *) left)->varattno;
		else if (IsA(left, Const) && IsA(right, Var))
			attno = ((Var *) right)->varattno;
		else
			return;

		opname = get_opname(opx->opno);
		if (opname == NULL)
			return;
		op = opname_to_op(opname);
		pfree(opname);
		if (op < 0)
			return;

		qualset_add(qs, attno, op);
		return;
	}

	/*
	 * Other node types (FuncExpr casts, NullTest, CaseExpr, SubPlan, ...)
	 * — silently ignore.
	 */
}

/* Stable sort qs->preds by attno ascending — small N, insertion sort.
 * Used to derive the canonical lookup key for find_colset_slot. */
static void
qualset_sort_by_attno(QualSet *qs)
{
	int		i, j;
	for (i = 1; i < qs->n_preds; i++)
	{
		LocalPred key = qs->preds[i];
		j = i - 1;
		while (j >= 0 && qs->preds[j].attno > key.attno)
		{
			qs->preds[j + 1] = qs->preds[j];
			j--;
		}
		qs->preds[j + 1] = key;
	}
}

/* If we observed > AUTO_INDEX_COLSET_MAX_COLS user-attrs, reduce qs in
 * place to the top AUTO_INDEX_COLSET_MAX_COLS — equality preferred,
 * then by attno ascending.  Result is still sorted by attno. */
static void
qualset_trim_to_max(QualSet *qs)
{
	int		i, j;
	int		eq_count = 0;
	LocalPred picked[AUTO_INDEX_COLSET_MAX_COLS];
	int		n_picked = 0;

	if (qs->n_preds <= AUTO_INDEX_COLSET_MAX_COLS)
		return;

	/* Pass 1: take equality preds first. */
	for (i = 0; i < qs->n_preds && n_picked < AUTO_INDEX_COLSET_MAX_COLS; i++)
		if (qs->preds[i].op == 0)
		{
			picked[n_picked++] = qs->preds[i];
			eq_count++;
		}
	/* Pass 2: top up with range preds if we have room. */
	for (i = 0; i < qs->n_preds && n_picked < AUTO_INDEX_COLSET_MAX_COLS; i++)
		if (qs->preds[i].op == 1)
			picked[n_picked++] = qs->preds[i];

	/* Re-sort the picked ones by attno. */
	for (i = 1; i < n_picked; i++)
	{
		LocalPred key = picked[i];
		j = i - 1;
		while (j >= 0 && picked[j].attno > key.attno)
		{
			picked[j + 1] = picked[j];
			j--;
		}
		picked[j + 1] = key;
	}

	for (i = 0; i < n_picked; i++)
		qs->preds[i] = picked[i];
	qs->n_preds = n_picked;
	(void) eq_count;
}

/*
 * Process one SeqScan node: walk its qual list and record per-column
 * predicate hits AND multi-column co-occurrence in shared memory.
 *
 *   Step 1: walk the qual tree, accumulating distinct (attno, op) pairs
 *           into a local QualSet (no lock held).
 *   Step 2: under the lock, bump the singleton hit counter for every
 *           attr in the QualSet, AND if 2-3 distinct attrs were observed,
 *           bump the matching colset slot.
 */
static void
process_seqscan(SeqScanState *node)
{
	Relation	rel;
	Oid			relid;
	int			tslot;
	List	   *qual;
	ListCell   *lc;
	QualSet		qs;
	int			i;

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

	/* Step 1: accumulate predicates without holding the lock. */
	qs.n_preds = 0;
	foreach(lc, qual)
		process_qual_node(&qs, (Expr *) lfirst(lc));

	if (qs.n_preds == 0)
		return;

	qualset_sort_by_attno(&qs);
	qualset_trim_to_max(&qs);

	/* Step 2: apply to shared memory. */
	LWLockAcquire(auto_index_state->lock, LW_EXCLUSIVE);

	tslot = find_table_slot(relid);

	/* Bump singletons. */
	for (i = 0; i < qs.n_preds; i++)
	{
		int		cslot = find_col_slot(tslot, qs.preds[i].attno);
		if (cslot < 0)
			continue;
		if (qs.preds[i].op == 0)
			auto_index_state->tables[tslot].columns[cslot].equality_hits++;
		else
			auto_index_state->tables[tslot].columns[cslot].range_hits++;
	}

	/* Bump colset if 2+ distinct attrs.  attnos sorted ascending; ops
	 * aligned with attnos. */
	if (qs.n_preds >= 2 && qs.n_preds <= AUTO_INDEX_COLSET_MAX_COLS)
	{
		AttrNumber	attnos[AUTO_INDEX_COLSET_MAX_COLS];
		int8		ops[AUTO_INDEX_COLSET_MAX_COLS];
		int			cset;

		for (i = 0; i < qs.n_preds; i++)
		{
			attnos[i] = qs.preds[i].attno;
			ops[i] = qs.preds[i].op;
		}
		cset = find_colset_slot(tslot, attnos, ops, qs.n_preds);
		if (cset >= 0)
			auto_index_state->tables[tslot].colsets[cset].hits++;
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
	int			n_cols;								/* 1 = singleton, 2-3 = composite */
	AttrNumber	attnos[AUTO_INDEX_COLSET_MAX_COLS];	/* ordered for CREATE INDEX */
	char		schema[NAMEDATALEN];
	char		relname[NAMEDATALEN];
	char		attnames[AUTO_INDEX_COLSET_MAX_COLS][NAMEDATALEN];
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

/* -------------------------------------------------------------------------
 * Create-decision strategies
 *
 * Each decide_create_* function is a pure predicate over (column, table,
 * cost) state.  Returns true if the column qualifies for index creation
 * subject to the later checks (no existing index, max indexes, naming).
 *
 * The dispatcher decide_create() routes on auto_index.create_strategy.
 * The default branch is ski_rental, which mirrors the inline logic in
 * evaluate_and_manage_indexes — kept for parity with the existing code
 * path but only invoked when the dispatcher is called explicitly.
 * -------------------------------------------------------------------------
 */

typedef struct CreateContext
{
	const AutoIndexColumnStats *col;
	const AutoIndexTableStats  *tbl;
	int64	create_threshold;	/* ski-rental "buy after N queries" */
	double	seq_scan_cost;
	double	build_cost;
	double	maint_per_write;
	double	reltuples;
	double	relpages;
} CreateContext;

/*
 * Strategy A — ski rental (matches the inline checks in
 * evaluate_and_manage_indexes).  Provided as a function so callers can
 * dispatch uniformly.
 */
static bool
decide_create_ski_rental(const CreateContext *cc)
{
	double	cum_ratio;

	if (cc->col->cumulative_benefit < cc->create_threshold)
		return false;

	cum_ratio = (double) cc->col->cumulative_benefit
				/ Max(1, cc->tbl->cumulative_write_cost);
	if (cum_ratio <= auto_index_threshold)
		return false;

	return true;
}

/*
 * Strategy B — simple threshold.  Fires when the cumulative weighted
 * benefit exceeds auto_index.simple_threshold.  No cost model, no
 * write-ratio guard.  Useful for benchmarks where you want a predictable
 * trigger.
 */
static bool
decide_create_simple_threshold(const CreateContext *cc)
{
	return cc->col->cumulative_benefit >= (int64) auto_index_simple_threshold;
}

/*
 * Strategy C — ratio only.  Read/write ratio above auto_index.threshold,
 * provided there is at least one read.  Ignores absolute volume, so it
 * fires on read-mostly tables even if traffic is light.
 */
static bool
decide_create_ratio_only(const CreateContext *cc)
{
	double	ratio;

	if (cc->col->cumulative_benefit == 0)
		return false;

	ratio = (double) cc->col->cumulative_benefit
			/ Max(1, cc->tbl->cumulative_write_cost);
	return ratio > auto_index_threshold;
}

/*
 * Strategy D — cost-gain projection.
 *
 *   savings_per_query ≈ seq_scan_cost − idx_lookup_cost
 *   total_savings     = cumulative_benefit × savings_per_query
 *   total_maint       = cumulative_write_cost × maint_per_write
 *
 * Create when (total_savings − total_maint) >= build_cost: i.e. the
 * projected net benefit pays off the index build.  More aggressive than
 * ski rental on high seq/idx cost gaps; more conservative on tables with
 * heavy write traffic.
 */
static bool
decide_create_cost_gain(const CreateContext *cc)
{
	double	idx_lookup_cost;
	double	savings_per_query;
	double	total_savings;
	double	total_maint;

	if (cc->col->cumulative_benefit == 0)
		return false;

	/* Approximate index-scan cost: B-tree descent + one heap fetch. */
	idx_lookup_cost = log2(Max(2.0, cc->reltuples)) * 2.0 * cpu_operator_cost
					  + random_page_cost;
	savings_per_query = cc->seq_scan_cost - idx_lookup_cost;

	if (savings_per_query <= 0)
		return false;

	total_savings = (double) cc->col->cumulative_benefit * savings_per_query;
	total_maint   = (double) cc->tbl->cumulative_write_cost * cc->maint_per_write;

	return (total_savings - total_maint) >= cc->build_cost;
}

/*
 * Strategy E — size gated.  Like simple_threshold but only fires once
 * the table is at least auto_index.min_table_rows rows.  Avoids creating
 * indexes on tiny lookup tables where seq scan is already optimal.
 */
static bool
decide_create_size_gated(const CreateContext *cc)
{
	if (cc->reltuples < auto_index_min_table_rows)
		return false;
	return cc->col->cumulative_benefit >= (int64) auto_index_simple_threshold;
}

/*
 * Strategy F — always.  Fire on any positive cumulative benefit.  Useful
 * as the "maximum aggressiveness" upper bound: shows what happens when
 * we index every column auto_index has ever seen.  Subject to the
 * post-checks (no existing index, max_indexes_per_table) — those still
 * limit total fan-out.
 */
static bool
decide_create_always(const CreateContext *cc)
{
	return cc->col->cumulative_benefit > 0;
}

/*
 * Dispatcher — routes on the auto_index.create_strategy GUC.
 */
static bool
decide_create(const CreateContext *cc)
{
	switch (auto_index_create_strategy)
	{
		case AI_CREATE_SIMPLE_THRESHOLD:
			return decide_create_simple_threshold(cc);
		case AI_CREATE_RATIO_ONLY:
			return decide_create_ratio_only(cc);
		case AI_CREATE_COST_GAIN:
			return decide_create_cost_gain(cc);
		case AI_CREATE_SIZE_GATED:
			return decide_create_size_gated(cc);
		case AI_CREATE_ALWAYS:
			return decide_create_always(cc);
		case AI_CREATE_SKI_RENTAL:
		default:
			return decide_create_ski_rental(cc);
	}
}

/*
 * Reorder a colset for the actual CREATE INDEX:
 *   1. Equality columns first (most restrictive predicates lead — also
 *      the only way the planner can use later columns of a B-tree).
 *   2. Range columns next.
 *   3. Within each group, ascending by attno (deterministic naming).
 *
 * out_attnos / out_ops must each have room for cs->n_cols entries.
 */
static void
reorder_colset_for_index(const AutoIndexColSet *cs,
						 AttrNumber *out_attnos, int8 *out_ops)
{
	int		i, k = 0;

	/* Equality first */
	for (i = 0; i < cs->n_cols; i++)
		if (cs->ops[i] == 0)
		{
			out_attnos[k] = cs->attnos[i];
			out_ops[k] = 0;
			k++;
		}
	/* Range last */
	for (i = 0; i < cs->n_cols; i++)
		if (cs->ops[i] == 1)
		{
			out_attnos[k] = cs->attnos[i];
			out_ops[k] = 1;
			k++;
		}
}

/*
 * Subsumption check.  Returns true if (relid, attno) is the LEADING
 * column of a composite already queued in to_create[].  When the
 * composite exists the planner can serve `WHERE col = ?` queries via
 * leftmost-prefix, so a separate singleton on that column is redundant.
 */
static bool
is_attno_covered_by_pending_composite(const CreateDecision *to_create,
									  int n_create,
									  Oid relid, AttrNumber attno)
{
	int		i;
	for (i = 0; i < n_create; i++)
	{
		if (to_create[i].relid == relid &&
			to_create[i].n_cols >= 2 &&
			to_create[i].attnos[0] == attno)
			return true;
	}
	return false;
}

/*
 * Core evaluation loop.  snapshot is a local copy of the shared memory
 * tables array taken before any SPI work.
 *
 * Phase 1: run SELECTs inside a normal transaction to gather decisions.
 *          Composites are evaluated FIRST so their leading columns can
 *          subsume singleton candidates in the second loop.
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
		int		ret;

		/* Per-table metadata resolved once and reused for both CREATE and DROP */
		char   *schema = NULL;
		char   *relname = NULL;
		double	reltuples,
				relpages;

		/*
		 * Ski rental cost model (PostgreSQL planner units):
		 *
		 *   seq_scan_cost  = cost of one full table scan
		 *   build_cost     = cost of building a B-tree index
		 *                    (heap read + sort comparisons)
		 *   maint_per_write = cost to maintain the index for one write
		 *                    (log2(N) comparisons + random page write)
		 *
		 * create_threshold  = queries needed before "buying" the index
		 *                   = ceil(build_cost / seq_scan_cost)
		 *
		 * drop_threshold    = build_cost * 1000  (scaled to fit int64)
		 * idle accumulator  = sum over idle intervals of:
		 *                     (writes * maint_per_write + seq_scan_cost) * 1000
		 *   The seq_scan_cost term is an opportunity-cost floor that ensures
		 *   even zero-write tables eventually drop an unused index after
		 *   create_threshold idle intervals.
		 */
		int64	create_threshold;
		int64	drop_threshold;
		double	seq_scan_cost_val;
		double	build_cost_val;
		double	maint_per_write;

		/* interval write count still available in snapshot before zeroing */
		int64	interval_write_cost =
			t->insert_count + t->update_count + t->delete_count;

		if (!OidIsValid(t->relation_id))
			continue;

		/* ------------------------------------------------------------------
		 * Resolve table metadata and compute cost-based thresholds once.
		 * ------------------------------------------------------------------
		 */
		{
			Oid		meta_argtypes[1] = {OIDOID};
			Datum	meta_args[1]	 = {ObjectIdGetDatum(t->relation_id)};
			char	meta_nulls[1]	 = {' '};
			bool	isnull_meta;
			int		ret_meta;

			ret_meta = SPI_execute_with_args(
				"SELECT n.nspname, c.relname, "
				"       greatest(c.reltuples::float8, 2.0), "
				"       greatest(c.relpages::float8,  1.0) "
				"FROM pg_class c "
				"JOIN pg_namespace n ON n.oid = c.relnamespace "
				"WHERE c.oid = $1",
				1, meta_argtypes, meta_args, meta_nulls, true, 1);

			if (ret_meta != SPI_OK_SELECT || SPI_processed == 0)
				continue;

			schema    = SPI_getvalue(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 1);
			relname   = SPI_getvalue(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 2);
			reltuples = DatumGetFloat8(SPI_getbinval(SPI_tuptable->vals[0],
								SPI_tuptable->tupdesc, 3, &isnull_meta));
			relpages  = DatumGetFloat8(SPI_getbinval(SPI_tuptable->vals[0],
								SPI_tuptable->tupdesc, 4, &isnull_meta));
		}

		/*
		 * Compute cost-based ski rental thresholds using PostgreSQL's own
		 * cost functions rather than hand-rolled formulas.
		 *
		 * cost_seqscan() gives the planner's actual estimate for one full
		 * table scan, respecting tablespace cost settings and all CPU costs.
		 *
		 * cost_sort() gives the planner's estimate for sorting reltuples
		 * rows, which dominates B-tree index build cost (the executor sorts
		 * the heap before writing the index in page order).  We use
		 * maintenance_work_mem because that is what CREATE INDEX actually
		 * uses for its sort buffers.
		 *
		 * maint_per_write uses cpu_index_tuple_cost (the planner's cost
		 * per index tuple insertion) plus a random I/O charge — identical
		 * to how the planner models ongoing B-tree maintenance overhead.
		 */
		{
			RelOptInfo	rel_info;
			PathTarget	tgt;
			Path		seq_path;
			Path		sort_path;

			memset(&rel_info,  0, sizeof(rel_info));
			memset(&tgt,	   0, sizeof(tgt));
			memset(&seq_path,  0, sizeof(seq_path));
			memset(&sort_path, 0, sizeof(sort_path));

			/* Minimal fields required by cost_seqscan assertions */
			rel_info.relid		  = 1;		/* > 0 to satisfy Assert */
			rel_info.rtekind	  = RTE_RELATION;
			rel_info.reltablespace = 0;		/* default tablespace */
			rel_info.pages		  = (BlockNumber) relpages;
			rel_info.tuples		  = reltuples;
			rel_info.rows		  = reltuples;

			/* cost_seqscan reads path->pathtarget->cost — must be non-NULL */
			seq_path.pathtarget = &tgt;

			cost_seqscan(&seq_path, NULL, &rel_info, NULL);
			seq_scan_cost_val = seq_path.total_cost;

			/*
			 * Index build cost ≈ sort cost with seq scan as input.
			 * input_disabled_nodes=0: we are not in a disabled plan context.
			 */
			cost_sort(&sort_path, NULL,
					  NIL,					/* pathkeys — unused by cost_sort */
					  0,					/* input_disabled_nodes */
					  seq_scan_cost_val,	/* cost to read the heap */
					  reltuples,
					  8,					/* avg index entry width (bytes) */
					  2.0 * cpu_operator_cost,	/* comparison cost */
					  maintenance_work_mem,
					  -1.0);				/* no row limit */
			build_cost_val = sort_path.total_cost;

			/*
			 * Per-write index maintenance cost: one index tuple insertion
			 * (cpu_index_tuple_cost) plus one random page write (amortised
			 * to random_page_cost).  This matches how the planner models
			 * write amplification for maintained B-tree indexes.
			 */
			maint_per_write = cpu_index_tuple_cost + random_page_cost;
		}

		create_threshold = (int64) Max(2, ceil(build_cost_val /
								Max(seq_scan_cost_val, 1.0)));
		drop_threshold   = (int64) (build_cost_val * 1000.0);

		/*
		 * CREATE candidates — composites FIRST, then singletons.
		 *
		 * A composite (a, b, c) is strictly superior to a singleton on the
		 * leading column `a` for any query that filters on `a` alone (B-tree
		 * leftmost prefix), so we want composites to win when both qualify.
		 * Singletons on non-leading columns of the composite are NOT
		 * suppressed — those queries can't use the composite.
		 */
		{
			int		cs_idx;

			/* === Composite candidates === */
			for (cs_idx = 0;
				 cs_idx < AUTO_INDEX_MAX_COLSETS && n_create < MAX_DECISIONS;
				 cs_idx++)
			{
				AutoIndexColSet	   *cs = &t->colsets[cs_idx];
				AttrNumber			ord_attnos[AUTO_INDEX_COLSET_MAX_COLS];
				int8				ord_ops[AUTO_INDEX_COLSET_MAX_COLS];
				bool				skip = false;
				int					ci;
				CreateDecision	   *d;
				char				attno_list[64];

				if (cs->n_cols < 2)
					continue;

				/* Strategy gate.  ski_rental keeps its original two-check
				 * cost+ratio test inline; other strategies use a synthetic
				 * column-stats so the existing decide_create() works. */
				if (auto_index_create_strategy == AI_CREATE_SKI_RENTAL)
				{
					double	cum_ratio;

					if (cs->cumulative_benefit < create_threshold)
						continue;
					cum_ratio = (double) cs->cumulative_benefit
								/ Max(1, t->cumulative_write_cost);
					if (cum_ratio <= auto_index_threshold)
						continue;
				}
				else
				{
					AutoIndexColumnStats	syn;
					CreateContext			cc;

					memset(&syn, 0, sizeof(syn));
					syn.attribute_number	= cs->attnos[0];
					syn.cumulative_benefit	= cs->cumulative_benefit;

					cc.col				= &syn;
					cc.tbl				= t;
					cc.create_threshold = create_threshold;
					cc.seq_scan_cost	= seq_scan_cost_val;
					cc.build_cost		= build_cost_val;
					cc.maint_per_write	= maint_per_write;
					cc.reltuples		= reltuples;
					cc.relpages			= relpages;

					if (!decide_create(&cc))
						continue;
				}

				if (count_auto_indexes(t->relation_id) >=
					auto_index_max_indexes_per_table)
					break;					/* per-table cap reached */

				/* Reorder for B-tree leading-column priority. */
				reorder_colset_for_index(cs, ord_attnos, ord_ops);
				(void) attno_list;

				d = &to_create[n_create];
				d->relid	= t->relation_id;
				d->n_cols	= cs->n_cols;
				for (ci = 0; ci < cs->n_cols; ci++)
					d->attnos[ci] = ord_attnos[ci];
				strlcpy(d->schema,	schema,		NAMEDATALEN);
				strlcpy(d->relname, relname,	NAMEDATALEN);

				/* Resolve column names individually.  If any lookup fails
				 * we abandon the whole composite to avoid partial state. */
				for (ci = 0; ci < cs->n_cols; ci++)
				{
					Oid		argtypes[2] = {OIDOID, INT2OID};
					Datum	args[2]		= {ObjectIdGetDatum(t->relation_id),
										   Int16GetDatum(d->attnos[ci])};
					char	nulls[2]	= {' ', ' '};
					int		ret2;
					char   *attname;

					ret2 = SPI_execute_with_args(
						"SELECT attname FROM pg_attribute "
						"WHERE attrelid = $1 AND attnum = $2 AND NOT attisdropped",
						2, argtypes, args, nulls, true, 1);
					if (ret2 != SPI_OK_SELECT || SPI_processed == 0)
					{
						skip = true;
						break;
					}
					attname = SPI_getvalue(SPI_tuptable->vals[0],
										   SPI_tuptable->tupdesc, 1);
					strlcpy(d->attnames[ci], attname, NAMEDATALEN);
				}
				if (skip)
					continue;

				/* Index name: auto_idx_m_<relid>_<attnos…> */
				if (cs->n_cols == 2)
					snprintf(d->idxname, NAMEDATALEN,
							 "auto_idx_m_%u_%d_%d",
							 t->relation_id, d->attnos[0], d->attnos[1]);
				else
					snprintf(d->idxname, NAMEDATALEN,
							 "auto_idx_m_%u_%d_%d_%d",
							 t->relation_id, d->attnos[0], d->attnos[1], d->attnos[2]);

				n_create++;
			}

			/* === Singleton candidates === */
			for (c = 0; c < AUTO_INDEX_MAX_COLS && n_create < MAX_DECISIONS; c++)
			{
				AutoIndexColumnStats   *col = &t->columns[c];
				double					cum_ratio;
				int						ret2;

				if (!AttrNumberIsForUserDefinedAttr(col->attribute_number))
					continue;

				/* Subsumption: leading column of a composite we already
				 * queued.  Don't create the singleton; the composite will
				 * serve `WHERE col = ?` via leftmost prefix. */
				if (is_attno_covered_by_pending_composite(
						to_create, n_create,
						t->relation_id, col->attribute_number))
					continue;

				/* Strategy dispatch.  ski_rental keeps original inline
				 * behaviour; others go through decide_create(). */
				if (auto_index_create_strategy == AI_CREATE_SKI_RENTAL)
				{
					if (col->cumulative_benefit < create_threshold)
						continue;
					cum_ratio = (double) col->cumulative_benefit
								/ Max(1, t->cumulative_write_cost);
					if (cum_ratio <= auto_index_threshold)
						continue;
				}
				else
				{
					CreateContext cc;

					cc.col				= col;
					cc.tbl				= t;
					cc.create_threshold = create_threshold;
					cc.seq_scan_cost	= seq_scan_cost_val;
					cc.build_cost		= build_cost_val;
					cc.maint_per_write	= maint_per_write;
					cc.reltuples		= reltuples;
					cc.relpages			= relpages;

					if (!decide_create(&cc))
						continue;
				}

				if (index_exists_for_column(t->relation_id, col->attribute_number))
					continue;
				if (count_auto_indexes(t->relation_id) >=
					auto_index_max_indexes_per_table)
					continue;

				/* Resolve column name */
				{
					Oid		col_argtypes[2] = {OIDOID, INT2OID};
					Datum	col_args[2]		= {ObjectIdGetDatum(t->relation_id),
										   Int16GetDatum(col->attribute_number)};
					char	col_nulls[2]	= {' ', ' '};
					char   *attname;
					CreateDecision *d = &to_create[n_create];

					ret2 = SPI_execute_with_args(
						"SELECT attname FROM pg_attribute "
						"WHERE attrelid = $1 AND attnum = $2 AND NOT attisdropped",
						2, col_argtypes, col_args, col_nulls, true, 1);
					if (ret2 != SPI_OK_SELECT || SPI_processed == 0)
						continue;

					attname = SPI_getvalue(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 1);

					d->relid	 = t->relation_id;
					d->n_cols	 = 1;
					d->attnos[0] = col->attribute_number;
					strlcpy(d->schema,		schema,  NAMEDATALEN);
					strlcpy(d->relname,		relname, NAMEDATALEN);
					strlcpy(d->attnames[0], attname, NAMEDATALEN);
					snprintf(d->idxname, NAMEDATALEN, "auto_idx_%u_%d",
							 t->relation_id, col->attribute_number);
					n_create++;
				}
			}
		}

		/* DROP candidates: ski rental on idle maintenance cost */
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
				"SELECT a.indexrelid, a.last_checked_idx_scan, a.idle_write_cost, "
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
					int64	last_scan, cur_scan, idle_cost, new_idle;
					char   *idx_schema, *idxname;
					bool	isnull2;
					int		ret3;
					Oid		scan_argtypes[1] = {OIDOID};
					Datum	scan_args[1];
					char	scan_nulls[1]	 = {' '};

					indexrelid = DatumGetObjectId(
						SPI_getbinval(tbl->vals[r], tbl->tupdesc, 1, &isnull2));
					last_scan  = DatumGetInt64(
						SPI_getbinval(tbl->vals[r], tbl->tupdesc, 2, &isnull2));
					idle_cost  = DatumGetInt64(
						SPI_getbinval(tbl->vals[r], tbl->tupdesc, 3, &isnull2));
					idx_schema = SPI_getvalue(tbl->vals[r], tbl->tupdesc, 4);
					idxname    = SPI_getvalue(tbl->vals[r], tbl->tupdesc, 5);

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

					if (last_scan == -1)
					{
						/*
						 * Sentinel: first evaluation after creation.  Just
						 * record the baseline scan count; don't start the ski
						 * rental idle accumulator yet.
						 */
						Oid		upd_t[2]  = {INT8OID, OIDOID};
						Datum	upd_a[2]  = {Int64GetDatum(cur_scan),
											 ObjectIdGetDatum(indexrelid)};
						char	upd_n[2]  = {' ', ' '};

						SPI_execute_with_args(
							"UPDATE auto_index_catalog "
							"SET last_checked_idx_scan = $1, "
							"    last_checked_at = now(), "
							"    idle_write_cost = 0 "
							"WHERE indexrelid = $2",
							2, upd_t, upd_a, upd_n, false, 0);
						continue;
					}

					if (cur_scan > last_scan)
					{
						/*
						 * Index was actually used this interval: reset the
						 * idle accumulator — the ski rental clock restarts.
						 */
						Oid		upd_t[2]  = {INT8OID, OIDOID};
						Datum	upd_a[2]  = {Int64GetDatum(cur_scan),
											 ObjectIdGetDatum(indexrelid)};
						char	upd_n[2]  = {' ', ' '};

						SPI_execute_with_args(
							"UPDATE auto_index_catalog "
							"SET last_checked_idx_scan = $1, "
							"    last_checked_at = now(), "
							"    idle_write_cost = 0 "
							"WHERE indexrelid = $2",
							2, upd_t, upd_a, upd_n, false, 0);
					}
					else
					{
						/*
						 * Ski rental drop: accumulate idle maintenance cost.
						 *
						 * Each idle interval contributes:
						 *   writes * maint_per_write + seq_scan_cost
						 *
						 * The seq_scan_cost floor is the opportunity cost of
						 * holding an unused index slot; it ensures zero-write
						 * tables also age out after create_threshold intervals.
						 *
						 * All values are scaled by 1000 to keep precision in
						 * the bigint catalog column.
						 */
						double	contribution =
							(interval_write_cost * maint_per_write
							 + seq_scan_cost_val) * 1000.0;

						new_idle = idle_cost + (int64) contribution;

						{
							Oid		upd_t[3]  = {INT8OID, OIDOID, INT8OID};
							Datum	upd_a[3]  = {Int64GetDatum(cur_scan),
												 ObjectIdGetDatum(indexrelid),
												 Int64GetDatum(new_idle)};
							char	upd_n[3]  = {' ', ' ', ' '};

							SPI_execute_with_args(
								"UPDATE auto_index_catalog "
								"SET last_checked_idx_scan = $1, "
								"    last_checked_at = now(), "
								"    idle_write_cost = $3 "
								"WHERE indexrelid = $2",
								3, upd_t, upd_a, upd_n, false, 0);
						}

						if (new_idle >= drop_threshold && n_drop < MAX_DECISIONS)
						{
							DropDecision *d = &to_drop[n_drop];

							d->indexrelid = indexrelid;
							strlcpy(d->schema,  idx_schema, NAMEDATALEN);
							strlcpy(d->idxname, idxname,    NAMEDATALEN);
							n_drop++;
						}
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
		char	collist[NAMEDATALEN * AUTO_INDEX_COLSET_MAX_COLS + 16];
		char	attnos_literal[64];
		int		k;

		/* Build "col1, col2, col3" for CREATE INDEX. */
		{
			int written = 0;
			for (k = 0; k < d->n_cols; k++)
			{
				written += snprintf(collist + written,
									sizeof(collist) - written,
									"%s%s",
									(k == 0 ? "" : ", "),
									d->attnames[k]);
			}
		}

		/* Build "ARRAY[a, b, c]::smallint[]" for catalog INSERT. */
		{
			int written = snprintf(attnos_literal, sizeof(attnos_literal),
								   "ARRAY[%d", d->attnos[0]);
			for (k = 1; k < d->n_cols; k++)
				written += snprintf(attnos_literal + written,
									sizeof(attnos_literal) - written,
									", %d", d->attnos[k]);
			snprintf(attnos_literal + written,
					 sizeof(attnos_literal) - written,
					 "]::smallint[]");
		}

		snprintf(sql, sizeof(sql),
				 "CREATE INDEX IF NOT EXISTS %s ON %s.%s (%s)",
				 d->idxname, d->schema, d->relname, collist);

		PG_TRY();
		{
			SetCurrentStatementStartTimestamp();
			StartTransactionCommand();
			SPI_connect();
			PushActiveSnapshot(GetTransactionSnapshot());

			SPI_execute(sql, false, 0);

			/* Record in catalog using INSERT...SELECT to avoid a separate
			 * OID lookup — the index is visible in pg_class within the same
			 * transaction so this finds it and inserts in one statement.
			 *
			 * The attnos array is interpolated into the SQL text directly
			 * (small ints from internal sources, no injection risk) because
			 * SPI_execute_with_args has no convenient way to pass an array
			 * literal as a single bind. */
			{
				char	insert_sql[768];
				Oid		argtypes[2] = {OIDOID, NAMEOID};
				Datum	args[2]		= {ObjectIdGetDatum(d->relid),
									   DirectFunctionCall1(namein,
										   CStringGetDatum(d->idxname))};
				char	nulls[2]	= {' ', ' '};

				snprintf(insert_sql, sizeof(insert_sql),
					"INSERT INTO auto_index_catalog "
					"    (relid, indexrelid, attnos, created_at, last_checked_idx_scan) "
					"SELECT $1, c.oid, %s, now(), -1 "
					"FROM   pg_class c "
					"JOIN   pg_namespace n ON n.oid = c.relnamespace "
					"WHERE  c.relname = $2 AND n.nspname = 'public' "
					"ON CONFLICT DO NOTHING",
					attnos_literal);

				/* Use -1 as the initial baseline so the first DROP evaluation
				 * sees cur_scan(0) != last_scan(-1) and skips the drop.
				 * Only after one full interval with no usage will it drop. */
				SPI_execute_with_args(insert_sql,
									  2, argtypes, args, nulls, false, 0);
			}

			PopActiveSnapshot();
			SPI_finish();
			CommitTransactionCommand();

			/*
			 * Reset cumulative_benefit so this column / colset doesn't
			 * immediately re-qualify on the next pass.  For singletons we
			 * reset the column slot; for composites we reset the matching
			 * colset slot AND the leading column's singleton (since the
			 * composite serves it via leftmost-prefix).
			 */
			{
				int si, ci, csi, m;

				LWLockAcquire(auto_index_state->lock, LW_EXCLUSIVE);
				for (si = 0; si < AUTO_INDEX_MAX_TABLES; si++)
				{
					AutoIndexTableStats *t;

					if (auto_index_state->tables[si].relation_id != d->relid)
						continue;
					t = &auto_index_state->tables[si];

					if (d->n_cols == 1)
					{
						for (ci = 0; ci < AUTO_INDEX_MAX_COLS; ci++)
							if (t->columns[ci].attribute_number == d->attnos[0])
							{
								t->columns[ci].cumulative_benefit = 0;
								break;
							}
					}
					else
					{
						/* Reset matching colset (any-order match on the set
						 * of attnos). */
						for (csi = 0; csi < AUTO_INDEX_MAX_COLSETS; csi++)
						{
							AutoIndexColSet *cs = &t->colsets[csi];
							bool match;

							if (cs->n_cols != d->n_cols)
								continue;
							match = true;
							for (m = 0; m < d->n_cols && match; m++)
							{
								int n;
								bool found = false;
								for (n = 0; n < cs->n_cols; n++)
									if (cs->attnos[n] == d->attnos[m])
									{
										found = true;
										break;
									}
								if (!found)
									match = false;
							}
							if (match)
								cs->cumulative_benefit = 0;
						}
						/* Reset leading column singleton too. */
						for (ci = 0; ci < AUTO_INDEX_MAX_COLS; ci++)
							if (t->columns[ci].attribute_number == d->attnos[0])
							{
								t->columns[ci].cumulative_benefit = 0;
								break;
							}
					}
					break;
				}
				LWLockRelease(auto_index_state->lock);
			}

			elog(LOG, "auto_index: created %s index %s on %s.%s (%s)",
				 (d->n_cols == 1 ? "single-col" : "composite"),
				 d->idxname, d->schema, d->relname, collist);
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

		/*
		 * Fold interval counters into cumulative totals BEFORE taking the
		 * snapshot, so this pass's evaluate sees the workload that just
		 * happened.  cumulative_benefit and cumulative_write_cost persist
		 * across intervals — they implement the ski rental "rent
		 * accumulator" and are only reset by auto_index_reset() or when an
		 * index is successfully created for a column.
		 *
		 * Interval counters (insert/update/delete_count, equality_hits,
		 * range_hits) are left non-zero through the snapshot so the drop
		 * logic in evaluate_and_manage_indexes can read interval_write_cost
		 * for the current interval.  They are zeroed in shared memory
		 * before releasing the lock.
		 */
		snapshot = palloc(sizeof(auto_index_state->tables));
		{
			int		si,
					ci;

			LWLockAcquire(auto_index_state->lock, LW_EXCLUSIVE);

			/* Phase A: fold interval -> cumulative */
			for (si = 0; si < AUTO_INDEX_MAX_TABLES; si++)
			{
				AutoIndexTableStats *t = &auto_index_state->tables[si];
				int		csi;

				t->cumulative_write_cost += t->insert_count + t->update_count + t->delete_count;
				for (ci = 0; ci < AUTO_INDEX_MAX_COLS; ci++)
				{
					t->columns[ci].cumulative_benefit +=
						t->columns[ci].equality_hits * 2 + t->columns[ci].range_hits;
				}
				for (csi = 0; csi < AUTO_INDEX_MAX_COLSETS; csi++)
				{
					/* Composite benefit weight: each hit reflects ALL the
					 * columns that came together, so it's worth more than
					 * a single-column hit.  Use n_cols × hits to bias
					 * toward composites the planner can actually exploit. */
					AutoIndexColSet *cs = &t->colsets[csi];
					if (cs->n_cols == 0)
						continue;
					cs->cumulative_benefit += cs->hits * cs->n_cols;
				}
			}

			/* Phase B: snapshot — captures updated cumulative + original interval */
			memcpy(snapshot, auto_index_state->tables, sizeof(auto_index_state->tables));

			/* Phase C: zero the interval counters in shared memory */
			for (si = 0; si < AUTO_INDEX_MAX_TABLES; si++)
			{
				AutoIndexTableStats *t = &auto_index_state->tables[si];
				int		csi;

				t->insert_count = 0;
				t->update_count = 0;
				t->delete_count = 0;
				for (ci = 0; ci < AUTO_INDEX_MAX_COLS; ci++)
				{
					t->columns[ci].equality_hits = 0;
					t->columns[ci].range_hits    = 0;
				}
				for (csi = 0; csi < AUTO_INDEX_MAX_COLSETS; csi++)
					t->colsets[csi].hits = 0;
			}

			LWLockRelease(auto_index_state->lock);
		}

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
	}
}
