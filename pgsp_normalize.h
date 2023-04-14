/*-------------------------------------------------------------------------------------------------
 *
 * pgsp_normalize.h: Normalize a query.
 *
 * This is a partial copy of the pg_store_plans/pgsp_json.h file.
 *
 * Copyright (c) 2012-2022, NIPPON TELEGRAPH AND TELEPHONE CORPORATION
 *
 *-------------------------------------------------------------------------------------------------
 */

extern void normalize_expr(char *expr, bool preserve_space);
