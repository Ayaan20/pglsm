MODULE_big = lsm
OBJS = lsm.o
EXTENSION = lsm
DATA = lsm--1.0.sql
PGFILEDESC = "lsm - LSM tree index access method built from B-trees"

PG_CONFIG ?= pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
