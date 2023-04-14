#pgsp_json_text.o
EXTENSION = pg_query_settings
EXTENSION_VERSION=$(shell grep default_version $(EXTENSION).control | sed -e "s/default_version[[:space:]]*=[[:space:]]*'\([^']*\)'/\1/")
DATA = $(EXTENSION)--$(EXTENSION_VERSION).sql

MODULE_big = pg_query_settings
OBJS = pg_query_settings.o pgsp_normalize.o

TESTS        = test/sql/base.sql
REGRESS      = base
REGRESS_OPTS = --inputdir=test

PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)

##
## D O C K E R
##
DOCKER_IMAGE?=pgxn/pgxn-tools

docker_bash: #: enter the docker image (useful for testing)
	docker run -it --rm --volume "`pwd`:/source" --workdir /source --user "$(id -u):$(id -g)" $(DOCKER_IMAGE) bash
