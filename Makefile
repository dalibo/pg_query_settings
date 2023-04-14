MODULE_big = pg_query_settings
OBJS = pg_query_settings.o pgsp_normalize.o
#pgsp_json_text.o
EXTENSION = pg_query_settings
DATA = pg_query_settings--0.1.sql

PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)

