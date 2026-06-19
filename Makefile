CC ?= cc
VERSION := 0.45.0-dev
CFLAGS ?= -std=c11 -Wall -Wextra -Werror -pedantic -g -D_POSIX_C_SOURCE=200809L -Iinclude -DIDM_VERSION=\"$(VERSION)\"
DEPFLAGS ?= -MMD -MP
LDFLAGS ?= -lpthread -lm
RELEASE_LTO ?= -flto=auto
RELEASE_CFLAGS ?= -std=c11 -O2 $(RELEASE_LTO) -DNDEBUG -DIDM_VERSION=\"$(VERSION)\" -D_POSIX_C_SOURCE=200809L -Iinclude

LIB_SRCS := \
 src/common/common.c \
 src/value/value.c \
 src/regex/regex.c \
 src/syntax/syntax.c \
 src/reader/reader.c \
  src/bytecode/bytecode.c \
  src/vm/vm.c \
  src/scope/scope.c \
  src/pattern/pattern.c \
  src/prims/prims.c \
  src/core/core.c \
  src/actor/actor.c \
  src/ports/ports.c \
  src/tty/tty.c \
 src/artifact/artifact.c \
 $(wildcard src/expand/*.c)

LIB_OBJS := $(LIB_SRCS:%.c=build/%.o)
CLI_OBJS := build/src/cli/main.o
TEST_SRCS := $(wildcard tests/unit/*.c)
TEST_OBJS := $(patsubst %.c,build/%.o,$(TEST_SRCS))
DEPS := $(LIB_OBJS:.o=.d) $(CLI_OBJS:.o=.d) $(TEST_OBJS:.o=.d)

.PHONY: all test sanitize tsan release clean snapshots perf perf-compare perf-profile perf-editor

SAN_FLAGS := -fsanitize=address,undefined -fno-omit-frame-pointer

all: build/idiomc build/ish build/nani

build/idiomc: $(LIB_OBJS) $(CLI_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

build/ish: build/idiomc $(wildcard app/ish/*.id)
	./build/idiomc build app/ish -o $@

build/nani: build/idiomc app/nani/pkg.id
	./build/idiomc build app/nani -o $@

build/unit_tests: $(LIB_OBJS) $(TEST_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

build/%.o: %.c
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(DEPFLAGS) -c -o $@ $<

build/pty_driver: tools/pty_driver.c
	mkdir -p build
	$(CC) $(CFLAGS) -o $@ tools/pty_driver.c

test: build/unit_tests build/idiomc build/ish build/pty_driver
	./build/unit_tests
	@sh tools/run_tests.sh ./build/idiomc ./build/ish output

build/unit_tests_san: $(LIB_SRCS) $(TEST_SRCS)
	mkdir -p build
	$(CC) $(CFLAGS) $(SAN_FLAGS) -o $@ $(LIB_SRCS) $(TEST_SRCS) $(LDFLAGS)

build/san/idiomc: $(LIB_SRCS) src/cli/main.c
	mkdir -p build/san
	$(CC) $(CFLAGS) $(SAN_FLAGS) -o $@ $(LIB_SRCS) src/cli/main.c $(LDFLAGS)

build/san/ish: build/san/idiomc $(wildcard app/ish/*.id)
	./build/san/idiomc build app/ish -o $@

sanitize: build/unit_tests_san build/san/idiomc build/san/ish build/pty_driver
	ASAN_OPTIONS=detect_leaks=1 ./build/unit_tests_san
	@ASAN_OPTIONS=detect_leaks=1 sh tools/run_tests.sh ./build/san/idiomc ./build/san/ish san

release:
	mkdir -p build/release
	$(CC) $(RELEASE_CFLAGS) -o build/release/idiomc $(LIB_SRCS) src/cli/main.c $(LDFLAGS)
	build/release/idiomc build app/ish -o build/release/ish
	build/release/idiomc build app/nani -o build/release/nani
	strip build/release/idiomc

PERF_RUNS ?= 5
PERF_WARMUPS ?= 1
PERF_BASE_REF ?= 9dbab72
PERF_ARGS ?=
PERF_ENV := IDIOMROOT=$(CURDIR)/std

perf: release
	@$(PERF_ENV) python3 tools/perf_suite.py --idiom-current ./build/release/idiomc --runs $(PERF_RUNS) --warmups $(PERF_WARMUPS) $(PERF_ARGS)

perf-compare: release
	@$(PERF_ENV) python3 tools/perf_suite.py --idiom-current ./build/release/idiomc --base-ref $(PERF_BASE_REF) --runs $(PERF_RUNS) --warmups $(PERF_WARMUPS) $(PERF_ARGS)

perf-profile: release
	@$(PERF_ENV) python3 tools/perf_suite.py --idiom-current ./build/release/idiomc --runs $(PERF_RUNS) --warmups $(PERF_WARMUPS) --with-sealed --dump-dir build/perf-dumps --callgrind-dir build/perf-callgrind --json-out build/perf-profile.json $(PERF_ARGS)

perf-editor: release
	@$(PERF_ENV) python3 tools/perf_suite.py --idiom-current ./build/release/idiomc --runs $(PERF_RUNS) --warmups $(PERF_WARMUPS) --cases editor_keys,editor_line,editor_buffer,editor_markers,editor_syntax,editor_render --runtimes idiom-current,elisp $(PERF_ARGS)

build/tsan/idiomc: $(LIB_SRCS) src/cli/main.c
	mkdir -p build/tsan
	$(CC) -std=c11 -g -O1 -fsanitize=thread -D_POSIX_C_SOURCE=200809L -DIDM_VERSION=\"$(VERSION)\" -Iinclude -o $@ $(LIB_SRCS) src/cli/main.c $(LDFLAGS)

tsan: build/tsan/idiomc
	./build/tsan/idiomc test tests/lang
	./build/tsan/idiomc test tests/app
	./build/tsan/idiomc repl < tests/repl/session.in >/dev/null

clean:
	rm -rf build
	find . -iname "*.ic" -delete

-include $(DEPS)

snapshots: build/unit_tests build/idiomc build/ish build/pty_driver
	@sh tools/run_tests.sh ./build/idiomc ./build/ish update
