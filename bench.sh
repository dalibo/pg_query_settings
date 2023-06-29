#!/bin/sh

PGDATA="/home/frbn/.pgenv/pgsql/data"
DB=benchs
SCALE=200
CLIENTS=20
JOBS=$CLIENTS
DURATION=600   # 10 minutes
REPEAT=5

# echo "##### Preparing benchmark database..."
# dropdb --if-exists ${DB}
# createdb ${DB}
# pgbench --initialize --scale ${SCALE} ${DB}
psql -Xc "CREATE EXTENSION pg_query_settings;" ${DB}

echo "##### Benchmarking without pg_query_settings"
for i in $(seq 1 ${REPEAT})
do
  pgbench --select-only --jobs ${JOBS} --client ${CLIENTS} --time ${DURATION} ${DB}
done

echo "##### Adding pg_query_setting to shared_preload_libraries"
psql -Xc "ALTER SYSTEM SET shared_preload_libraries TO 'pg_query_settings';"
pg_ctl -D ${PGDATA} restart

for CONFIGS in 1 1000 1000000
do
  echo "##### Benchmarking with pg_query_settings (${CONFIGS} config)"
  psql -Xc "TRUNCATE pgqs_config;" ${DB}
  psql -Xc "INSERT INTO pgqs_config SELECT i, 'work_mem', '10MB' FROM generate_series(1,${CONFIGS}) i;" ${DB}
  for i in $(seq 1 ${REPEAT})
  do
    pgbench --select-only --jobs ${JOBS} --client ${CLIENTS} --time ${DURATION} ${DB}
  done
done

echo "##### Removing pg_query_setting from shared_preload_libraries"
>$PGDATA/postgresql.auto.conf
pg_ctl -D ${PGDATA} restart
