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

#if COMPUTE_LOCAL_QUERYID
static post_parse_analyze_hook_type prev_post_parse_analyze_hook = NULL;
static void pgqs_post_parse_analyze(ParseState *pstate, Query *query);
#endif

// -----------------------------------------------------------------
/* Functions */

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
  PlannedStmt    *result;
  TableScanDesc  config_scan;
  Datum          data;
  bool           isnull;
  bool           rethrow = false;
  int64          id;
  char           *guc_value = NULL;
  char           *guc_name = NULL;
  parameter      *param = NULL;
  uint64          queryid = 0;

// Index scan
  IndexScanDesc         config_index_scan = NULL;
  bool                  index_scan_call_again = false;
  bool                  all_dead = false;
  TupleTableSlot        * slot = NULL;
  List                  * pgqs_index_list = NULL;
  Oid                   pgqs_first_indexOid = 0;
  ListCell              * pgqs_first_index = NULL;

//index
  Relation              indexRel;
  ItemPointer           index_tuple_tid;
  HeapTuple             index_tuple;
  TupleDesc             index_tupdesc;
  Datum                 indexScan_result;

//table
  Relation              config_rel;
  HeapTuple             config_tuple;
  Oid                   config_relid = 0;
// ItemPointer           tuple_tid;
  TupleDesc             tupdesc;
  BlockNumber           blkno;
  OffsetNumber          offnum;

  bool                  tuple_is_null = true;

  Datum                 * elem_values = NULL;
  int                   num_results = 0;
  bool                  * elem_nulls = NULL;
  HeapTupleData         tuple_data;
  HeapTuple             tuple = &tuple_data;

// ---------------
  Snapshot          _snapshot = SnapshotAny ;
  Buffer		        _buffer;
  Buffer		        *userbuf;
  ItemId		        _lp;
  Page		          _page;
  OffsetNumber      _offnum;
  bool              _valid;
  ItemPointer       _tid; //&(tuple->t_self);
  bool              valid;


// ---------------

/*

*/
  if (enabled)
  {
    config_relid = RelnameGetRelid(pgqs_config);

    if (debug) elog(DEBUG1, "opening relation : %i", config_relid);
    config_rel = table_open(config_relid, AccessShareLock);
    if (debug && config_rel) elog(DEBUG1, "relation opened: %i", config_relid);
    if (debug) elog(DEBUG1, "getting the tuple desc");
    tupdesc = RelationGetDescr(config_rel);

    if (OidIsValid(config_relid))
    {

      // set query_st
#if PG_VERSION_NUM < 130000
      char * query_st;
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

      // Get the indexes list
      if (debug) elog(DEBUG1, "RelationGetIndexList");
      pgqs_index_list = RelationGetIndexList(config_rel);
      if (debug && pgqs_index_list) elog(DEBUG1, "pgqs_index_list ok");

      // Get the first and /should be/ last index
      if (debug) elog(DEBUG1, "Getting the first index from list head");
      pgqs_first_index = list_head(pgqs_index_list);
      pgqs_first_indexOid = lfirst_oid(pgqs_first_index);
      if (debug && pgqs_first_indexOid) elog(DEBUG1, "Got this index OID : %i",pgqs_first_indexOid);

      /* Open the index */
      if (debug) elog(DEBUG1, "Opening index %i",pgqs_first_indexOid);
      indexRel = index_open(pgqs_first_indexOid, AccessShareLock);
      if (debug && indexRel) elog(DEBUG1, "Got the Relation of %i",pgqs_first_indexOid);

      //Start the index scan
      if (debug) elog(DEBUG1, "Starting the index scan");
      config_index_scan = index_beginscan(config_rel,indexRel,SnapshotAny, 0, 0);
      if (debug && config_index_scan != NULL) elog(DEBUG1, "Index scan started");

      // Allocate 64×Datum and 64×bool arrays
      elem_values = palloc(sizeof(Datum) * 64);
      elem_nulls = palloc(sizeof(bool) * 64);
      if (debug) elog(DEBUG1, "Arrays allocated");

      // Scan
      // Get the first tuple from the index scan
      if (debug) elog(DEBUG1, "Getting first index tuple");
      index_tuple_tid = index_getnext_tid(config_index_scan, ForwardScanDirection);

      while ( index_tuple_tid != NULL){

        blkno = ItemPointerGetBlockNumber(index_tuple_tid);
        offnum = ItemPointerGetOffsetNumber(index_tuple_tid);

        if (debug) elog(DEBUG1, "Got this index_tuple tid : %i/%i", blkno, offnum);
        if (debug) elog(DEBUG1, "Setting tupple->t_self with tid");
        ItemPointerCopy(index_tuple_tid, &(tuple->t_self) );

        if (debug) elog(DEBUG1, "Fetching the tuple");
        if (debug && config_rel) elog(DEBUG1, "Fetching the tuple: config_rel not NULL");

        // heap_fetch
        tuple = &tuple_data; //already initialised in declaration
        /*
	       * Fetch and pin the appropriate page of the relation.
	       */
        if (debug) elog(DEBUG1, "Fetch and pin the buffer");
        _buffer = ReadBuffer(config_rel, ItemPointerGetBlockNumber(index_tuple_tid));

        /*
    	   * Need share lock on buffer to examine tuple commit status.
    	  */
        if (debug) elog(DEBUG1, "Locking the buffer");
	      LockBuffer(_buffer, BUFFER_LOCK_SHARE);

        if (debug) elog(DEBUG1, "Getting the page");
        _page = BufferGetPage(_buffer);
        if (debug) elog(DEBUG1, "test for old snapshot");
        TestForOldSnapshot(_snapshot, config_rel, _page);

        if (debug) elog(DEBUG1, "Getting the offset");
        if (debug) elog(DEBUG1, " CRASH");

        _offnum = ItemPointerGetOffsetNumber(index_tuple_tid);
        // _offnum = (index_tuple_tid)->ip_posid;
        if (debug) elog(DEBUG1, " got the offnum");

        /*
        * We'd better check for out-of-range offnum in case of VACUUM since the
        * TID was obtained.
        */
        if (_offnum < FirstOffsetNumber || _offnum > PageGetMaxOffsetNumber(_page))
	      {
          if (debug) elog(DEBUG1, " locking buffer");

		      LockBuffer(_buffer, BUFFER_LOCK_UNLOCK);
		      ReleaseBuffer(_buffer);
		      *userbuf = InvalidBuffer;
		      tuple->t_data = NULL;
          if (debug && config_rel) elog(DEBUG1, "offnum out of page");
          goto close;
	      }
        if (debug && config_rel) elog(DEBUG1, "offnum in page");


        /*
         * get the item line pointer corresponding to the requested tid
         */
        _lp = PageGetItemId(_page, _offnum);

        /*
         * Must check for deleted tuple.
         */
        if (!ItemIdIsNormal(_lp))
    	  {
    		  LockBuffer(_buffer, BUFFER_LOCK_UNLOCK);
    		  ReleaseBuffer(_buffer);
    		  *userbuf = InvalidBuffer;
    		  tuple->t_data = NULL;
          goto close;
    	  }

        /*
      	 * fill in *tuple fields
      	 */
      	tuple->t_data = (HeapTupleHeader) PageGetItem(_page, _lp);
      	tuple->t_len = ItemIdGetLength(_lp);
      	tuple->t_tableOid = RelationGetRelid(config_rel);

        /*
      	 * check tuple visibility, then release lock
      	 */
      	valid = HeapTupleSatisfiesVisibility(tuple, _snapshot, _buffer);
        if (valid)
      		PredicateLockTID( config_rel, &(tuple->t_self), _snapshot,
      						          HeapTupleHeaderGetXmin(tuple->t_data));

      	HeapCheckForSerializableConflictOut(valid, config_rel, tuple, _buffer, _snapshot);

      	LockBuffer(_buffer, BUFFER_LOCK_UNLOCK);

        if (valid)
        {
          /*
           * All checks passed, so return the tuple as valid. Caller is now
           * responsible for releasing the buffer.
           */
          *userbuf = _buffer;
          if (debug) elog(DEBUG1, "Buffer OK !");
          if (debug) elog(DEBUG1, "Tuple data fetched");
          // what do we do with it ?
          // get the value of field 1 put it in elem_values[]
          elem_values[num_results] = heap_getattr(tuple, 1, tupdesc, &elem_nulls[num_results]);
        }


       // *************************
        // if (!heap_fetch(config_rel, SnapshotAny, tuple, NULL, false)) {
        //   if (debug) elog(DEBUG1, "NULL fetch !");
        //     tuple_data.t_data = NULL;
        // }else
        // {
        // }
        // index_tuple_tid = index_getnext_tid(config_index_scan, ForwardScanDirection);


      // elem_values[num_results] = fastgetattr(index_tuple, 1, index_tupdesc, &elem_nulls[num_results]);
        // num_results++;

      if (debug) elog(DEBUG1, "End");


      // if (debug) elog(DEBUG1, "Getting index_tupdesc from indexRel");
      // index_tupdesc = RelationGetDescr(indexRel);
      // if (debug) elog(DEBUG1, "Got index_tupdesc from indexRel");




    // }
    // else
    // {
    //   if (debug) elog(DEBUG1, "index_tuple_tid is NULL");
    //
    // }


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
            // SetConfigOption(guc_name, guc_value, PGC_USERSET, PGC_S_SESSION);
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
      // Clean up
      if (debug) elog(DEBUG1, "Ending the index scan");
      index_endscan(config_index_scan);
      if (debug) elog(DEBUG1, "Closing index");
      index_close(indexRel, AccessShareLock);

      // table_endscan(config_scan);
      if (debug) elog(DEBUG1, "Closing pgqs_config");
      table_close(config_rel, AccessShareLock);

// Index Scan End
      // if (debug) elog(DEBUG1, "table_index_fetch_end");
      // table_index_fetch_end(config_index_scan);

      if (rethrow)
      {
        PG_RE_THROW();
      }
    }
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
