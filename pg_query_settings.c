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
#include "access/genam.h"
#include <catalog/namespace.h>
#include "catalog/index.h"
#include <miscadmin.h>
#include <executor/executor.h>
#include <optimizer/planner.h>
#include <storage/bufmgr.h>
#include "storage/ipc.h"
#include <storage/predicate.h>
#include <utils/builtins.h>
#include <utils/guc.h>
#include <lib/ilist.h>
#include <access/genam.h>
#include <utils/fmgroids.h>

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
static slist_head paramResetList = SLIST_STATIC_INIT(paramResetList);

#if COMPUTE_LOCAL_QUERYID
static char * pgqs_queryString = NULL;
#endif

/* Constants */
/* Name of our config table */
static const char* pgqs_config = "pgqs_config";

#define PGQS_MAXPARAMNAMELENGTH   39
#define PGQS_MAXPARAMVALUELENGTH  10

/* Parameter struct */
typedef struct parameter
{
  char *name;
  slist_node node;
} parameter;

/* Our hooks on PostgreSQL */
static planner_hook_type prevHook  = NULL;
static ExecutorEnd_hook_type prev_ExecutorEnd = NULL;

/*
hooks needed to initialize and modify the hashtable in shmem
*/
static shmem_request_hook_type prev_shmem_request_hook = NULL;
static shmem_startup_hook_type prev_shmem_startup_hook = NULL;



PG_FUNCTION_INFO_V1(pg_query_settings_reload);

static void pgqs_shmem_request_hook(void);
static void pgqs_shmem_startup_hook(void);
static Size pgqs_memsize(void);

#if COMPUTE_LOCAL_QUERYID
static post_parse_analyze_hook_type prev_post_parse_analyze_hook = NULL;
static void pgqs_post_parse_analyze(ParseState *pstate, Query *query);
#endif


/*
Define our HashKey structure
*/
typedef struct pgqsHashKey
{
    uint64 queryid;
} pgqsHashKey;

/*
Define our settings
*/

typedef struct pgqsSettings
{
  /* FIXME*/
  char name[PGQS_MAXPARAMNAMELENGTH];
  char value[PGQS_MAXPARAMVALUELENGTH];
} pgqsSettings;

/*
Define our entries structure
*/
typedef struct pgqsEntry
{
  pgqsHashKey key; /* hash key of the entry, must be first */
  pgqsSettings settings; /* the settings for this hashkey */
  slock_t mutex;  /* protects from modification while reading */
} pgqsEntry;

/*
Shared State
*/
typedef struct pgqsSharedState
{
  LWLock *lock; /* protects hashtable search/modificartion */
  /* do we need more ? */
} pgqsSharedState;

/* links to shared memory state */
static pgqsSharedState *pgqs = NULL;
static HTAB *pgqs_hash = NULL;

// -----------------------------------------------------------------
/* Functions */

void pgqs_shmem_request_hook(void){
  if (debug) elog(DEBUG1,"Entering shmem_request_hook");

  if (prev_shmem_request_hook)
		prev_shmem_request_hook();

	RequestAddinShmemSpace(pgqs_memsize());
	RequestNamedLWLockTranche("pg_query_settings", 1);

};

void pgqs_shmem_startup_hook(void){
  if (debug) elog(DEBUG1,"Entering shmem_startup_hook");


};


/*
 * Estimate shared memory space needed.
 */
static Size
pgqs_memsize(void)
{
	Size		size;

	size = MAXALIGN(sizeof(pgqsSharedState));

/* FIXME : 1000 == max number  of queryid stored */
	size = add_size(size, hash_estimate_size(1000, sizeof(pgqsEntry)));


	return size;
}







#if COMPUTE_LOCAL_QUERYID

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
#if PG_VERSION_NUM < 130000
execPlantuner(Query *parse, int cursorOptions, ParamListInfo boundp)
#else
execPlantuner(Query *parse, const char *query_st, int cursorOptions, ParamListInfo boundp)
#endif
{
  PlannedStmt           *result;
  bool                  rethrow = false;
  uint64                queryid = 0;

// Index
  List                  * pgqs_index_list = NULL;
  Oid                   pgqs_first_indexOid = 0;
  ListCell              * pgqs_first_index = NULL;

//table
  Relation              config_rel;
  Oid                   config_relid = 0;


  Datum                 * elem_values = NULL;
  Datum                 * elem_gucname = NULL;
  Datum                 * elem_gucvalue = NULL;
  int                   num_results = 0;
  int                   _indice = 0;
  bool                  * elem_nulls = NULL;
  HeapTuple            config_tuple;

// ---------------
  Snapshot              _snapshot = NULL; // last snapshot

  SysScanDesc           _scandesc;
  ScanKeyData           _entry[1];
// ---------------

  if (debug) elog(DEBUG1, "entering execPlanTuner");


  if (enabled)
  {
    // getting the oid of our relation
    config_relid = RelnameGetRelid(pgqs_config);

    if (OidIsValid(config_relid))
    {

      if (debug) elog(DEBUG1, "opening table relation : %i", config_relid);
      // opening the relation
      config_rel = table_open(config_relid, AccessShareLock);

      if (debug && config_rel) elog(DEBUG1, "relation opened: %i", config_relid);


      // set query_st regarding the pg version
#if PG_VERSION_NUM < 130000
      char * query_st;

      //refactoring needed here
      query_st = pgqs_queryString;

      if (debug) elog(DEBUG1,"query_st=%s", query_st);
      if (debug) elog(DEBUG1,"pgqs_queryString=%s", pgqs_queryString);
#endif
      // Compute or not the queryid
#if COMPUTE_LOCAL_QUERYID
      queryid = hash_query(query_st);
#else
      queryid = parse->queryId;
#endif

      if (printQueryId) elog(NOTICE, "QueryID is '%li'", queryid);
      if (debug) elog(DEBUG1, "query's QueryID is '%li'", queryid);


      if (debug) elog(DEBUG1, "RelationGetIndexList");

      // Get the indexes list of our relation
      pgqs_index_list = RelationGetIndexList(config_rel);

      if (debug && pgqs_index_list) elog(DEBUG1, "pgqs_index_list ok");

      if (debug) elog(DEBUG1, "Getting the first index from list head");

      // Get the first and /should be/ last index of our relation
      pgqs_first_index = list_head(pgqs_index_list);
      pgqs_first_indexOid = lfirst_oid(pgqs_first_index);

      if (debug && pgqs_first_indexOid) elog(DEBUG1, "Got this index OID : %i",pgqs_first_indexOid);


      if (debug ) elog(DEBUG1, "freeing pgqs_index_list");

      // we dont need this list anymore
      pfree(pgqs_index_list);

      if (debug) elog(DEBUG1, "Initialising the scan");

      // ScanKeyInit(&_entry[0], 1, BTEqualStrategyNumber, F_OIDEQ, Int64GetDatum(queryid));
      // ScanKeyInit(&_entry[0], 1, BTEqualStrategyNumber, F_INT4EQ, Int64GetDatum(queryid));
      ScanKeyInit(&_entry[0], 1, BTEqualStrategyNumber, F_INT8EQ, Int64GetDatum(queryid));

      if (debug) elog(DEBUG1, "Starting the index scan");
      _scandesc = systable_beginscan(  config_rel,
                                       pgqs_first_indexOid,
                                       true,
                                       _snapshot,
                                      1,
                                      _entry);

      if (debug && _scandesc != NULL) elog(DEBUG1, "Index scan started");

      elem_values = palloc(sizeof(Datum) * 64);
      elem_gucname = palloc(sizeof(Datum) * 64);
      elem_gucvalue = palloc(sizeof(Datum) * 64);
      elem_nulls = palloc(sizeof(bool) * 64);

      if (debug) elog(DEBUG1, "Arrays allocated");

      if (debug) elog(DEBUG1, "Getting the first tuple");

      while ((config_tuple = systable_getnext(_scandesc)) != NULL)
      {
        if (debug) elog(DEBUG1, "--------------------");
        if (debug) elog(DEBUG1, "Tuple #%i", num_results);

        if (debug) elog(DEBUG1, "Getting field 1");
        elem_values[num_results] =
          heap_getattr(config_tuple, 1, config_rel->rd_att, &elem_nulls[num_results]);
        if (debug) elog(DEBUG1, "queryid=%li",elem_values[num_results]);

        if (debug) elog(DEBUG1, "getting guc name");
        elem_gucname[num_results] =
          heap_getattr(config_tuple, 2, config_rel->rd_att, &elem_nulls[num_results]);
        if (debug) elog(DEBUG1, "got guc name:%s",
          pstrdup(TextDatumGetCString(elem_gucname[num_results])));

        if (debug) elog(DEBUG1, "getting guc value");
        elem_gucvalue[num_results] =
          heap_getattr(config_tuple, 3, config_rel->rd_att, &elem_nulls[num_results]);
        if (debug) elog(DEBUG1, "got guc value:%s",
          pstrdup(TextDatumGetCString(elem_gucvalue[num_results])));


        num_results++;
      } // while we have tuples
      if (debug) elog(DEBUG1, "--------------------");
      if (debug) elog(DEBUG1, "End of the index scan");
      if (debug) elog(DEBUG1, "numresults=%i",num_results);


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
            // parcours des tableaux
            for (_indice = 0; _indice < num_results; _indice++){
              elog(DEBUG1, "Setting %s = %s",
                pstrdup(TextDatumGetCString(elem_gucname[_indice]) ),
                pstrdup(TextDatumGetCString(elem_gucvalue[_indice]) )
              );
              SetConfigOption(
                pstrdup(TextDatumGetCString(elem_gucname[_indice])),
                pstrdup(TextDatumGetCString(elem_gucvalue[_indice])),
                PGC_USERSET,
                PGC_S_SESSION
              );
            }
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

close:
    if (debug) elog(DEBUG1, "Endscan");
    systable_endscan(_scandesc);

    if (debug) elog(DEBUG1, "Closing table pgqs_config");
    table_close(config_rel, AccessShareLock);

    if (debug) elog(DEBUG1, "freeing arrays");
    pfree(elem_values);
    if (debug) elog(DEBUG1, "freeing elem_nulls");
    pfree(elem_nulls);
    if (debug) elog(DEBUG1, "freeing elem_gucname");
    pfree(elem_gucname);
    if (debug) elog(DEBUG1, "freeing elem_gucvalue");
    pfree(elem_gucvalue);

    if (rethrow)
    {
      PG_RE_THROW();
    }
  }
    else {
      // Cant open pgqs_config
      elog(ERROR, "Can't open %s", pgqs_config);
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


/* ************************************************ */

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

  if (debug) elog(DEBUG1,"Entering _PG_init()");

/*

*/
  prev_shmem_request_hook   = shmem_request_hook;
  shmem_request_hook        = pgqs_shmem_request_hook;
  prev_shmem_startup_hook   = shmem_startup_hook;
  shmem_startup_hook        = pgqs_shmem_startup_hook;

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

#if COMPUTE_LOCAL_QUERYID
  prev_post_parse_analyze_hook = post_parse_analyze_hook;
  post_parse_analyze_hook = pgqs_post_parse_analyze;
#endif

  if (debug) elog(DEBUG1,"Exiting _PG_init()");
}

/*
 * Reload hastable from table to shmem
 */
Datum
pg_query_settings_reload(PG_FUNCTION_ARGS)
{
  if (debug) elog (DEBUG1,"Reload");
	PG_RETURN_VOID();
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

  #if COMPUTE_LOCAL_QUERYID
    if (debug) elog(DEBUG1,"Recovering post_parse_analyze_hook");
    post_parse_analyze_hook = prev_post_parse_analyze_hook;
  #endif

  if (debug) elog(DEBUG1,"Exiting _PG_fini()");

}
