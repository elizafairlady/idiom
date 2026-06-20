#!/bin/sh
set -u
IDIOMC="$1"
ISH="$2"
MODE="$3"
fail=0
updated=0
ROOT="$PWD"
IDIOMC_DIR=$(dirname "$IDIOMC")
case "$IDIOMC_DIR" in
/*) ;;
*) IDIOMC_DIR="$PWD/$IDIOMC_DIR" ;;
esac
PATH="$IDIOMC_DIR:$PATH"
TEST_IDIOMPATH="$ROOT${IDIOMPATH:+:$IDIOMPATH}"
export PATH

san_check() {
    if grep -qiE 'leaked|AddressSanitizer:|UndefinedBehavior' "$1"; then
        echo "SANITIZE FAIL: $2"
        cat "$1"
        fail=1
    fi
}

pin_check() {
    if [ "$MODE" = "update" ]; then
        if ! cmp -s "$1" "$2" 2>/dev/null; then
            cp "$2" "$1"; echo "UPDATED: $1"; updated=$((updated+1))
        fi
    elif ! cmp -s "$1" "$2"; then
        echo "$3"; diff "$1" "$2" || true; fail=1
    fi
}

status_pin() {
    if [ "$MODE" = "update" ]; then
        if [ "$4" -ne "$5" ]; then
            printf '%s\n' "$4" > "$1"; echo "UPDATED: $1"; updated=$((updated+1))
        fi
    elif [ "$4" -ne "$5" ]; then
        echo "$3 (got $4 want $5)"; fail=1
    fi
}

if [ "$MODE" != "san" ]; then
    "$IDIOMC" eval 'add 40 2' > build/cli-eval.out 2>build/cli-eval.err
    if [ -s build/cli-eval.err ] || ! grep -qx '42' build/cli-eval.out; then
        echo "CLI FAIL: eval"; cat build/cli-eval.err; fail=1
    fi
    printf 'use std/string\nargs |> join "," |> println\n' | "$IDIOMC" run - -- alpha beta > build/cli-run-stdin.out 2>build/cli-run-stdin.err
    if [ -s build/cli-run-stdin.err ] || ! grep -qx 'alpha,beta' build/cli-run-stdin.out; then
        echo "CLI FAIL: run stdin args"; cat build/cli-run-stdin.err; fail=1
    fi
    "$IDIOMC" dump bytecode tests/perf/idiom/startup.id >/dev/null 2>build/cli-dump-bytecode.err || { echo "CLI FAIL: dump bytecode"; cat build/cli-dump-bytecode.err; fail=1; }

    "$IDIOMC" test tests/lang || fail=1
    if [ -d tests/app ]; then
        "$IDIOMC" test tests/app || fail=1
    fi
    IDIOMMAXPROCS=1 "$IDIOMC" test tests/lang >/dev/null 2>build/lang-n1.err || { echo "LANG FAIL (IDIOMMAXPROCS=1)"; cat build/lang-n1.err; fail=1; }
    if [ -d tests/app ]; then
        IDIOMMAXPROCS=1 "$IDIOMC" test tests/app >/dev/null 2>build/app-n1.err || { echo "APP FAIL (IDIOMMAXPROCS=1)"; cat build/app-n1.err; fail=1; }
    fi
    echo "lang suites passed (IDIOMMAXPROCS=1)"
    # actor-reap floor: 1,000,000 spawn+join must stay flat (corpses-not-reaped would be ~590MB).
    "$IDIOMC" tests/perf/actor-reap.id >/dev/null 2>&1 &
    reap_pid=$!; reap_peak=0
    while kill -0 "$reap_pid" 2>/dev/null; do
        r=$(awk '/VmRSS/{print $2}' "/proc/$reap_pid/status" 2>/dev/null)
        [ -n "$r" ] && [ "$r" -gt "$reap_peak" ] && reap_peak=$r
        sleep 0.02
    done
    if ! wait "$reap_pid"; then echo "ACTOR-REAP FAIL: nonzero exit"; fail=1
    elif [ "$reap_peak" -gt 102400 ]; then echo "ACTOR-REAP FAIL: peak RSS $((reap_peak/1024))MB > 100MB ceiling (leak returned)"; fail=1
    else echo "actor-reap floor passed (peak $((reap_peak/1024))MB over 1,000,000 spawn+join)"; fi
else
    "$IDIOMC" test tests/lang >/dev/null 2>build/san-lang.err || true
    san_check build/san-lang.err "lang suites"
    if [ -d tests/app ]; then
        "$IDIOMC" test tests/app >/dev/null 2>build/san-app.err || true
        san_check build/san-app.err "app suites"
    fi
    IDIOMMAXPROCS=1 "$IDIOMC" test tests/lang >/dev/null 2>build/san-lang-n1.err || true
    san_check build/san-lang-n1.err "lang suites (IDIOMMAXPROCS=1)"
    if [ -d tests/app ]; then
        IDIOMMAXPROCS=1 "$IDIOMC" test tests/app >/dev/null 2>build/san-app-n1.err || true
        san_check build/san-app-n1.err "app suites (IDIOMMAXPROCS=1)"
    fi
fi

for f in tests/golden/*.id; do
    name=$(basename "$f" .id)
    if [ "$MODE" != "san" ]; then
        "$IDIOMC" eval "$(cat "$f")" >"build/golden-$name.out" 2>"build/golden-$name.err"
        if [ -s "build/golden-$name.err" ]; then
            echo "GOLDEN STDERR: $name"; cat "build/golden-$name.err"; fail=1
        else
            pin_check "tests/golden/$name.out" "build/golden-$name.out" "GOLDEN FAIL: $name"
        fi
    else
        "$IDIOMC" eval "$(cat "$f")" >/dev/null 2>"build/san-golden-$name.err" || true
        san_check "build/san-golden-$name.err" "golden $name"
    fi
done
[ "$MODE" != "san" ] && echo "goldens passed ($(ls tests/golden/*.id | wc -l) cases)"

for f in tests/run/*.id; do
    name=$(basename "$f" .id)
    if [ "$MODE" != "san" ]; then
        "$IDIOMC" "$f" >"build/run-$name.out" 2>"build/run-$name.err"
        if [ -s "build/run-$name.err" ]; then
            echo "RUN STDERR: $name"; cat "build/run-$name.err"; fail=1
        else
            pin_check "tests/run/$name.out" "build/run-$name.out" "RUN FAIL: $name"
        fi
    else
        "$IDIOMC" "$f" >/dev/null 2>"build/san-run-$name.err" || true
        san_check "build/san-run-$name.err" "run $name"
    fi
done

for f in tests/diag/*.id; do
    name=$(basename "$f" .id)
    if [ "$MODE" != "san" ]; then
        if "$IDIOMC" "$f" >"build/diag-$name.out" 2>"build/diag-$name.err"; then
            echo "DIAG UNEXPECTED SUCCESS: $name"; fail=1
        else
            pin_check "tests/diag/$name.err" "build/diag-$name.err" "DIAG FAIL: $name"
        fi
    else
        "$IDIOMC" "$f" >/dev/null 2>"build/san-diag-$name.err" || true
        san_check "build/san-diag-$name.err" "diag $name"
    fi
done

for f in tests/ish/*.keys; do
    name=$(basename "$f" .keys)
    case "$name" in
    session_*)
        home="$ROOT/build/ish-home-$name"
        rm -rf "$home"
        mkdir -p "$home"
        extra_env=
        pty_env=
        case "$name" in
        session_history_cap) extra_env="ISH_HISTSIZE=2" ;;
        session_dumb) pty_env="PTY_TERM=dumb" ;;
        esac
        if [ -e "tests/ish/$name.rc" ]; then
            mkdir -p "$home/.config/ish"
            cp "tests/ish/$name.rc" "$home/.config/ish/rc.ish"
        fi
        [ -e "tests/ish/$name.ishrc" ] && cp "tests/ish/$name.ishrc" "$home/.ishrc"
        want=0
        [ -e "tests/ish/$name.status" ] && want=$(cat "tests/ish/$name.status")
        if [ "$MODE" != "san" ]; then
            (cd "$home" && env -u XDG_CONFIG_HOME -u XDG_STATE_HOME -u ISH_HISTSIZE \
                $extra_env $pty_env HOME="$home" IDIOMROOT="$ROOT/std" IDIOMPATH="$TEST_IDIOMPATH" ISH_HISTFILE=hist \
                "$ROOT/build/pty_driver" "$ROOT/$f" "$ROOT/$ISH") >"build/ish-$name.out" 2>"build/ish-$name.err"
            rc=$?
            status_pin "tests/ish/$name.status" "" "ISH SESSION STATUS: $name" "$rc" "$want"
            pin_check "tests/ish/$name.out" "build/ish-$name.out" "ISH SESSION FAIL: $name"
        else
            (cd "$home" && env -u XDG_CONFIG_HOME -u XDG_STATE_HOME -u ISH_HISTSIZE \
                $extra_env $pty_env HOME="$home" IDIOMROOT="$ROOT/std" IDIOMPATH="$TEST_IDIOMPATH" ISH_HISTFILE=hist \
                "$ROOT/build/pty_driver" "$ROOT/$f" "$ROOT/$ISH") >"build/san-ish-$name.out" 2>/dev/null || true
            san_check "build/san-ish-$name.out" "ish session $name"
        fi
        ;;
    *)
        prog="tests/ish/$name.id"
        [ -e "$prog" ] || prog="tests/ish/edit_driver.id"
        if [ "$MODE" != "san" ]; then
            ./build/pty_driver "$f" "$IDIOMC" "$prog" >"build/ish-$name.out" 2>"build/ish-$name.err"
            rc=$?
            if [ "$rc" -ne 0 ]; then
                echo "ISH PTY FAIL: $name (status $rc)"; cat "build/ish-$name.err"; fail=1
            else
                pin_check "tests/ish/$name.out" "build/ish-$name.out" "ISH PTY FAIL: $name"
            fi
        else
            ./build/pty_driver "$f" "$IDIOMC" "$prog" >"build/san-ish-$name.out" 2>/dev/null || true
            san_check "build/san-ish-$name.out" "ish pty $name"
        fi
        ;;
    esac
done
if [ "$MODE" != "san" ]; then
    for procs in 1 4; do
        name=session_suspend_fg
        home="$ROOT/build/ish-home-$name-n$procs"
        rm -rf "$home"
        mkdir -p "$home"
        (cd "$home" && env -u XDG_CONFIG_HOME -u XDG_STATE_HOME -u ISH_HISTSIZE \
            HOME="$home" IDIOMROOT="$ROOT/std" IDIOMPATH="$TEST_IDIOMPATH" ISH_HISTFILE=hist IDIOMMAXPROCS=$procs \
            "$ROOT/build/pty_driver" "$ROOT/tests/ish/$name.keys" "$ROOT/$ISH") >"build/ish-$name-n$procs.out" 2>"build/ish-$name-n$procs.err"
        rc=$?
        if [ "$rc" -ne 0 ]; then
            echo "ISH SESSION FAIL: $name (IDIOMMAXPROCS=$procs, status $rc)"; cat "build/ish-$name-n$procs.err"; fail=1
        elif ! cmp -s "tests/ish/$name.out" "build/ish-$name-n$procs.out"; then
            echo "ISH SESSION FAIL: $name (IDIOMMAXPROCS=$procs)"; diff "tests/ish/$name.out" "build/ish-$name-n$procs.out" || true; fail=1
        fi
    done
fi
[ "$MODE" != "san" ] && echo "ish pty goldens passed ($(ls tests/ish/*.keys | wc -l) cases)"

for f in tests/shell/*.ish; do
    name=$(basename "$f" .ish)
    if [ "$MODE" != "san" ]; then
        "$ISH" "$f" >"build/shell-$name.out" 2>"build/shell-$name.err"
        if [ -s "build/shell-$name.err" ]; then
            echo "SHELL STDERR: $name"; cat "build/shell-$name.err"; fail=1
        else
            pin_check "tests/shell/$name.out" "build/shell-$name.out" "SHELL FAIL: $name"
        fi
    else
        "$ISH" "$f" >/dev/null 2>"build/san-shell-$name.err" || true
        san_check "build/san-shell-$name.err" "shell $name"
    fi
done

for f in tests/shell/fail/*.ish; do
    name=$(basename "$f" .ish)
    if [ "$MODE" != "san" ]; then
        "$ISH" "$f" >"build/shellfail-$name.out" 2>"build/shellfail-$name.err"
        rc=$?
        want=$(cat "tests/shell/fail/$name.status")
        status_pin "tests/shell/fail/$name.status" "" "SHELL-FAIL STATUS: $name" "$rc" "$want"
        pin_check "tests/shell/fail/$name.err" "build/shellfail-$name.err" "SHELL-FAIL STDERR: $name"
        pin_check "tests/shell/fail/$name.out" "build/shellfail-$name.out" "SHELL-FAIL OUT: $name"
    else
        "$ISH" "$f" >/dev/null 2>"build/san-shellfail-$name.err" || true
        san_check "build/san-shellfail-$name.err" "shell-fail $name"
    fi
done

if [ "$MODE" != "san" ]; then
    "$IDIOMC" build tests/seal/tool.id -o build/sealed-tool >/dev/null 2>build/seal.err
    if [ -s build/seal.err ]; then
        echo "SEAL BUILD FAIL"; cat build/seal.err; fail=1
    else
        env -u IDIOMROOT -u IDIOMPATH "$PWD/$IDIOMC" build/sealed-tool one two > build/sealed-run.out 2>build/sealed-run.err
        if [ -s build/sealed-run.err ]; then
            echo "SEAL RUN FAIL"; cat build/sealed-run.err; fail=1
        else
            pin_check "tests/seal/tool.out" "build/sealed-run.out" "SEAL RUN FAIL"
            [ "$MODE" = "output" ] && [ "$fail" -eq 0 ] && echo "sealed program passed (1 case)"
        fi
    fi

    "$IDIOMC" repl < tests/repl/session.in > build/repl-session.out 2>build/repl-session.err
    pin_check "tests/repl/session.err" "build/repl-session.err" "REPL FAIL (stderr)"
    pin_check "tests/repl/session.out" "build/repl-session.out" "REPL FAIL (stdout)"
    IDIOMMAXPROCS=1 "$IDIOMC" repl < tests/repl/session.in > build/repl-session-n1.out 2>build/repl-session-n1.err
    if ! cmp -s tests/repl/session.err build/repl-session-n1.err || ! cmp -s tests/repl/session.out build/repl-session-n1.out; then
        echo "REPL FAIL (IDIOMMAXPROCS=1)"; diff tests/repl/session.err build/repl-session-n1.err || true; diff tests/repl/session.out build/repl-session-n1.out || true; fail=1
    elif [ "$MODE" = "output" ]; then
        echo "repl session passed (2 cases)"
    fi

    "$IDIOMC" dump surface > build/dump-surface-default.out 2>/dev/null
    pin_check tests/dump/surface-default.out build/dump-surface-default.out "DUMP FAIL: surface-default"
    ISH_PRELUDE='use app/ish
activate Shell'
    "$IDIOMC" dump surface "$ISH_PRELUDE" > build/dump-surface-shell.out 2>/dev/null
    pin_check tests/dump/surface-shell.out build/dump-surface-shell.out "DUMP FAIL: surface-shell"

    printf 'foo bar\n' | "$IDIOMC" dump reader - >/dev/null || fail=1
    printf '%s\necho hello\n' "$ISH_PRELUDE" | "$IDIOMC" dump core - | grep -q 'prim exec' || { echo "PIN FAIL: exec"; fail=1; }
    printf '%s\nls -la | wc -l\n' "$ISH_PRELUDE" | "$IDIOMC" dump core - | grep -q ':pipeline' || { echo "PIN FAIL: pipeline"; fail=1; }
    printf 'echo hello\n' | "$IDIOMC" dump core - 2>&1 | grep -q "unbound identifier 'echo'" || { echo "PIN FAIL: no-shell"; fail=1; }
    printf '%s\nls *.zz\n' "$ISH_PRELUDE" | "$IDIOMC" dump core - | grep -q ':glob' || { echo "PIN FAIL: glob"; fail=1; }
    printf '%s\nls nl*\n' "$ISH_PRELUDE" | "$IDIOMC" dump core - | grep -q ':glob "nl\*"' || { echo "PIN FAIL: glob-joined"; fail=1; }
    printf '%s\nsh -c x 2>&1\n' "$ISH_PRELUDE" | "$IDIOMC" dump core - | grep -q ':dup' || { echo "PIN FAIL: dup"; fail=1; }
    printf '%s\nFOO=bar printenv FOO\n' "$ISH_PRELUDE" | "$IDIOMC" dump core - | grep -q '"FOO"' || { echo "PIN FAIL: env"; fail=1; }
    printf '%s\necho a$X.txt\n' "$ISH_PRELUDE" | "$IDIOMC" dump core - | grep -q ':cat' || { echo "PIN FAIL: cat"; fail=1; }
    printf '%s\necho hi > out.txt\n' "$ISH_PRELUDE" | "$IDIOMC" dump core - | grep -q ':lit "out.txt"' || { echo "PIN FAIL: redirect-join"; fail=1; }
fi

if [ "$MODE" = "update" ]; then
    if [ "$fail" -ne 0 ]; then
        echo "snapshots: invariant failures above are NOT updatable (fix the test or the code)"
        exit 1
    fi
    echo "snapshots: $updated updated"
    exit 0
fi

if [ "$fail" -ne 0 ]; then
    echo "harness: FAILURES ($MODE)"
    exit 1
fi
echo "harness: all lanes passed ($MODE)"
