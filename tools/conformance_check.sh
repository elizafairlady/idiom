#!/bin/sh
set -u
doc="docs/CONFORMANCE.md"
fail=0

for fn in $(grep -oE 'test_[a-z0-9_]+' "$doc" | sort -u); do
    if ! grep -q "static void ${fn}(void)" tests/unit/*.c; then
        echo "CONFORMANCE MANIFEST DRIFT: unit test '${fn}' not found in tests/unit/"
        fail=1
    fi
done

for path in $(grep -oE '`tests/[a-z0-9/._-]+`' "$doc" | tr -d '\140' | sort -u); do
    if [ ! -e "$path" ]; then
        echo "CONFORMANCE MANIFEST DRIFT: path '${path}' does not exist"
        fail=1
    fi
done

grep -iE 'golden|diag|dump pin' "$doc" | grep -oE '`[a-z0-9][a-z0-9-]*`' | tr -d '\140' | sort -u | while read -r name; do
    case "$name" in
        test_*) continue ;;
    esac
    if [ ! -e "tests/golden/${name}.ish" ] && [ ! -e "tests/diag/${name}.ish" ] && [ ! -e "tests/dump/${name}.out" ]; then
        echo "CONFORMANCE MANIFEST DRIFT: artifact '${name}' not found in tests/golden, tests/diag, or tests/dump"
        echo drift > build/.conformance_drift
    fi
done
if [ -e build/.conformance_drift ]; then
    rm -f build/.conformance_drift
    fail=1
fi

if [ "$fail" -ne 0 ]; then
    exit 1
fi
echo "conformance manifest verified"
