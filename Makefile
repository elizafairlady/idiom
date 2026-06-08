CC ?= cc
CFLAGS ?= -std=c11 -Wall -Wextra -Werror -pedantic -g -Iinclude
DEPFLAGS ?= -MMD -MP
LDFLAGS ?=

LIB_SRCS := \
 src/common/common.c \
 src/value/value.c \
 src/syntax/syntax.c \
 src/reader/reader.c \
 src/bytecode/bytecode.c \
 src/vm/vm.c \
 src/scope/scope.c \
 src/pattern/pattern.c \
 src/core/core.c \
 src/expand/expand.c

LIB_OBJS := $(LIB_SRCS:%.c=build/%.o)
CLI_OBJS := build/src/cli/main.o
TEST_OBJS := build/tests/unit/test_main.o
DEPS := $(LIB_OBJS:.o=.d) $(CLI_OBJS:.o=.d) $(TEST_OBJS:.o=.d)

.PHONY: all test clean

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
	printf 'foo bar\n' | ./build/ishc --dump-reader - >build/ish-reader-dump.txt
	printf 'x = 40\nadd x 2\n' | ./build/ishc --dump-core - >build/ish-core-dump.txt
	printf 'add 1 2\n' | ./build/ishc --dump-bytecode - >build/ish-bytecode-dump.txt
	printf '42\n' >build/expected-42.txt
	./build/ish --eval 'x = 40; add x 2' >build/ish-eval-out.txt
	cmp build/expected-42.txt build/ish-eval-out.txt
	./build/ish --eval 'inc = fn x -> add x 1; inc 41' >build/ish-fn-eval-out.txt
	cmp build/expected-42.txt build/ish-fn-eval-out.txt
	./build/ish --eval 'defn inc x -> x + 1; inc 41' >build/ish-defn-eval-out.txt
	cmp build/expected-42.txt build/ish-defn-eval-out.txt
	./build/ish --eval 'defn f x -> g x; defn g x -> x + 1; f 41' >build/ish-mutual-defn-eval-out.txt
	cmp build/expected-42.txt build/ish-mutual-defn-eval-out.txt
	./build/ish --eval 'defn f 0 -> 40; defn f n -> n; add (f 0) 2' >build/ish-multiclause-defn-eval-out.txt
	cmp build/expected-42.txt build/ish-multiclause-defn-eval-out.txt
	printf '15\n' >build/expected-15.txt
	./build/ish --eval 'defn sumdown n -> cond (n < 1) 0 (n + sumdown (n - 1)); sumdown 5' >build/ish-recursive-defn-eval-out.txt
	cmp build/expected-15.txt build/ish-recursive-defn-eval-out.txt
	./build/ish --eval 'match 0 do 0 -> 42; n -> n end' >build/ish-match-eval-out.txt
	cmp build/expected-42.txt build/ish-match-eval-out.txt
	./build/ish --eval 'match [1 2] do [1 2] -> 42; _ -> 0 end' >build/ish-list-match-eval-out.txt
	cmp build/expected-42.txt build/ish-list-match-eval-out.txt
	printf '1\n' >build/expected-1.txt
	./build/ish --eval 'match [1 2] do [h t] -> h end' >build/ish-list-bind-match-eval-out.txt
	cmp build/expected-1.txt build/ish-list-bind-match-eval-out.txt
	./build/ish --eval 'match [1 2 3] do [h . t] -> h end' >build/ish-list-rest-match-eval-out.txt
	cmp build/expected-1.txt build/ish-list-rest-match-eval-out.txt
	./build/ish --eval 'match {:ok 42} do {:ok 42} -> 42; _ -> 0 end' >build/ish-tuple-match-eval-out.txt
	cmp build/expected-42.txt build/ish-tuple-match-eval-out.txt
	printf '%%[2 3]\n' >build/expected-vector-rest.txt
	./build/ish --eval 'match %[1 2 3] do %[h . t] -> t end' >build/ish-vector-rest-match-eval-out.txt
	cmp build/expected-vector-rest.txt build/ish-vector-rest-match-eval-out.txt
	printf '{2 3}\n' >build/expected-tuple-rest.txt
	./build/ish --eval 'match {1 2 3} do {h . t} -> t end' >build/ish-tuple-rest-match-eval-out.txt
	cmp build/expected-tuple-rest.txt build/ish-tuple-rest-match-eval-out.txt
	./build/ish --eval 'match %{:a 42 :b 0} do %{:a 42} -> 42; _ -> 0 end' >build/ish-dict-match-eval-out.txt
	cmp build/expected-42.txt build/ish-dict-match-eval-out.txt
	./build/ish --eval 'match %{:a 42} do %{:a x} -> x end' >build/ish-dict-bind-match-eval-out.txt
	cmp build/expected-42.txt build/ish-dict-bind-match-eval-out.txt
	./build/ish --eval 'x = 2; x * 20 + 2' >build/ish-op-eval-out.txt
	cmp build/expected-42.txt build/ish-op-eval-out.txt
	./build/ish --eval 'defn f n when n < 0 -> 0; defn f n -> n; f 42' >build/ish-guard-defn-out.txt
	cmp build/expected-42.txt build/ish-guard-defn-out.txt
	./build/ish --eval 'defn g n when n == 42 -> 42; defn g n -> 0; g 42' >build/ish-guard-defn-eq-out.txt
	cmp build/expected-42.txt build/ish-guard-defn-eq-out.txt
	./build/ish --eval 'match 7 do n when n < 10 -> 42; _ -> 0 end' >build/ish-guard-match-out.txt
	cmp build/expected-42.txt build/ish-guard-match-out.txt
	./build/ishc --not-implemented-test future-phase >/dev/null 2>build/ish-ni-err.txt; test $$? -eq 2

clean:
	rm -rf build

-include $(DEPS)
