# Copyright (c) 2026, PostgreSQL Global Development Group

pg_rat_sources = files(
  'pg_rat.c',
  'pg_rat_capture.c',
  'pg_rat_shmem.c',
  'pg_rat_bgworker.c',
  'pg_rat_replayer.c',
  'pg_rat_spa.c',
  'pg_rat_report.c',
)

if host_system == 'windows'
  pg_rat_sources += rc_lib_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'pg_rat',
    '--FILEDESC', 'pg_rat - Real Application Testing for PostgreSQL',])
endif

pg_rat = shared_module('pg_rat',
  pg_rat_sources,
  kwargs: contrib_mod_args + {
    'dependencies': contrib_mod_args['dependencies'] + [libpq],
  },
)
contrib_targets += pg_rat

install_data(
  'pg_rat.control',
  'pg_rat--1.0.sql',
  kwargs: contrib_data_args,
)
