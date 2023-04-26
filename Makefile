MODULE_big = pg_query_settings
OBJS = pg_query_settings.o
EXTENSION = pg_query_settings
DATA = pg_query_settings--0.1.sql

PG_CONFIG = pg_config

PGVER := $(shell $(PG_CONFIG) --version | cut -d. -f1 | tr -dc '[[:digit:]]')

# If version not supported exit with an error message.
ifneq ($(PGVER),$(filter $(PGVER),12 13 14 15))
  $(error PostgreSQL $(PGVER) is not supported by this extension (v12..v15))
else
  # Add an object file to compute a QueryID for PostgreSQL versions prior to 14.
  ifneq ($(PGVER),$(filter $(PGVER),14 15))
    $(info Add QueryID computation for PostgreSQL $(PGVER))
    OBJS += pgsp_queryid.o
  endif
endif

PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
