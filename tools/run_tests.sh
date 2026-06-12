#!/bin/sh
set -u
IDIOMC="$1"
ISH="$2"
MODE="$3"
fail=0

san_check() {
    if grep -qiE 'leaked|AddressSanitizer:|UndefinedBehavior' "$1"; then
        echo "SANITIZE FAIL: $2"
        cat "$1"
        fail=1
    fi
}

if [ "$MODE" = "output" ]; then
    "$IDIOMC" test tests/lang || fail=1
    IDIOMMAXPROCS=1 "$IDIOMC" test tests/lang >/dev/null 2>build/lang-n1.err || { echo "LANG FAIL (IDIOMMAXPROCS=1)"; cat build/lang-n1.err; fail=1; }
    echo "lang suites passed (IDIOMMAXPROCS=1)"
else
    "$IDIOMC" test tests/lang >/dev/null 2>build/san-lang.err || true
    san_check build/san-lang.err "lang suites"
    IDIOMMAXPROCS=1 "$IDIOMC" test tests/lang >/dev/null 2>build/san-lang-n1.err || true
    san_check build/san-lang-n1.err "lang suites (IDIOMMAXPROCS=1)"
fi

for f in tests/golden/*.id; do
    name=$(basename "$f" .id)
    if [ "$MODE" = "output" ]; then
        "$IDIOMC" --eval "$(cat "$f")" >"build/golden-$name.out" 2>"build/golden-$name.err"
        if [ -s "build/golden-$name.err" ]; then
            echo "GOLDEN STDERR: $name"; cat "build/golden-$name.err"; fail=1
        elif ! cmp -s "tests/golden/$name.out" "build/golden-$name.out"; then
            echo "GOLDEN FAIL: $name"; diff "tests/golden/$name.out" "build/golden-$name.out" || true; fail=1
        fi
    else
        "$IDIOMC" --eval "$(cat "$f")" >/dev/null 2>"build/san-golden-$name.err" || true
        san_check "build/san-golden-$name.err" "golden $name"
    fi
done
[ "$MODE" = "output" ] && echo "goldens passed ($(ls tests/golden/*.id | wc -l) cases)"

for f in tests/run/*.id; do
    name=$(basename "$f" .id)
    if [ "$MODE" = "output" ]; then
        "$IDIOMC" "$f" >"build/run-$name.out" 2>"build/run-$name.err"
        if [ -s "build/run-$name.err" ]; then
            echo "RUN STDERR: $name"; cat "build/run-$name.err"; fail=1
        elif ! cmp -s "tests/run/$name.out" "build/run-$name.out"; then
            echo "RUN FAIL: $name"; diff "tests/run/$name.out" "build/run-$name.out" || true; fail=1
        fi
    else
        "$IDIOMC" "$f" >/dev/null 2>"build/san-run-$name.err" || true
        san_check "build/san-run-$name.err" "run $name"
    fi
done

for f in tests/diag/*.id; do
    name=$(basename "$f" .id)
    if [ "$MODE" = "output" ]; then
        if "$IDIOMC" "$f" >"build/diag-$name.out" 2>"build/diag-$name.err"; then
            echo "DIAG UNEXPECTED SUCCESS: $name"; fail=1
        elif ! cmp -s "tests/diag/$name.err" "build/diag-$name.err"; then
            echo "DIAG FAIL: $name"; diff "tests/diag/$name.err" "build/diag-$name.err" || true; fail=1
        fi
    else
        "$IDIOMC" "$f" >/dev/null 2>"build/san-diag-$name.err" || true
        san_check "build/san-diag-$name.err" "diag $name"
    fi
done

for f in tests/shell/*.ish; do
    name=$(basename "$f" .ish)
    if [ "$MODE" = "output" ]; then
        "$ISH" "$f" >"build/shell-$name.out" 2>"build/shell-$name.err"
        if [ -s "build/shell-$name.err" ]; then
            echo "SHELL STDERR: $name"; cat "build/shell-$name.err"; fail=1
        elif ! cmp -s "tests/shell/$name.out" "build/shell-$name.out"; then
            echo "SHELL FAIL: $name"; diff "tests/shell/$name.out" "build/shell-$name.out" || true; fail=1
        fi
    else
        "$ISH" "$f" >/dev/null 2>"build/san-shell-$name.err" || true
        san_check "build/san-shell-$name.err" "shell $name"
    fi
done

for f in tests/shell/fail/*.ish; do
    name=$(basename "$f" .ish)
    if [ "$MODE" = "output" ]; then
        "$ISH" "$f" >"build/shellfail-$name.out" 2>"build/shellfail-$name.err"
        rc=$?
        want=$(cat "tests/shell/fail/$name.status")
        if [ "$rc" -ne "$want" ]; then
            echo "SHELL-FAIL STATUS: $name (got $rc want $want)"; fail=1
        elif ! cmp -s "tests/shell/fail/$name.err" "build/shellfail-$name.err"; then
            echo "SHELL-FAIL STDERR: $name"; diff "tests/shell/fail/$name.err" "build/shellfail-$name.err" || true; fail=1
        elif ! cmp -s "tests/shell/fail/$name.out" "build/shellfail-$name.out"; then
            echo "SHELL-FAIL OUT: $name"; diff "tests/shell/fail/$name.out" "build/shellfail-$name.out" || true; fail=1
        fi
    else
        "$ISH" "$f" >/dev/null 2>"build/san-shellfail-$name.err" || true
        san_check "build/san-shellfail-$name.err" "shell-fail $name"
    fi
done

if [ "$MODE" = "output" ]; then
    "$IDIOMC" build tests/seal/tool.id -o build/sealed-tool >/dev/null 2>build/seal.err
    if [ -s build/seal.err ]; then
        echo "SEAL BUILD FAIL"; cat build/seal.err; fail=1
    else
        env -u IDIOMROOT -u IDIOMPATH "$PWD/$IDIOMC" build/sealed-tool one two > build/sealed-run.out 2>build/sealed-run.err
        if [ -s build/sealed-run.err ] || ! cmp -s tests/seal/tool.out build/sealed-run.out; then
            echo "SEAL RUN FAIL"; cat build/sealed-run.err; diff tests/seal/tool.out build/sealed-run.out || true; fail=1
        else
            echo "sealed program passed (1 case)"
        fi
    fi

    "$IDIOMC" repl < tests/repl/session.in > build/repl-session.out 2>build/repl-session.err
    if ! cmp -s tests/repl/session.err build/repl-session.err || ! cmp -s tests/repl/session.out build/repl-session.out; then
        echo "REPL FAIL"; diff tests/repl/session.err build/repl-session.err || true; diff tests/repl/session.out build/repl-session.out || true; fail=1
    fi
    IDIOMMAXPROCS=1 "$IDIOMC" repl < tests/repl/session.in > build/repl-session-n1.out 2>build/repl-session-n1.err
    if ! cmp -s tests/repl/session.err build/repl-session-n1.err || ! cmp -s tests/repl/session.out build/repl-session-n1.out; then
        echo "REPL FAIL (IDIOMMAXPROCS=1)"; diff tests/repl/session.err build/repl-session-n1.err || true; diff tests/repl/session.out build/repl-session-n1.out || true; fail=1
    else
        echo "repl session passed (2 cases)"
    fi

    "$IDIOMC" --dump-surface > build/dump-surface-default.out 2>/dev/null
    cmp -s tests/dump/surface-default.out build/dump-surface-default.out || { echo "DUMP FAIL: surface-default"; diff tests/dump/surface-default.out build/dump-surface-default.out; fail=1; }
    "$IDIOMC" --dump-surface 'implement std/shell' > build/dump-surface-shell.out 2>/dev/null
    cmp -s tests/dump/surface-shell.out build/dump-surface-shell.out || { echo "DUMP FAIL: surface-shell"; diff tests/dump/surface-shell.out build/dump-surface-shell.out; fail=1; }

    printf 'foo bar\n' | "$IDIOMC" --dump-reader - >/dev/null || fail=1
    printf 'implement std/shell\necho hello\n' | "$IDIOMC" --dump-core - | grep -q 'prim exec' || { echo "PIN FAIL: exec"; fail=1; }
    printf 'implement std/shell\nls -la | wc -l\n' | "$IDIOMC" --dump-core - | grep -q ':pipeline' || { echo "PIN FAIL: pipeline"; fail=1; }
    printf 'echo hello\n' | "$IDIOMC" --dump-core - 2>&1 | grep -q "unbound identifier 'echo'" || { echo "PIN FAIL: no-shell"; fail=1; }
    printf 'implement std/shell\nls *.zz\n' | "$IDIOMC" --dump-core - | grep -q ':glob' || { echo "PIN FAIL: glob"; fail=1; }
    printf 'implement std/shell\nls nl*\n' | "$IDIOMC" --dump-core - | grep -q ':glob "nl\*"' || { echo "PIN FAIL: glob-joined"; fail=1; }
    printf 'implement std/shell\nsh -c x 2>&1\n' | "$IDIOMC" --dump-core - | grep -q ':dup' || { echo "PIN FAIL: dup"; fail=1; }
    printf 'implement std/shell\nFOO=bar printenv FOO\n' | "$IDIOMC" --dump-core - | grep -q '"FOO"' || { echo "PIN FAIL: env"; fail=1; }
    printf 'implement std/shell\necho a$X.txt\n' | "$IDIOMC" --dump-core - | grep -q ':cat' || { echo "PIN FAIL: cat"; fail=1; }
    printf 'implement std/shell\necho hi > out.txt\n' | "$IDIOMC" --dump-core - | grep -q ':lit "out.txt"' || { echo "PIN FAIL: redirect-join"; fail=1; }
fi

if [ "$fail" -ne 0 ]; then
    echo "harness: FAILURES ($MODE)"
    exit 1
fi
echo "harness: all lanes passed ($MODE)"
