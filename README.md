README
======

`pg_query_settings` is a module that dynamically set queries' parameters based
on their queryid.

The original idea was to configure a specific value of the `work_mem` parameter
for a specific query. Currently, all query parameters may have a customizable
value.

Requirements
------------

To compile these tools, you will need the PostgreSQL 14+ header files, and the
`pg_config` tool. The header files and the tool are usually available in a -dev
package.

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

For each query executed, the library will read the `pgqs_config` table searching
for the queryid of the query. For each `pgqs_config` line with this queryid,
the second column indicates the parameter name and the third column indicates
the parameter value.

The `pgqs_config` table may be populated with standard DML queries (`INSERT`,
`UPDATE`, `DELETE`).

The execution of the library function's code is automatic once it is loaded.

The extension can be disabled with the `pg_query_settings.enabled` parameter.

More informations on pg_query_settings
--------------------------------------

Load the library :

```
postgres=# LOAD 'pg_query_settings';
LOAD
```

Create the extension:

```
postgres=# CREATE EXTENSION pg_query_settings;
CREATE EXTENSION
```

Create a user table and add some rows:

```
postgres=# CREATE TABLE toto (c1 integer, c2 text);
CREATE TABLE
postgres=# INSERT INTO toto SELECT i, 'Ligne '||i FROM generate_series(1, 10000000) i;
INSERT 0 10000000
```

We run a query that uses a sort :

```
postgres=# EXPLAIN (COSTS OFF, ANALYZE, SETTINGS, VERBOSE) SELECT * FROM toto ORDER BY c2;
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

This query sorts rows on disk because `work_mem` is not high enough.

We increase `work_mem` for this session:

```
postgres=# SET work_mem TO '1GB';
SET
postgres=# EXPLAIN (COSTS OFF, ANALYZE, SETTINGS, VERBOSE) SELECT * FROM toto ORDER BY c2;
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

Now, it uses this memory to sort in memory even if the duration is roughly the same.

We go back to the default configuration (4 MB):

```
postgres=# RESET work_mem;
RESET
```

We insert the configuration to apply in the `pgqs_config` table (we get the
query identifier from the `EXPLAIN output`):

```
postgres=# INSERT INTO pgqs_config VALUES (2507635424379213761, 'work_mem', '1000000000');
INSERT 0 1
```

We execute the query :

```
postgres=# EXPLAIN (COSTS OFF, ANALYZE, SETTINGS, VERBOSE) SELECT * FROM toto ORDER BY c2;
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

And this time, the specific configuration is applied.

Caveats
--------

`pg_query_settings` doesn't work well with **prepared queries**. These are
considered as not supported yet. This will be fixed in future releases.

Enabling this extension might have a performance impact on any workloads
with a high number of fast queries, as it slighlty increases the time taken
to compute the query plan. The more entries in the table, the greater the
impact. This will be improved in future releases.

It is possible to disable it globally by setting the parameter
`pg_query_settings.enabled` to `false` in the main configuration file,
and to enable it only for a specific database or user. For example:

```
ALTER ROLE olap_user SET pg_query_settings.enabled = true;
```
