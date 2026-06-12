CC ?= cc
VERSION := 0.1.0-dev
CFLAGS ?= -std=c11 -Wall -Wextra -Werror -pedantic -g -D_POSIX_C_SOURCE=200809L -Iinclude -DIDM_VERSION=\"$(VERSION)\"
DEPFLAGS ?= -MMD -MP
LDFLAGS ?= -lpthread -lm

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
  src/tty/tty.c \
 src/artifact/artifact.c \
 $(wildcard src/expand/*.c)

LIB_OBJS := $(LIB_SRCS:%.c=build/%.o)
CLI_OBJS := build/src/cli/main.o
TEST_SRCS := $(wildcard tests/unit/*.c)
TEST_OBJS := $(patsubst %.c,build/%.o,$(TEST_SRCS))
DEPS := $(LIB_OBJS:.o=.d) $(CLI_OBJS:.o=.d) build/src/cli/ish.d $(TEST_OBJS:.o=.d)

.PHONY: all test sanitize tsan conformance release clean snapshots

SAN_FLAGS := -fsanitize=address,undefined -fno-omit-frame-pointer

all: build/idiomc build/ish

build/idiomc: $(LIB_OBJS) $(CLI_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

build/ish: $(LIB_OBJS) build/src/cli/ish.o
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

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

build/san/ish: $(LIB_SRCS) src/cli/ish.c
	mkdir -p build/san
	$(CC) $(CFLAGS) $(SAN_FLAGS) -o $@ $(LIB_SRCS) src/cli/ish.c $(LDFLAGS)

build/san/idiomc: $(LIB_SRCS) src/cli/main.c
	mkdir -p build/san
	$(CC) $(CFLAGS) $(SAN_FLAGS) -o $@ $(LIB_SRCS) src/cli/main.c $(LDFLAGS)

sanitize: build/unit_tests_san build/san/idiomc build/san/ish build/pty_driver
	ASAN_OPTIONS=detect_leaks=1 ./build/unit_tests_san
	@ASAN_OPTIONS=detect_leaks=1 sh tools/run_tests.sh ./build/san/idiomc ./build/san/ish san

release: 
	mkdir -p build/release
	$(CC) -std=c11 -O2 -DNDEBUG -DIDM_VERSION=\"$(VERSION)\" -D_POSIX_C_SOURCE=200809L -Iinclude -o build/release/idiomc $(LIB_SRCS) src/cli/main.c $(LDFLAGS)
	$(CC) -std=c11 -O2 -DNDEBUG -DIDM_VERSION=\"$(VERSION)\" -D_POSIX_C_SOURCE=200809L -Iinclude -o build/release/ish $(LIB_SRCS) src/cli/ish.c $(LDFLAGS)
	strip build/release/idiomc build/release/ish

build/tsan/idiomc: $(LIB_SRCS) src/cli/main.c
	mkdir -p build/tsan
	$(CC) -std=c11 -g -O1 -fsanitize=thread -D_POSIX_C_SOURCE=200809L -DIDM_VERSION=\"$(VERSION)\" -Iinclude -o $@ $(LIB_SRCS) src/cli/main.c $(LDFLAGS)

tsan: build/tsan/idiomc
	./build/tsan/idiomc test tests/lang
	./build/tsan/idiomc repl < tests/repl/session.in >/dev/null

conformance: test sanitize tsan
	@sh tools/conformance_check.sh
	@echo "conformance suite passed"

clean:
	rm -rf build

-include $(DEPS)

snapshots: build/unit_tests build/idiomc build/ish build/pty_driver
	@sh tools/run_tests.sh ./build/idiomc ./build/ish update
