# Host-only regression tests for the HAWK m4 SHAKE/SHA3 glue.
#
# pqm4's on-target build (`make PLATFORM=nucleo-l4r5zi`) does not pick these
# up — pqm4's scheme discovery walks `find crypto_sign -mindepth 2 -maxdepth 2
# -type d`, so this `tests/` subdir is invisible to it. Run on the host:
#
#     make -C crypto_sign/hawk512/m4/tests run
#
# from the repo root. Each test cross-checks our wrapper against pqm4's
# reference fips202 implementation; both sit on top of the same
# KeccakF1600_State* primitives, so any byte mismatch is a wrapper bug.

CC      ?= gcc
CFLAGS  ?= -O2 -Wall -Wextra
TOP     := ../../../..
INCLUDES = -I.. -I$(TOP)/mupq/common
SOURCES = test_sha3_glue.c \
          ../sha3.c \
          $(TOP)/mupq/common/keccakf1600.c \
          $(TOP)/mupq/common/fips202.c

PLANT_SOURCES = test_plant_ntt.c ../plant_18433.c

.PHONY: all build run run-calibrate run-plant run-twiddle clean
all: build
build: test_sha3_glue test_plantard_calibrate test_plant_ntt test_plant_twiddle
test_sha3_glue: $(SOURCES)
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ $(SOURCES)
test_plantard_calibrate: test_plantard_calibrate.c
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ test_plantard_calibrate.c
test_plant_ntt: $(PLANT_SOURCES)
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ $(PLANT_SOURCES)
test_plant_twiddle: test_plant_twiddle.c
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ test_plant_twiddle.c
run: test_sha3_glue
	./test_sha3_glue
run-calibrate: test_plantard_calibrate
	./test_plantard_calibrate
run-plant: test_plant_ntt
	./test_plant_ntt
run-twiddle: test_plant_twiddle
	./test_plant_twiddle
clean:
	rm -f test_sha3_glue test_plantard_calibrate test_plant_ntt test_plant_twiddle
