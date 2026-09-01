# flint: no external dependencies. libc and libm are the whole list.
#
# one target for the thing you actually want, plus a debug build. everything
# else came later.

CC ?= cc
STD = -std=c11
WARN = -Wall -Wextra -Wpedantic -Werror
INCLUDES = -Isrc/core -Isrc/frontend -Isrc/runtime -Isrc/util

SRC = $(wildcard src/*.c src/core/*.c src/frontend/*.c src/runtime/*.c src/util/*.c)

release: ; $(CC) $(STD) $(WARN) $(INCLUDES) -O2 -DNDEBUG $(SRC) -o flint -lm

debug: ; $(CC) $(STD) $(WARN) $(INCLUDES) -O0 -g3 $(SRC) -o flint-debug -lm

test: release ; sh tests/run_tests.sh ./flint

clean: ; rm -f flint flint-debug
