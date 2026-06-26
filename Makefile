.DEFAULT_GOAL := all

CC ?= cc
PYTHON ?= python3
VERSION := 0.47.0-dev

CPPFLAGS ?= -D_POSIX_C_SOURCE=200809L -DIDM_VERSION=\"$(VERSION)\" -Iinclude
CFLAGS ?= -std=c11 -Wall -Wextra -Werror -pedantic -g
DEPFLAGS ?= -MMD -MP
LDFLAGS ?= -lpthread -lm

RELEASE_LTO ?= -flto=auto
RELEASE_CFLAGS ?= -std=c11 -O2 $(RELEASE_LTO) -DNDEBUG
SAN_FLAGS ?= -fsanitize=address,undefined -fno-omit-frame-pointer
TSAN_FLAGS ?= -std=c11 -g -O1 -fsanitize=thread

IDIOMC := build/idiomc
ISH := build/ish
NANI := build/nani
UNIT_TESTS := build/unit_tests
PTY_DRIVER := build/pty_driver
RELEASE_IDIOMC := build/release/idiomc
RELEASE_ISH := build/release/ish
RELEASE_NANI := build/release/nani

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

CLI_SRCS := src/cli/main.c
TEST_SRCS := $(wildcard tests/unit/*.c)

LIB_OBJS := $(LIB_SRCS:%.c=build/%.o)
CLI_OBJS := $(CLI_SRCS:%.c=build/%.o)
TEST_OBJS := $(TEST_SRCS:%.c=build/%.o)
DEPS := $(LIB_OBJS:.o=.d) $(CLI_OBJS:.o=.d) $(TEST_OBJS:.o=.d)

VALGRIND ?= valgrind
MEMCHECK_FLAGS ?= --leak-check=full --show-leak-kinds=all --error-exitcode=99
MEMCHECK_LOGS := \
	build/valgrind-unit-tests.log \
	build/valgrind-startup.log \
	build/valgrind-protocols.log \
	build/valgrind-editor-buffer.log

PERF_RUNS ?= 5
PERF_WARMUPS ?= 1
PERF_BASE_REF ?= 9dbab72
PERF_CASES ?=
PERF_RUNTIMES ?=
PERF_MODES ?= source
PERF_CACHE ?= hot
PERF_ARGS ?=

RUNS ?= $(PERF_RUNS)
WARMUPS ?= $(PERF_WARMUPS)
BASE ?= $(PERF_BASE_REF)
CASES ?= $(PERF_CASES)
RUNTIMES ?= $(PERF_RUNTIMES)
MODES ?= $(PERF_MODES)
CACHE ?= $(PERF_CACHE)

PERF_ENV := IDIOMROOT=$(CURDIR)/std
PERF_CASE_ARGS = $(if $(strip $(CASES)),--cases $(CASES))
PERF_RUNTIME_ARGS = $(if $(strip $(RUNTIMES)),--runtimes $(RUNTIMES))
PERF_COMMON_ARGS = --idiom-current ./$(RELEASE_IDIOMC) --runs $(RUNS) --warmups $(WARMUPS) --modes $(MODES) --cache $(CACHE) $(PERF_CASE_ARGS) $(PERF_RUNTIME_ARGS)
PERF_PROFILE_CASES ?= startup,arith_idiomatic,list_sum_idiomatic,range_size,pattern_matrix,regex_scan,regex_capture,actor_spawn
PERF_EDITOR_CASES ?= editor_keys,editor_line,editor_buffer,editor_markers,editor_syntax,editor_render

.PHONY: all build help clean
.PHONY: unit harness test check snapshots
.PHONY: asan sanitize memcheck tsan
.PHONY: release
.PHONY: perf perf-quick perf-list perf-history perf-compare perf-build perf-cache perf-sealed perf-profile perf-editor

all: $(IDIOMC) $(ISH) $(NANI)

build: all

help:
	@printf '%s\n' \
		'Build:' \
		'  make              build idiomc, ish, and nani' \
		'  make release      build optimized binaries in build/release' \
		'  make clean        remove build products and cached .ic files' \
		'' \
		'Tests:' \
		'  make unit         run C unit tests only' \
		'  make harness      run language, shell, golden, REPL, and dump checks' \
		'  make test         run unit + harness' \
		'  make snapshots    update checked-in golden output after intentional changes' \
		'  make asan         run the address/undefined sanitizer lane' \
		'  make tsan         run the thread sanitizer smoke lane' \
		'  make memcheck     run selected tests under valgrind' \
		'' \
		'Performance:' \
		'  make perf         sealed runtime, plus source/hot tax, vs installed runtimes' \
		'  make perf-quick   same suite, shorter run' \
		'  make perf-list    list benchmark cases' \
		'  make perf-history compare current Idiom with BASE=<git-ref> using matching modes' \
		'  make perf-build   measure idiomc build time with hot and cold caches' \
		'  make perf-cache   compare hot, cold, and disabled source cache' \
		'  make perf-sealed  measure sealed artifact startup/runtime only' \
		'  make perf-profile profile source/hot current Idiom runs' \
		'  make perf-editor  editor benchmarks against elisp' \
		'' \
		'Perf knobs: CASES=a,b RUNTIMES=idiom,python MODES=source,build CACHE=hot,cold,off RUNS=5 WARMUPS=1 BASE=9dbab72'

$(IDIOMC): $(LIB_OBJS) $(CLI_OBJS)
	$(CC) -o $@ $^ $(LDFLAGS)

$(ISH): $(IDIOMC) $(wildcard app/ish/*.id)
	./$(IDIOMC) build app/ish -o $@

$(NANI): $(IDIOMC) app/nani/pkg.id
	./$(IDIOMC) build app/nani -o $@

$(UNIT_TESTS): $(LIB_OBJS) $(TEST_OBJS)
	$(CC) -o $@ $^ $(LDFLAGS)

$(PTY_DRIVER): tools/pty_driver.c
	mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $<

build/%.o: %.c
	mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(DEPFLAGS) -c -o $@ $<

unit: $(UNIT_TESTS)
	@printf '%s\n' '== unit =='
	./$(UNIT_TESTS)

harness: $(IDIOMC) $(ISH) $(PTY_DRIVER)
	@printf '%s\n' '== harness =='
	sh tools/run_tests.sh ./$(IDIOMC) ./$(ISH) output

test check: unit harness

snapshots: $(IDIOMC) $(ISH) $(PTY_DRIVER)
	sh tools/run_tests.sh ./$(IDIOMC) ./$(ISH) update

build/san/unit_tests: $(LIB_SRCS) $(TEST_SRCS)
	mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(SAN_FLAGS) -o $@ $(LIB_SRCS) $(TEST_SRCS) $(LDFLAGS)

build/san/idiomc: $(LIB_SRCS) $(CLI_SRCS)
	mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(SAN_FLAGS) -o $@ $(LIB_SRCS) $(CLI_SRCS) $(LDFLAGS)

build/san/ish: build/san/idiomc $(wildcard app/ish/*.id)
	./build/san/idiomc build app/ish -o $@

asan: build/san/unit_tests build/san/idiomc build/san/ish $(PTY_DRIVER)
	@printf '%s\n' '== asan/ubsan =='
	ASAN_OPTIONS=detect_leaks=1 ./build/san/unit_tests
	ASAN_OPTIONS=detect_leaks=1 sh tools/run_tests.sh ./build/san/idiomc ./build/san/ish san

sanitize: asan

memcheck: $(UNIT_TESTS) $(IDIOMC)
	$(VALGRIND) $(MEMCHECK_FLAGS) --log-file=build/valgrind-unit-tests.log ./$(UNIT_TESTS)
	$(VALGRIND) $(MEMCHECK_FLAGS) --log-file=build/valgrind-startup.log ./$(IDIOMC) tests/perf/idiom/startup.id
	$(VALGRIND) $(MEMCHECK_FLAGS) --log-file=build/valgrind-protocols.log ./$(IDIOMC) test tests/lang/provider_surface.id tests/lang/golden_package.id tests/lang/activation.id tests/lang/syntax_patterns.id
	$(VALGRIND) $(MEMCHECK_FLAGS) --log-file=build/valgrind-editor-buffer.log ./$(IDIOMC) tests/perf/idiom/editor_buffer.id
	@grep -E "in use at exit|ERROR SUMMARY" $(MEMCHECK_LOGS)

build/tsan/idiomc: $(LIB_SRCS) $(CLI_SRCS)
	mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(TSAN_FLAGS) -o $@ $(LIB_SRCS) $(CLI_SRCS) $(LDFLAGS)

tsan: build/tsan/idiomc
	./build/tsan/idiomc test tests/lang
	./build/tsan/idiomc test tests/app
	./build/tsan/idiomc repl < tests/repl/session.in >/dev/null

release: $(RELEASE_IDIOMC) $(RELEASE_ISH) $(RELEASE_NANI)

$(RELEASE_IDIOMC): $(LIB_SRCS) $(CLI_SRCS)
	mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(RELEASE_CFLAGS) -o $@ $(LIB_SRCS) $(CLI_SRCS) $(LDFLAGS)
	strip $@

$(RELEASE_ISH): $(RELEASE_IDIOMC) $(wildcard app/ish/*.id)
	./$(RELEASE_IDIOMC) build app/ish -o $@

$(RELEASE_NANI): $(RELEASE_IDIOMC) app/nani/pkg.id
	./$(RELEASE_IDIOMC) build app/nani -o $@

perf: MODES = sealed,source
perf: CACHE = hot
perf: release
	$(PERF_ENV) $(PYTHON) tools/perf_suite.py $(PERF_COMMON_ARGS) $(PERF_ARGS)

perf-quick: RUNS = 3
perf-quick: WARMUPS = 0
perf-quick: MODES = sealed,source
perf-quick: CACHE = hot
perf-quick: release
	$(PERF_ENV) $(PYTHON) tools/perf_suite.py $(PERF_COMMON_ARGS) $(PERF_ARGS)

perf-list:
	$(PERF_ENV) $(PYTHON) tools/perf_suite.py $(PERF_CASE_ARGS) --list $(PERF_ARGS)

perf-history: RUNTIMES = idiom,base
perf-history: MODES = sealed,source
perf-history: CACHE = hot
perf-history: release
	$(PERF_ENV) $(PYTHON) tools/perf_suite.py $(PERF_COMMON_ARGS) --base-ref $(BASE) $(PERF_ARGS)

perf-compare: perf-history

perf-build: MODES = build
perf-build: CACHE = hot,cold
perf-build: RUNTIMES = idiom
perf-build: release
	$(PERF_ENV) $(PYTHON) tools/perf_suite.py $(PERF_COMMON_ARGS) $(PERF_ARGS)

perf-cache: CACHE = hot,cold,off
perf-cache: MODES = source
perf-cache: RUNTIMES = idiom
perf-cache: release
	$(PERF_ENV) $(PYTHON) tools/perf_suite.py $(PERF_COMMON_ARGS) $(PERF_ARGS)

perf-sealed: MODES = sealed
perf-sealed: CACHE = hot
perf-sealed: RUNTIMES = idiom
perf-sealed: release
	$(PERF_ENV) $(PYTHON) tools/perf_suite.py $(PERF_COMMON_ARGS) $(PERF_ARGS)

perf-profile: CASES = $(PERF_PROFILE_CASES)
perf-profile: RUNTIMES = idiom
perf-profile: MODES = source
perf-profile: CACHE = hot
perf-profile: release
	$(PERF_ENV) $(PYTHON) tools/perf_suite.py $(PERF_COMMON_ARGS) --dump-dir build/perf-dumps --callgrind-dir build/perf-callgrind --json-out build/perf-profile.json $(PERF_ARGS)

perf-editor: CASES = $(PERF_EDITOR_CASES)
perf-editor: RUNTIMES = idiom,elisp
perf-editor: MODES = sealed
perf-editor: CACHE = hot
perf-editor: release
	$(PERF_ENV) $(PYTHON) tools/perf_suite.py $(PERF_COMMON_ARGS) $(PERF_ARGS)

clean:
	rm -rf build
	find . -iname "*.ic" -delete

-include $(DEPS)
