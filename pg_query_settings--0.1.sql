-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION pg_query_settings" to load this file. \quit

-- create our configuration table
CREATE TABLE IF NOT EXISTS pgqs_config (queryid BIGINT, param TEXT, value TEXT NOT NULL);
ALTER TABLE pgqs_config ADD CONSTRAINT pgqs_config_pkey PRIMARY KEY (queryid, param) INCLUDE (value);

-- mark it as dumpable
SELECT pg_catalog.pg_extension_config_dump('pgqs_config','');
