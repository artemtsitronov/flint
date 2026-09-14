# The build.
#
# This used to be one compiler invocation over every source. Now there are
# object files, dependency tracking, and separate trees per configuration,
# which is a lot of ceremony for a 300ms build and the only reason `make`
# gets faster after the first edit.

CC ?= cc
STD = -std=c11
WARN = -Wall -Wextra -Wpedantic -Werror
INCLUDES = -Isrc/core -Isrc/frontend -Isrc/runtime -Isrc/util
CFLAGS ?=
ALL_CFLAGS := $(STD) $(WARN) $(INCLUDES) $(CFLAGS)

SRCDIRS := src src/core src/frontend src/runtime src/util
# sorted, because $(wildcard) hands back whatever order the filesystem feels
# like and two clones would then link into different binaries
SRCS := $(sort $(wildcard $(addsuffix /*.c,$(SRCDIRS))))
LIBS := -lm

# one tree per configuration. -O0 and -O2 objects are not interchangeable and
# make cannot know that, so they cannot share a directory.
BUILD := build
REL_DIR := $(BUILD)/release
DBG_DIR := $(BUILD)/debug
STR_DIR := $(BUILD)/stress

REL_OBJS := $(SRCS:%.c=$(REL_DIR)/%.o)
DBG_OBJS := $(SRCS:%.c=$(DBG_DIR)/%.o)
STR_OBJS := $(SRCS:%.c=$(STR_DIR)/%.o)

REL_DEPS := $(REL_OBJS:.o=.d)
DBG_DEPS := $(DBG_OBJS:.o=.d)
STR_DEPS := $(STR_OBJS:.o=.d)

# -MP is the part everyone forgets: without it, deleting a header leaves a
# .d file naming a file that no longer exists.
DEPFLAGS := -MMD -MP

REL_CFLAGS := -O2 -DNDEBUG
DBG_CFLAGS := -O0 -g3 -DFL_DEBUG_PRINT_CODE -DFL_DEBUG_TRACE_EXECUTION
STR_CFLAGS := -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -DFL_GC_STRESS
STR_LDFLAGS := -fsanitize=address,undefined

.PHONY: all release debug stress test unit bench clean help
.SUFFIXES:

# a recipe that failed halfway leaves a truncated object, and make then
# believes it is current
.DELETE_ON_ERROR:

all: release
release: flint

flint: $(REL_OBJS)
	$(CC) $(ALL_CFLAGS) $(REL_CFLAGS) $^ -o $@ $(LIBS)

flint-debug: $(DBG_OBJS)
	$(CC) $(ALL_CFLAGS) $(DBG_CFLAGS) $^ -o $@ $(LIBS)
debug: flint-debug

flint-stress: $(STR_OBJS)
	$(CC) $(ALL_CFLAGS) $(STR_CFLAGS) $(STR_LDFLAGS) $^ -o $@ $(LIBS)

$(REL_DIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(ALL_CFLAGS) $(REL_CFLAGS) $(DEPFLAGS) -c $< -o $@

$(DBG_DIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(ALL_CFLAGS) $(DBG_CFLAGS) $(DEPFLAGS) -c $< -o $@

$(STR_DIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(ALL_CFLAGS) $(STR_CFLAGS) $(DEPFLAGS) -c $< -o $@

-include $(REL_DEPS) $(DBG_DEPS) $(STR_DEPS)

test: flint
	@sh tests/run_tests.sh ./flint
stress: flint-stress
	@sh tests/run_tests.sh ./flint-stress
unit: ;

clean:
	rm -rf $(BUILD) flint flint-debug flint-stress

help:
	@echo "make            release build          -> ./flint"
	@echo "make debug      -O0, disassembly, trace -> ./flint-debug"
	@echo "make stress     gc stress + asan/ubsan, runs the suite"
	@echo "make test       release build, runs the language suite"
	@echo "make clean      remove build/ and the binaries"
