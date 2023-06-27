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
#if PG_VERSION_NUM < 130000
#include <catalog/pg_type.h>
#endif
#include <miscadmin.h>
#include <executor/executor.h>
#include <optimizer/planner.h>
#include <storage/bufmgr.h>
#include <utils/builtins.h>
#include <utils/guc.h>
#include <lib/ilist.h>
#include <executor/spi.h>

#include "pgsp_queryid.h"


/* This is a module :) */

PG_MODULE_MAGIC;

/* Macro definitions */

#if PG_VERSION_NUM < 140000
#define COMPUTE_LOCAL_QUERYID 1
#else
#define COMPUTE_LOCAL_QUERYID 0
#endif

#if COMPUTE_LOCAL_QUERYID
#include "parser/analyze.h"
#endif

/* Function definitions */

void _PG_init(void);
void _PG_fini(void);

/* Variables */

static bool    enabled = true;
static bool    debug = false;
static bool    printQueryId = false;
static uint64  queryid = 0;

#if PG_VERSION_NUM < 130000
static char *pgqs_queryString = NULL;
#endif

/* Constants */
/* Name of our config table */
static const char *pgqs_config ="pgqs_config";

/* Parameter struct */
typedef struct parameter
{
  char *name;
  char *oldValue;
} parameter;

/* ParameterList struct */
typedef struct parameterList
{
  int num;
  parameter * params;
} parameterList;

static parameterList paramResetList;

/* Our hooks on PostgreSQL */
static planner_hook_type prevHook  = NULL;
static ExecutorRun_hook_type prev_ExecutorRun = NULL;
static ExecutorEnd_hook_type prev_ExecutorEnd = NULL;

#if PG_VERSION_NUM < 130000
static post_parse_analyze_hook_type prev_post_parse_analyze_hook = NULL;
static void pgqs_post_parse_analyze(ParseState *pstate, Query *query);
#endif

static void pgqs_ExecutorRun(QueryDesc *queryDesc, ScanDirection direction,
uint64 count, bool execute_once);

// -----------------------------------------------------------------
/* Functions */

#if PG_VERSION_NUM < 130000

static void pgqs_post_parse_analyze(ParseState *pstate, Query *query)
{
  /* here we get the query string and put it in pgqs_queryString.
   */

  if (debug) elog (DEBUG1,"Entering pgqs_post_parse_analyze");

  if (prev_post_parse_analyze_hook)
    prev_post_parse_analyze_hook(pstate, query);

  if (debug) elog (DEBUG1,"setting pgqs_queryString to \"%s\"",pstate->p_sourcetext);

  pgqs_queryString = pstrdup(pstate->p_sourcetext);

  if (debug) elog (DEBUG1,"Exiting pgqs_post_parse_analyze");

}

#endif //COMPUTE_LOCAL_QUERYID = 1

/*
 * Destroy the table of parameters.
 * If 'reset' is true, then restore value of each parameter.
 */
static void DestroyPRList(bool reset)
{
  if (debug) elog(DEBUG1, "Destroy paramResetList");

  if (reset) {
    for (int i=0; i<paramResetList.num; i++) {
      if (debug) elog(DEBUG1, "Reset guc %s to %s", paramResetList.params[i].name, paramResetList.params[i].oldValue);
      SetConfigOption(paramResetList.params[i].name, paramResetList.params[i].oldValue, PGC_USERSET, PGC_S_SESSION);
    }
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
#if PG_VERSION_NUM < 130000
execPlantuner(Query *parse, int cursorOptions, ParamListInfo boundp)
#else
execPlantuner(Query *parse, const char *query_st, int cursorOptions, ParamListInfo boundp)
#endif
{
  PlannedStmt    *result;
  Oid            config_relid;
  bool           rethrow = false;
  char           *guc_value = NULL;
  char           *guc_name = NULL;
#if PG_VERSION_NUM < 130000
  char * query_st;
#endif
  int     ret = 0;
  int64   rows = 0;
  int     t = 0;
  const char *sql = "SELECT param, value FROM pgqs_config WHERE queryid = $1;";
  static SPIPlanPtr plan = NULL;

  Oid arg_types[1] = { INT8OID };
  Datum arg_values[1] = { queryid };

  if (enabled)
  {
    config_relid = RelnameGetRelid(pgqs_config);

   if (OidIsValid(config_relid))
   {
#if PG_VERSION_NUM < 130000
      query_st = pgqs_queryString;

      if (debug) elog(DEBUG1,"query_st=%s", query_st);
      if (debug) elog(DEBUG1,"pgqs_queryString=%s", pgqs_queryString);
#endif

      if (debug) elog(DEBUG1,"query_st=%s", query_st);

#if COMPUTE_LOCAL_QUERYID
      queryid = hash_query(query_st);
#else
      queryid = parse->queryId;
#endif

      if (printQueryId) elog(NOTICE, "QueryID is '%li'", queryid);

      if (debug) elog(DEBUG1, "query's QueryID is '%li'", queryid);

      arg_values[0] = UInt64GetDatum(queryid);

      elog(DEBUG1, "context parent: %s", CurrentMemoryContext->parent->name);

      SPI_connect();

      if (plan == NULL)
      {
       plan = SPI_prepare(sql, 1, arg_types);
       SPI_keepplan(plan);
      }
      else
      {
        elog(DEBUG1, "PLAN EN CACHE");
      }

      PG_TRY();
      {
        planner_hook = prevHook;

        ret = SPI_execute_plan(plan, arg_values, NULL, true, 0);
        elog(DEBUG1, "%s retourne RET %d", sql, ret);

        prevHook = planner_hook;
        planner_hook = execPlantuner;
      }
      PG_CATCH();
      {
        elog(DEBUG1, "Problème lors de l'exec de %s", sql);
      }
      PG_END_TRY();


      if (ret<=0) goto close;

      rows = SPI_processed;
      if (SPI_tuptable)
      {
        char    *buf;
        const char      *oldValue;
        TupleDesc       tupdesc = SPI_tuptable->tupdesc;
        SPITupleTable   *tuptable = SPI_tuptable;
        parameter       *param;
        MemoryContext oldcontext;

        /* Allocation mémoire pour le tableau */

        elog(DEBUG1,"Memory Context Name : %s",CurrentMemoryContext->name);

        paramResetList.params = SPI_palloc(rows * sizeof(parameter));
        paramResetList.num = 0;
        param = paramResetList.params;

        for (t=0; t < rows; t++)
        {
          HeapTuple   tuple = tuptable->vals[t];

          /* Get the name of the parameter (table field : 'param'). */
          guc_name = SPI_getvalue(tuple, tupdesc, 1);

          /* Get the value for the parameter (table field : 'value'). */
          guc_value = SPI_getvalue(tuple, tupdesc, 2);

          oldValue = GetConfigOption(guc_name, true, false);
          if (oldValue == NULL)
          {
            elog(WARNING, "parameter %s does not exists", guc_name);
            continue;
          }

          oldcontext = MemoryContextSwitchTo(PortalContext);

          buf = palloc(strlen(guc_name) + 1);
          strcpy(buf, guc_name);
          guc_name = buf;
          param->name = guc_name;

          /* Get and store current value for the parameter. */
    
          buf = palloc(strlen(oldValue) + 1);
          strcpy(buf, oldValue);
          param->oldValue = buf;

          paramResetList.num++;

          param++;

          MemoryContextSwitchTo(oldcontext);

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
            elog(DEBUG1, "Setting %s = %s", guc_name,guc_value);
            SetConfigOption(guc_name, guc_value, PGC_USERSET, PGC_S_SESSION);
          }
          PG_CATCH();
          {
            elog(DEBUG1, "ERROR SetConfigOption");
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
      SPI_finish();
      if (rethrow)
        PG_RE_THROW();
    }
  }

  /*
   * Call next hook if it exists
   */
  if (prevHook)
#if PG_VERSION_NUM < 130000
    result = prevHook(parse, cursorOptions, boundp);
#else
    result = prevHook(parse, query_st, cursorOptions, boundp);
#endif
  else
#if PG_VERSION_NUM < 130000
    result = standard_planner(parse, cursorOptions, boundp);
#else
    result = standard_planner(parse, query_st, cursorOptions, boundp);
#endif

  return result;
}

/*
 * ExecutorRun hook function
 *
 * TODO: describe the use case that required its implementation
 */
static void
pgqs_ExecutorRun(QueryDesc *queryDesc, ScanDirection direction, uint64 count, bool execute_once)
{

  PG_TRY();
  {
    if (prev_ExecutorRun)
      prev_ExecutorRun(queryDesc, direction, count, execute_once);
    else
      standard_ExecutorRun(queryDesc, direction, count, execute_once);
  }
  PG_CATCH();
  {
    DestroyPRList(false);
    PG_RE_THROW();
  }
  PG_END_TRY();
}

/*
 * ExecutorEnd hook function: this is where we reset all our GUC for a specific
 * query.
 */
static void
PlanTuner_ExecutorEnd(QueryDesc *q)
{
  //MemoryContextStats(TopMemoryContext);

  if( planner_hook == execPlantuner )
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

  if (debug) elog(DEBUG1,"Entering _PG_init()");

  /* Set our three hooks */
  if (planner_hook != execPlantuner)
  {
    prevHook = planner_hook;
    planner_hook = execPlantuner;
  }

  if (ExecutorRun_hook != pgqs_ExecutorRun)
  {
    prev_ExecutorRun = ExecutorRun_hook;
    ExecutorRun_hook = pgqs_ExecutorRun;
  }

  if (ExecutorEnd_hook != PlanTuner_ExecutorEnd)
  {
    prev_ExecutorEnd = ExecutorEnd_hook;
    ExecutorEnd_hook = PlanTuner_ExecutorEnd;
  }

#if PG_VERSION_NUM < 130000
  prev_post_parse_analyze_hook = post_parse_analyze_hook;
  post_parse_analyze_hook = pgqs_post_parse_analyze;
#endif

  if (debug) elog(DEBUG1,"Exiting _PG_init()");
}

/*
 * Reset hooks
 */
void
_PG_fini(void)
{

  if (debug) elog(DEBUG1,"Entering _PG_fini()");

  planner_hook = prevHook;
  ExecutorEnd_hook = prev_ExecutorEnd;

#if PG_VERSION_NUM < 130000
  if (debug) elog(DEBUG1,"Recovering post_parse_analyze_hook");
  post_parse_analyze_hook = prev_post_parse_analyze_hook;
#endif

  if (debug) elog(DEBUG1,"Exiting _PG_fini()");

}
