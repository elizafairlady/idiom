CC ?= cc
CFLAGS ?= -std=c11 -Wall -Wextra -Werror -pedantic -g -D_POSIX_C_SOURCE=200809L -Iinclude
DEPFLAGS ?= -MMD -MP
LDFLAGS ?= -lm

LIB_SRCS := \
 src/common/common.c \
 src/value/value.c \
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
 src/artifact/artifact.c \
 $(wildcard src/expand/*.c)

LIB_OBJS := $(LIB_SRCS:%.c=build/%.o)
CLI_OBJS := build/src/cli/main.o
TEST_SRCS := $(wildcard tests/unit/*.c)
TEST_OBJS := $(patsubst %.c,build/%.o,$(TEST_SRCS))
DEPS := $(LIB_OBJS:.o=.d) $(CLI_OBJS:.o=.d) $(TEST_OBJS:.o=.d)

.PHONY: all test sanitize clean

SAN_FLAGS := -fsanitize=address,undefined -fno-omit-frame-pointer

all: build/ishc build/ish

build/ishc: $(LIB_OBJS) $(CLI_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

build/ish: build/ishc
	cp build/ishc build/ish

build/unit_tests: $(LIB_OBJS) $(TEST_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

build/%.o: %.c
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(DEPFLAGS) -c -o $@ $<

test: build/unit_tests build/ishc build/ish
	./build/unit_tests
	printf 'foo bar\n' | ./build/ishc --dump-reader - >/dev/null
	printf 'x = 40\nadd x 2\n' | ./build/ishc --dump-core - >/dev/null
	printf 'add 1 2\n' | ./build/ishc --dump-bytecode - >/dev/null
	printf 'implements std/shell\necho hello\n' | ./build/ishc --dump-core - | grep -q 'prim exec'
	printf 'implements std/shell\nls -la | wc -l\n' | ./build/ishc --dump-core - | grep -q ':pipeline'
	printf 'echo hello\n' | ./build/ishc --dump-core - 2>&1 | grep -q "unbound identifier 'echo'"
	printf 'g = &nope\n' | ./build/ishc --dump-core - 2>&1 | grep -q "unbound identifier 'nope'"
	printf 'implements std/shell\nls *.zz\n' | ./build/ishc --dump-core - | grep -q ':glob'
	printf 'implements std/shell\nsh -c x 2>&1\n' | ./build/ishc --dump-core - | grep -q ':dup'
	printf 'implements std/shell\nFOO=bar printenv FOO\n' | ./build/ishc --dump-core - | grep -q '"FOO"'
	@for f in tests/golden/*.ish; do \
		name=$$(basename "$$f" .ish); \
		./build/ish --eval "$$(cat "$$f")" >"build/golden-$$name.out" 2>"build/golden-$$name.err"; \
		if [ -s "build/golden-$$name.err" ]; then \
			echo "GOLDEN STDERR (unexpected error): $$name"; \
			cat "build/golden-$$name.err"; \
			exit 1; \
		fi; \
		if ! cmp -s "tests/golden/$$name.out" "build/golden-$$name.out"; then \
			echo "GOLDEN FAIL: $$name"; \
			diff "tests/golden/$$name.out" "build/golden-$$name.out" || true; \
			exit 1; \
		fi; \
	done
	@echo "goldens passed ($$(ls tests/golden/*.ish | wc -l) cases)"
	@for f in tests/run/*.ish; do \
		name=$$(basename "$$f" .ish); \
		./build/ish "$$f" >"build/run-$$name.out" 2>"build/run-$$name.err"; \
		if [ -s "build/run-$$name.err" ]; then \
			echo "RUN STDERR (unexpected error): $$name"; \
			cat "build/run-$$name.err"; \
			exit 1; \
		fi; \
		if ! cmp -s "tests/run/$$name.out" "build/run-$$name.out"; then \
			echo "RUN FAIL: $$name"; \
			diff "tests/run/$$name.out" "build/run-$$name.out" || true; \
			exit 1; \
		fi; \
	done
	@echo "run scripts passed ($$(ls tests/run/*.ish | wc -l) cases)"
	@for f in tests/diag/*.ish; do \
		name=$$(basename "$$f" .ish); \
		if ./build/ish "$$f" >"build/diag-$$name.out" 2>"build/diag-$$name.err"; then \
			echo "DIAG UNEXPECTED SUCCESS: $$name"; exit 1; \
		fi; \
		if ! cmp -s "tests/diag/$$name.err" "build/diag-$$name.err"; then \
			echo "DIAG FAIL: $$name"; \
			diff "tests/diag/$$name.err" "build/diag-$$name.err" || true; \
			exit 1; \
		fi; \
	done
	@echo "diag goldens passed ($$(ls tests/diag/*.ish | wc -l) cases)"
	@./build/ish --dump-surface > build/dump-surface-default.out 2>/dev/null
	@cmp -s tests/dump/surface-default.out build/dump-surface-default.out || { echo "DUMP FAIL: surface-default"; diff tests/dump/surface-default.out build/dump-surface-default.out; exit 1; }
	@./build/ish --dump-surface 'implements std/shell' > build/dump-surface-shell.out 2>/dev/null
	@cmp -s tests/dump/surface-shell.out build/dump-surface-shell.out || { echo "DUMP FAIL: surface-shell"; diff tests/dump/surface-shell.out build/dump-surface-shell.out; exit 1; }
	@echo "dump goldens passed (2 cases)"

build/unit_tests_san: $(LIB_SRCS) $(TEST_SRCS)
	mkdir -p build
	$(CC) $(CFLAGS) $(SAN_FLAGS) -o $@ $(LIB_SRCS) $(TEST_SRCS) $(LDFLAGS)

build/san/ish: $(LIB_SRCS) src/cli/main.c
	mkdir -p build/san
	$(CC) $(CFLAGS) $(SAN_FLAGS) -o $@ $(LIB_SRCS) src/cli/main.c $(LDFLAGS)

sanitize: build/unit_tests_san build/san/ish
	ASAN_OPTIONS=detect_leaks=1 ./build/unit_tests_san
	@for f in tests/golden/*.ish; do \
		name=$$(basename "$$f" .ish); \
		ASAN_OPTIONS=detect_leaks=1 ./build/san/ish --eval "$$(cat "$$f")" >/dev/null 2>"build/san-$$name.err" || true; \
		if grep -qiE 'leaked|runtime error|AddressSanitizer:|UndefinedBehavior' "build/san-$$name.err"; then \
			echo "SANITIZE FAIL: $$name"; cat "build/san-$$name.err"; exit 1; \
		fi; \
	done
	@echo "sanitize goldens passed ($$(ls tests/golden/*.ish | wc -l) cases)"
	@for f in tests/run/*.ish; do \
		name=$$(basename "$$f" .ish); \
		ASAN_OPTIONS=detect_leaks=1 ./build/san/ish "$$f" >/dev/null 2>"build/san-run-$$name.err" || true; \
		if grep -qiE 'leaked|runtime error|AddressSanitizer:|UndefinedBehavior' "build/san-run-$$name.err"; then \
			echo "SANITIZE RUN FAIL: $$name"; cat "build/san-run-$$name.err"; exit 1; \
		fi; \
	done
	@echo "sanitize run scripts passed ($$(ls tests/run/*.ish | wc -l) cases)"
	@for f in tests/diag/*.ish; do \
		name=$$(basename "$$f" .ish); \
		ASAN_OPTIONS=detect_leaks=1 ./build/san/ish "$$f" >/dev/null 2>"build/san-diag-$$name.err" || true; \
		if grep -qiE 'leaked|AddressSanitizer:|UndefinedBehavior' "build/san-diag-$$name.err"; then \
			echo "SANITIZE DIAG FAIL: $$name"; cat "build/san-diag-$$name.err"; exit 1; \
		fi; \
	done
	@echo "sanitize diag goldens passed ($$(ls tests/diag/*.ish | wc -l) cases)"

conformance: test sanitize
	@sh tools/conformance_check.sh
	@echo "conformance suite passed"

clean:
	rm -rf build

-include $(DEPS)
