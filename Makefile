MODULE_big = pg_query_settings
OBJS = pg_query_settings.o pgsp_normalize.o
#pgsp_json_text.o
EXTENSION = pg_query_settings
DATA = pg_query_settings--0.1.sql

PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)

##
## D O C K E R
##
DOCKER_IMAGE?=pgxn/pgxn-tools

docker_bash: #: enter the docker image (useful for testing)
	docker run -it --rm --volume "`pwd`:/source" --workdir /source --user "$(id -u):$(id -g)" $(DOCKER_IMAGE) bash
