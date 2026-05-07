# pg_rat/Makefile

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

# Auto-detect if we are being built as a contrib module or standalone.
# If ../../src/Makefile.global exists, assume contrib. Otherwise, use PGXS.
ifeq ($(wildcard ../../src/Makefile.global),)
USE_PGXS = 1
endif

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
# Link against libpq in standalone mode
SHLIB_LINK += -lpq
else
# Contrib build logic
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
