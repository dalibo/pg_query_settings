/*-------------------------------------------------------------------------------------------------
 *
 * pg_query_settings.c
 *		Modify one or more GUC parameters on the fly for some queries, based on their query ID.
 *
 *
 * Copyright (c) 2022-2023, Dalibo (Franck Boudehen, Frédéric Yhuel, Guillaume Lelarge, Thibaud Walkowiak)
 *
 *-------------------------------------------------------------------------------------------------
 */
#ident "pg_query_settings version 0.1"

/* Headers */

#include <postgres.h>

#include <access/heapam.h>
#include <catalog/namespace.h>
#include <miscadmin.h>
#include <executor/executor.h>
#include <optimizer/planner.h>
#include <storage/bufmgr.h>
#include <utils/builtins.h>
#include <utils/guc.h>
#include <lib/ilist.h>

#include "pgsp_queryid.h"

/* This is a module :) */

PG_MODULE_MAGIC;

/* Macro definitions */

#if PG_VERSION_NUM < 140000
#define COMPUTE_LOCAL_QUERYID 1
#else
#define COMPUTE_LOCAL_QUERYID 0
#endif

/* Function definitions */

void _PG_init(void);
void _PG_fini(void);

/* Variables */

static bool    enabled = true;
static bool    debug = false;
static bool    printQueryId = false;
static slist_head paramResetList = SLIST_STATIC_INIT(paramResetList);

/* Constants */
/* Name of our config table */
static const char* pgqs_config ="pgqs_config";

/* Parameter struct */
typedef struct parameter
{
  char *name;
  slist_node node;
} parameter;

/* Our hooks on PostgreSQL */
static planner_hook_type prevHook  = NULL;
static ExecutorEnd_hook_type prev_ExecutorEnd = NULL;

/* Functions */

/*
 * Destroy the list of parameters.
 * If 'reset' is true, then restore value of each parameter.
 */
static void DestroyPRList(bool reset)
{
  slist_mutable_iter  iter;
  parameter   *param;

  if (debug) elog(DEBUG1, "Destroy paramResetList");

  slist_foreach_modify(iter, &paramResetList)
  {
    param = slist_container(parameter, node, iter.cur);

    if (reset)
    {
      if (debug) elog(DEBUG1, "Reset guc %s", param->name);

      SetConfigOption(param->name, NULL, PGC_USERSET, PGC_S_SESSION);
    }
    slist_delete_current(&iter);
    free(param);
  }
}

/*
 * planner hook function: this is where we set all our GUC if we find a
 * configuration for the query
 *
 * The function scans our 'pgqs_config' table, and for each matching tuple, we
 * call SetConfigOption() to set the runtime parameter.
 * We also build a list of these parameters so that we can restore them to their
 * default values afterwards.
 */
static PlannedStmt *
execPlantuner(Query *parse, const char *query_st, int cursorOptions, ParamListInfo boundp)
{
  PlannedStmt    *result;
  Relation       config_rel;
  HeapTuple      config_tuple;
  Oid            config_relid;
  TableScanDesc  config_scan;
  Datum          data;
  bool           isnull;
  bool           rethrow = false;
  int64          id;
  char           *guc_value = NULL;
  char           *guc_name = NULL;
  parameter      *param = NULL;
  uint64          queryid = 0;

  if (enabled)
  {
#if COMPUTE_LOCAL_QUERYID
    queryid = hash_query(query_st);
#else
    queryid = parse->queryId;
#endif

    if (printQueryId) elog(NOTICE, "QueryID is '%li'", queryid);

    if (debug) elog(DEBUG1, "query's QueryID is '%li'", queryid);

    config_relid = RelnameGetRelid(pgqs_config);
    config_rel = table_open(config_relid, AccessShareLock);

    config_scan = table_beginscan(config_rel, GetActiveSnapshot(), 0, NULL);

    while ((config_tuple = heap_getnext(config_scan, ForwardScanDirection)) != NULL)
    {
      /* Get the queryid in the currently read tuple. */
      data = heap_getattr(config_tuple, 1, config_rel->rd_att, &isnull);
      id = DatumGetInt64(data);
      if (debug) elog(DEBUG1, "config QueryID is '%li'", id);

      /* Compare the queryid previously obtained with the queryid
       * of the current query. */
      if (queryid == id)
      {
        /* Get the name of the parameter (table field : 'param'). */
        data = heap_getattr(config_tuple, 2, config_rel->rd_att, &isnull);
        guc_name = pstrdup(TextDatumGetCString(data));

        /* Get the value for the parameter (table field : 'value'). */
        data = heap_getattr(config_tuple, 3, config_rel->rd_att, &isnull);
        /* FIXME should we add a test on isnull? */
        guc_value = pstrdup(TextDatumGetCString(data));

        param = malloc(sizeof(parameter));
        param->name = guc_name;

        slist_push_head(&paramResetList, &param->node);

        /*
         * Here we use the PostgreSQL try/catch mecanism so that when
         * SetConfigOption() returns an error, the current transaction
         * is rollbacked and its error message is logged. Such an
         * error message could be like:
         * 'ERROR:  unrecognized configuration parameter "Dalibo"'
         * or like:
         * 'ERROR:  invalid value for parameter "work_mem": "512KB"'.
         */
        PG_TRY();
        {
          SetConfigOption(guc_name, guc_value, PGC_USERSET, PGC_S_SESSION);
        }
        PG_CATCH();
        {
          rethrow = true;

          /* Current transaction will be rollbacked when exception is
           * re-thrown, so there's no need to reset the parameters that
           * may have successfully been set. Let's just destroy the list.
           */
          DestroyPRList(false);
          goto close;
        }
        PG_END_TRY();
      }
    }

close:
    table_endscan(config_scan);
    table_close(config_rel, AccessShareLock);
    if (rethrow)
    {
      PG_RE_THROW();
    }
  }

  /*
   * Call next hook if it exists
   */
  if (prevHook)
    result = prevHook(parse, query_st, cursorOptions, boundp);
  else
    result = standard_planner(parse, query_st, cursorOptions, boundp);

  return result;
}

/*
 * executor hook function: this is where we reset all our GUC for a specific
 * query
 */
static void
PlanTuner_ExecutorEnd(QueryDesc *q)
{
  DestroyPRList(true);

  if (prev_ExecutorEnd)
    prev_ExecutorEnd(q);
  else
    standard_ExecutorEnd(q);
}

/*
 * Initialize our library to set hooks and define our GUCs
 */
void
_PG_init(void)
{
  /* Create a GUC variable named pg_query_settings.enabled
   * used to enable or disable this module. */
  DefineCustomBoolVariable(
      "pg_query_settings.enabled",
      "Disable pg_query_settings module",
      "Disable pg_query_settings module",
      &enabled,
      true,
      PGC_USERSET,
      GUC_EXPLAIN,
      NULL,
      NULL,
      NULL
      );

  /* Create a GUC variable named pg_query_settings.debug
   * used to print debugging messages. */
  DefineCustomBoolVariable(
      "pg_query_settings.debug",
      "Print debugging messages",
      "Print debugging messages",
      &debug,
      false,
      PGC_USERSET,
      0,
      NULL,
      NULL,
      NULL
      );

  /* Create a GUC variable named pg_query_settings.print_queryid
   * used to print query identifier with NOTICE level. */
  DefineCustomBoolVariable(
      "pg_query_settings.print_queryid",
      "Print query identifier",
      "Print query identifier",
      &printQueryId,
      false,
      PGC_USERSET,
      0,
      NULL,
      NULL,
      NULL
      );

  /* Set our two hooks */
  if (planner_hook != execPlantuner)
  {
    prevHook = planner_hook;
    planner_hook = execPlantuner;
  }

  if (ExecutorEnd_hook != PlanTuner_ExecutorEnd)
  {
    prev_ExecutorEnd = ExecutorEnd_hook;
    ExecutorEnd_hook = PlanTuner_ExecutorEnd;
  }
}

/*
 * Reset hooks
 */
void
_PG_fini(void)
{
  planner_hook = prevHook;
  ExecutorEnd_hook = prev_ExecutorEnd;
}
