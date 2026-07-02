.DEFAULT_GOAL := build

CC ?= cc
VERSION := 0.72.0-dev
BUILD := build

CPPFLAGS ?= -D_POSIX_C_SOURCE=200809L -DIDM_VERSION=\"$(VERSION)\" -Iinclude
WARNINGS := -Wall -Wextra -Werror -pedantic
BASE_CFLAGS := -std=c11 $(WARNINGS)
DEBUG_CFLAGS := $(BASE_CFLAGS) -g
RELEASE_CFLAGS := $(BASE_CFLAGS) -O2 -g -DNDEBUG
ASAN_CFLAGS := $(BASE_CFLAGS) -O1 -g -fno-omit-frame-pointer -fsanitize=address
UBSAN_CFLAGS := $(BASE_CFLAGS) -O1 -g -fno-omit-frame-pointer -fsanitize=undefined
TSAN_CFLAGS := $(BASE_CFLAGS) -O1 -g -fno-omit-frame-pointer -fsanitize=thread
DEPFLAGS := -MMD -MP
LDLIBS := -lpthread -lm
ASAN_LDFLAGS := -fsanitize=address
UBSAN_LDFLAGS := -fsanitize=undefined
TSAN_LDFLAGS := -fsanitize=thread
ASAN_ENV := ASAN_OPTIONS=detect_leaks=0 LSAN_OPTIONS=detect_leaks=0
VALGRIND ?= valgrind
BENCH_COUNT ?= 1000
PROFILE_COUNT ?= 10
PROFILE_DIR := $(BUILD)/profile

LIB_SRCS := \
	src/common/common.c \
	src/bignum/bignum.c \
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
	src/expand/expand.c \
	src/expand/expand_body.c \
	src/expand/expand_context.c \
	src/expand/expand_declarations.c \
	src/expand/expand_packages.c \
	src/expand/expand_quote.c \
	src/expand/expand_surfaces.c \
	src/expand/expand_typecheck.c \
	src/infer/infer.c

CLI_SRCS := src/cli/main.c

UNIT_CASE_SRCS := \
	tests/unit/bytecode_record.c \
	tests/unit/closure_arity.c \
	tests/unit/byteprog.c \
	tests/unit/cli.c \
	tests/unit/gc.c \
	tests/unit/grow.c \
	tests/unit/intern.c \
	tests/unit/pattern_selector.c \
	tests/unit/primitive_registry.c \
	tests/unit/signature_contract.c \
	tests/unit/type_term.c \
	tests/unit/infer.c \
	tests/unit/reader_escape.c \
	tests/unit/repl.c \
	tests/unit/regex_set.c \
	tests/unit/syntax_equal.c \
	tests/unit/wire_helpers.c

UNIT_SRCS := $(UNIT_CASE_SRCS) tests/unit/runner.c

LANG_TESTS := tests/lang
PKG_TESTS := tests/pkg/bytecode_shape tests/pkg/cross_trait_order tests/pkg/reader_constructor_artifact tests/pkg/surface_macros
REAL_TESTS := tests/real
TEST_PATHS := $(LANG_TESTS) $(PKG_TESTS) $(REAL_TESTS)
REAL_PROG := tests/real/actor_regex_port.id
BOOT_PKG := tests/pkg/reader_constructor_artifact

IDIOMC := $(BUILD)/idiomc
RELEASE_IDIOMC := $(BUILD)/release/idiomc
ASAN_IDIOMC := $(BUILD)/asan/idiomc
UBSAN_IDIOMC := $(BUILD)/ubsan/idiomc
TSAN_IDIOMC := $(BUILD)/tsan/idiomc

UNIT := $(BUILD)/unit
ASAN_UNIT := $(BUILD)/asan/unit
UBSAN_UNIT := $(BUILD)/ubsan/unit
TSAN_UNIT := $(BUILD)/tsan/unit

DEBUG_LIB_OBJS := $(LIB_SRCS:%.c=$(BUILD)/obj/debug/%.o)
DEBUG_CLI_OBJS := $(CLI_SRCS:%.c=$(BUILD)/obj/debug/%.o)
DEBUG_UNIT_OBJS := $(UNIT_SRCS:%.c=$(BUILD)/obj/debug/%.o)
RELEASE_LIB_OBJS := $(LIB_SRCS:%.c=$(BUILD)/obj/release/%.o)
RELEASE_CLI_OBJS := $(CLI_SRCS:%.c=$(BUILD)/obj/release/%.o)
ASAN_LIB_OBJS := $(LIB_SRCS:%.c=$(BUILD)/obj/asan/%.o)
ASAN_CLI_OBJS := $(CLI_SRCS:%.c=$(BUILD)/obj/asan/%.o)
ASAN_UNIT_OBJS := $(UNIT_SRCS:%.c=$(BUILD)/obj/asan/%.o)
UBSAN_LIB_OBJS := $(LIB_SRCS:%.c=$(BUILD)/obj/ubsan/%.o)
UBSAN_CLI_OBJS := $(CLI_SRCS:%.c=$(BUILD)/obj/ubsan/%.o)
UBSAN_UNIT_OBJS := $(UNIT_SRCS:%.c=$(BUILD)/obj/ubsan/%.o)
TSAN_LIB_OBJS := $(LIB_SRCS:%.c=$(BUILD)/obj/tsan/%.o)
TSAN_CLI_OBJS := $(CLI_SRCS:%.c=$(BUILD)/obj/tsan/%.o)
TSAN_UNIT_OBJS := $(UNIT_SRCS:%.c=$(BUILD)/obj/tsan/%.o)

DEPS := \
	$(DEBUG_LIB_OBJS:.o=.d) $(DEBUG_CLI_OBJS:.o=.d) $(DEBUG_UNIT_OBJS:.o=.d) \
	$(RELEASE_LIB_OBJS:.o=.d) $(RELEASE_CLI_OBJS:.o=.d) \
	$(ASAN_LIB_OBJS:.o=.d) $(ASAN_CLI_OBJS:.o=.d) $(ASAN_UNIT_OBJS:.o=.d) \
	$(UBSAN_LIB_OBJS:.o=.d) $(UBSAN_CLI_OBJS:.o=.d) $(UBSAN_UNIT_OBJS:.o=.d) \
	$(TSAN_LIB_OBJS:.o=.d) $(TSAN_CLI_OBJS:.o=.d) $(TSAN_UNIT_OBJS:.o=.d)

.PHONY: all build release test check bench profile sanitize clean help
.PHONY: .real .roundtrip .bootstrap .profile-memcheck .profile-callgrind .profile-helgrind .asan .ubsan .tsan .ic-clean .assert-no-ic
.NOTPARALLEL: test check profile sanitize .profile-memcheck .profile-callgrind .profile-helgrind .asan .ubsan .tsan .roundtrip .bootstrap

all: build

build: $(IDIOMC)

release: $(RELEASE_IDIOMC)

test: $(IDIOMC) $(UNIT)
	IDIOMC_UNDER_TEST=$(abspath $(IDIOMC)) $(UNIT)
	$(IDIOMC) test $(TEST_PATHS)

check: $(IDIOMC) $(RELEASE_IDIOMC) $(UNIT) test .real .roundtrip .bootstrap bench sanitize
	$(MAKE) .ic-clean
	$(RELEASE_IDIOMC) test $(TEST_PATHS)
	$(MAKE) .ic-clean
	$(MAKE) .assert-no-ic

bench: $(RELEASE_IDIOMC)
	$(RELEASE_IDIOMC) test -bench . -count $(BENCH_COUNT) tests/bench

profile: .profile-memcheck .profile-callgrind .profile-helgrind

.profile-memcheck: $(RELEASE_IDIOMC)
	mkdir -p $(PROFILE_DIR)
	$(VALGRIND) --error-exitcode=99 --tool=memcheck --track-origins=yes --leak-check=full --show-leak-kinds=definite,possible --errors-for-leak-kinds=definite,possible --log-file=$(PROFILE_DIR)/memcheck.log $(RELEASE_IDIOMC) test -bench . -count $(PROFILE_COUNT) -json tests/bench > $(PROFILE_DIR)/memcheck.json

.profile-callgrind: $(RELEASE_IDIOMC)
	mkdir -p $(PROFILE_DIR)
	$(VALGRIND) --error-exitcode=99 --tool=callgrind --collect-jumps=yes --cache-sim=yes --callgrind-out-file=$(PROFILE_DIR)/callgrind.out --log-file=$(PROFILE_DIR)/callgrind.log $(RELEASE_IDIOMC) test -bench . -count $(PROFILE_COUNT) -json tests/bench > $(PROFILE_DIR)/callgrind.json

.profile-helgrind: $(RELEASE_IDIOMC)
	mkdir -p $(PROFILE_DIR)
	$(VALGRIND) --error-exitcode=99 --tool=helgrind --history-level=approx --log-file=$(PROFILE_DIR)/helgrind.log $(RELEASE_IDIOMC) test -bench . -count $(PROFILE_COUNT) -json tests/bench > $(PROFILE_DIR)/helgrind.json

.real: $(IDIOMC)
	$(IDIOMC) run $(REAL_PROG)

.roundtrip: $(IDIOMC)
	$(IDIOMC) build $(REAL_PROG) -o $(BUILD)/roundtrip_actor_regex_port.ic
	$(IDIOMC) run $(BUILD)/roundtrip_actor_regex_port.ic
	$(IDIOMC) build $(BOOT_PKG) -o $(BUILD)/roundtrip_reader_constructor_artifact.ic
	$(IDIOMC) run $(BUILD)/roundtrip_reader_constructor_artifact.ic

.bootstrap: $(IDIOMC) $(RELEASE_IDIOMC)
	$(IDIOMC) build $(BOOT_PKG) -o $(BUILD)/bootstrap_debug_reader_constructor_artifact.ic
	$(RELEASE_IDIOMC) build $(BOOT_PKG) -o $(BUILD)/bootstrap_release_reader_constructor_artifact.ic
	$(IDIOMC) run $(BUILD)/bootstrap_debug_reader_constructor_artifact.ic > $(BUILD)/bootstrap_debug.out
	$(RELEASE_IDIOMC) run $(BUILD)/bootstrap_release_reader_constructor_artifact.ic > $(BUILD)/bootstrap_release.out
	cmp $(BUILD)/bootstrap_debug.out $(BUILD)/bootstrap_release.out

sanitize: .asan .ubsan .tsan

.asan: $(ASAN_IDIOMC) $(ASAN_UNIT)
	IDIOMC_UNDER_TEST=$(abspath $(ASAN_IDIOMC)) $(ASAN_ENV) $(ASAN_UNIT)
	$(ASAN_ENV) $(ASAN_IDIOMC) test $(TEST_PATHS)

.ubsan: $(UBSAN_IDIOMC) $(UBSAN_UNIT)
	IDIOMC_UNDER_TEST=$(abspath $(UBSAN_IDIOMC)) $(UBSAN_UNIT)
	$(UBSAN_IDIOMC) test $(TEST_PATHS)

.tsan: $(TSAN_IDIOMC) $(TSAN_UNIT)
	IDIOMC_UNDER_TEST=$(abspath $(TSAN_IDIOMC)) $(TSAN_UNIT)
	$(TSAN_IDIOMC) test $(TEST_PATHS)

.ic-clean:
	find . -type f -iname '*.ic' -delete

.assert-no-ic:
	@found="$$(find . -type f -iname '*.ic' -print -quit)"; \
	if [ -n "$$found" ]; then \
		find . -type f -iname '*.ic' -print; \
		exit 1; \
	fi

help:
	@printf '%s\n' 'build release test check bench profile sanitize clean'

$(IDIOMC): $(DEBUG_LIB_OBJS) $(DEBUG_CLI_OBJS)
	mkdir -p $(dir $@)
	$(CC) -o $@ $^ $(LDLIBS)

$(UNIT): $(DEBUG_LIB_OBJS) $(DEBUG_UNIT_OBJS)
	mkdir -p $(dir $@)
	$(CC) -o $@ $^ $(LDLIBS)

$(RELEASE_IDIOMC): $(RELEASE_LIB_OBJS) $(RELEASE_CLI_OBJS)
	mkdir -p $(dir $@)
	$(CC) -o $@ $^ $(LDLIBS)

$(ASAN_IDIOMC): $(ASAN_LIB_OBJS) $(ASAN_CLI_OBJS)
	mkdir -p $(dir $@)
	$(CC) $(ASAN_LDFLAGS) -o $@ $^ $(LDLIBS)

$(ASAN_UNIT): $(ASAN_LIB_OBJS) $(ASAN_UNIT_OBJS)
	mkdir -p $(dir $@)
	$(CC) $(ASAN_LDFLAGS) -o $@ $^ $(LDLIBS)

$(UBSAN_IDIOMC): $(UBSAN_LIB_OBJS) $(UBSAN_CLI_OBJS)
	mkdir -p $(dir $@)
	$(CC) $(UBSAN_LDFLAGS) -o $@ $^ $(LDLIBS)

$(UBSAN_UNIT): $(UBSAN_LIB_OBJS) $(UBSAN_UNIT_OBJS)
	mkdir -p $(dir $@)
	$(CC) $(UBSAN_LDFLAGS) -o $@ $^ $(LDLIBS)

$(TSAN_IDIOMC): $(TSAN_LIB_OBJS) $(TSAN_CLI_OBJS)
	mkdir -p $(dir $@)
	$(CC) $(TSAN_LDFLAGS) -o $@ $^ $(LDLIBS)

$(TSAN_UNIT): $(TSAN_LIB_OBJS) $(TSAN_UNIT_OBJS)
	mkdir -p $(dir $@)
	$(CC) $(TSAN_LDFLAGS) -o $@ $^ $(LDLIBS)

$(BUILD)/obj/debug/%.o: %.c
	mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(DEBUG_CFLAGS) $(DEPFLAGS) -c -o $@ $<

$(BUILD)/obj/release/%.o: %.c
	mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(RELEASE_CFLAGS) $(DEPFLAGS) -c -o $@ $<

$(BUILD)/obj/asan/%.o: %.c
	mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(ASAN_CFLAGS) $(DEPFLAGS) -c -o $@ $<

$(BUILD)/obj/ubsan/%.o: %.c
	mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(UBSAN_CFLAGS) $(DEPFLAGS) -c -o $@ $<

$(BUILD)/obj/tsan/%.o: %.c
	mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(TSAN_CFLAGS) $(DEPFLAGS) -c -o $@ $<

clean:
	rm -rf $(BUILD)

-include $(DEPS)
