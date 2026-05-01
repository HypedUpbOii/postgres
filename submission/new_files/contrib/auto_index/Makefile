MODULE_big = auto_index
OBJS = auto_index.o
EXTENSION = auto_index
DATA = auto_index--1.0.sql
PGFILEDESC = "auto_index - automatic index management"

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = contrib/auto_index
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_builddir)/contrib/contrib-global.mk
endif
