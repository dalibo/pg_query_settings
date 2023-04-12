README
======

`pg_query_settings` is a module that dynamically set queries parameters based
on their queryid.

The original idea was to configure a specific value of the `work_mem` parameter
for a specific query. Currently, any query parameters also have customizable
values.

Requirements
------------

To compile these tools, you will need the libpq library (.so), the libpgcommon
and libpgfeutils libraries (.a), the PostgreSQL 14+ header files, and the
`pg_config` tool. The header files and the tool are usually available in a -dev
package.

To use them once compiled, you only need the libpq library. Any version should
be fine.

Compilation
-----------

You only have to do:

```
$ make
$ make install
```

Usage
-----

The extension must be created first:

```sql
CREATE EXTENSION pg_query_settings;
```

The library must be loaded either with `LOAD`:

```sql
LOAD 'pg_query_settings';
```

or with the usual parameters (`shared_preload_libraries` for example).
The library will read the `pgqs_config` table searching for the queryid
of the query.
For each `pgqs_config` line with this queryid, the second column indicates the
parameter name and the third column indicates the parameter value.
The `pgqs_config` table is populated with standard queries (`INSERT`, `UPDATE`,
`DELETE`).
The execution of the library function is automatic once it is loaded.

The extension can be disabled with the `pg_query_settings.enable` parameter.

More informations on pg_query_settings
--------------------------------------

Extension creation :

```
🐘 on postgres@r14 =# CREATE EXTENSION pg_query_settings;
CREATE EXTENSION
```

Creation and population of a user table :

```
🐘 on postgres@r14 =# CREATE TABLE toto (c1 integer, c2 text);
CREATE TABLE
🐘 on postgres@r14 =# INSERT INTO toto SELECT i, 'Ligne '||i FROM generate_series(1, 10000000) i;
INSERT 0 10000000
```

We run a query that generates a sort :

```
🐘 on postgres@r14 =# EXPLAIN (COSTS OFF, ANALYZE, SETTINGS, VERBOSE) SELECT * FROM toto ORDER BY c2;
┌────────────────────────────────────────────────────────────────────────────────────┐
│                                     QUERY PLAN                                     │
├────────────────────────────────────────────────────────────────────────────────────┤
│ Sort (actual time=28686.665..41124.312 rows=10000000 loops=1)                      │
│   Output: c1, c2                                                                   │
│   Sort Key: toto.c2                                                                │
│   Sort Method: external merge  Disk: 272920kB                                      │
│   ->  Seq Scan on public.toto (actual time=0.029..12849.197 rows=10000000 loops=1) │
│         Output: c1, c2                                                             │
│ Settings: random_page_cost = '1.1'                                                 │
│ Query Identifier: 2507635424379213761                                              │
│ Planning Time: 0.125 ms                                                            │
│ Execution Time: 52424.680 ms                                                       │
└────────────────────────────────────────────────────────────────────────────────────┘
(10 rows)
```

This query sorts on disk because there are not enough `work_mem`.

We increase `work_mem` for this session :

```
🐘 on postgres@r14 =# SET work_mem TO '1GB';
SET
🐘 on postgres@r14 =# EXPLAIN (COSTS OFF, ANALYZE, SETTINGS, VERBOSE) SELECT * FROM toto ORDER BY c2;
┌────────────────────────────────────────────────────────────────────────────────────┐
│                                     QUERY PLAN                                     │
├────────────────────────────────────────────────────────────────────────────────────┤
│ Sort (actual time=31700.626..43454.941 rows=10000000 loops=1)                      │
│   Output: c1, c2                                                                   │
│   Sort Key: toto.c2                                                                │
│   Sort Method: quicksort  Memory: 1020995kB                                        │
│   ->  Seq Scan on public.toto (actual time=0.026..13009.674 rows=10000000 loops=1) │
│         Output: c1, c2                                                             │
│ Settings: random_page_cost = '1.1', work_mem = '1GB'                               │
│ Query Identifier: 2507635424379213761                                              │
│ Planning Time: 0.376 ms                                                            │
│ Execution Time: 55004.212 ms                                                       │
└────────────────────────────────────────────────────────────────────────────────────┘
(10 rows)
```

And we can see that it uses this memory to sort in memory
even if the duration is roughly the same.

We go back to the default configuration (4 MB) :

```
🐘 on postgres@r14 =# RESET work_mem;
RESET
```

We insert the configuration to apply in the `pgqs_config` table :

```
🐘 on postgres@r14 =# INSERT INTO pgqs_config VALUES (2507635424379213761, 'work_mem', '1000000000');
INSERT 0 1
```

We execute the query :

```
🐘 on postgres@r14 =# EXPLAIN (COSTS OFF, ANALYZE, SETTINGS, VERBOSE) SELECT * FROM toto ORDER BY c2;
┌────────────────────────────────────────────────────────────────────────────────────┐
│                                     QUERY PLAN                                     │
├────────────────────────────────────────────────────────────────────────────────────┤
│ Sort (actual time=29046.787..41640.526 rows=10000000 loops=1)                      │
│   Output: c1, c2                                                                   │
│   Sort Key: toto.c2                                                                │
│   Sort Method: external merge  Disk: 272920kB                                      │
│   ->  Seq Scan on public.toto (actual time=0.035..12971.535 rows=10000000 loops=1) │
│         Output: c1, c2                                                             │
│ Settings: random_page_cost = '1.1'                                                 │
│ Query Identifier: 2507635424379213761                                              │
│ Planning Time: 0.162 ms                                                            │
│ Execution Time: 53110.755 ms                                                       │
└────────────────────────────────────────────────────────────────────────────────────┘
(10 rows)
```

And we don't see our applied configuration...
That's normal, we haven't loaded the library.

We load the library :

```
🐘 on postgres@r14 =# LOAD 'pg_query_settings';
LOAD
```

We rexecute the query again :

```
🐘 on postgres@r14 =# EXPLAIN (COSTS OFF, ANALYZE, SETTINGS, VERBOSE) SELECT * FROM toto ORDER BY c2;
WARNING:  queryid is '2507635424379213761'
WARNING:  value is 1000000000
┌────────────────────────────────────────────────────────────────────────────────────┐
│                                     QUERY PLAN                                     │
├────────────────────────────────────────────────────────────────────────────────────┤
│ Sort (actual time=31387.960..43111.072 rows=10000000 loops=1)                      │
│   Output: c1, c2                                                                   │
│   Sort Key: toto.c2                                                                │
│   Sort Method: quicksort  Memory: 1171342kB                                        │
│   ->  Seq Scan on public.toto (actual time=0.025..12888.501 rows=10000000 loops=1) │
│         Output: c1, c2                                                             │
│ Settings: random_page_cost = '1.1', work_mem = '1000000000kB'                      │
│ Query Identifier: 2507635424379213761                                              │
│ Planning Time: 0.392 ms                                                            │
│ Execution Time: 54615.882 ms                                                       │
└────────────────────────────────────────────────────────────────────────────────────┘
(10 rows)
```

And this time, the specific configuration is applied \o/ .
