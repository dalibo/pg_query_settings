MODULE_big = pg_query_settings
OBJS = pg_query_settings.o
EXTENSION = pg_query_settings
DATA = pg_query_settings--0.1.sql

PG_CONFIG = pg_config

# Add an object file to compute a QueryID for PostgreSQL versions prior to 14.
PGVER := $(shell $(PG_CONFIG) --version | cut -d. -f1 | tr -dc '[[:digit:]]')
ifeq ($(PGVER), 12)
  OBJS += pgsp_queryid.o
endif
ifeq ($(PGVER), 13)
  OBJS += pgsp_queryid.o
endif

PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
