/*-------------------------------------------------------------------------------------------------
 *
 * pgsp_queryid.h: Normalize a query and get compute a queryid.
 *
 * This is a partial copy of the pg_store_plans/pgsp_json.h file.
 *
 * Copyright (c) 2012-2022, NIPPON TELEGRAPH AND TELEPHONE CORPORATION
 *
 *-------------------------------------------------------------------------------------------------
 */

extern uint64 hash_query(const char* query);
