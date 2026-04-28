# contrib/pg_rat/Makefile

MODULE_big = pg_rat
OBJS = \
	pg_rat.o \
	pg_rat_capture.o \
	pg_rat_shmem.o \
	pg_rat_bgworker.o \
	pg_rat_replayer.o \
	pg_rat_spa.o \
	pg_rat_report.o \
	$(WIN32RES)

EXTENSION = pg_rat
DATA = pg_rat--1.0.sql
PGFILEDESC = "pg_rat - Real Application Testing for PostgreSQL"

# The module can be built either as a contrib module in a Postgres source tree
# or as a standalone extension using PGXS.
ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
# When building standalone, we must link directly to libpq
SHLIB_LINK += -lpq
else
# When building as contrib, we link to the internal libpq build
PG_CPPFLAGS = -I$(libpq_srcdir)
SHLIB_LINK_INTERNAL = $(libpq)
SHLIB_PREREQS = submake-libpq
subdir = contrib/pg_rat
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif

# Ensure we don't try to run regression tests without the library preloaded
NO_INSTALLCHECK = 1
