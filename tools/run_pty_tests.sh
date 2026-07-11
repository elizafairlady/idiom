#!/bin/sh
set -e
driver="$1"
idiomc="$2"
PATH="$(cd "$(dirname "$idiomc")" && pwd):$PATH"
export PATH
status=0
for keys in tests/pty/*.keys; do
    name=$(basename "$keys" .keys)
    unset IEM_ISH IEM_INIT
    case "$name" in
        tty_line_winch) prog="run tests/pty/tty_line_winch.id" ;;
        sigint_batch) prog="run tests/pty/sigint_batch.id" ;;
        ish_*) prog="run app/ish" ;;
        iem_scratch) prog="run app/iem/main" ;;
        iem_meta) prog="run app/iem/main" ;;
        iem_evil)
            export IEM_INIT=examples/iem-init.id
            prog="run app/iem/main"
            ;;
        iem_ish)
            export IEM_ISH=build/ish
            export IEM_INIT=examples/iem-init.id
            prog="run app/iem/main"
            ;;
        iem_*)
            rm -f /tmp/iem_pty_scratch.txt
            prog="run app/iem/main /tmp/iem_pty_scratch.txt"
            ;;
        *) prog="repl" ;;
    esac
    if "$driver" "$keys" "$idiomc" $prog > /dev/null 2>&1; then
        echo "pass pty/$name"
    else
        echo "FAIL pty/$name"
        status=1
    fi
done
exit $status
