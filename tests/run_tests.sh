#!/bin/sh
#
# Run every language test and diff its output against the expected file.
#
# usage: sh tests/run_tests.sh [path-to-flint]
#
# A test is a pair: tests/language/**/name.fl and name.expected. The expected
# file holds stdout, compared byte for byte. stderr is folded in, so a test
# that is supposed to error has the error message in its .expected file.
set -e

FLINT="${1:-./flint}"

passed=0
failed=0

for test_file in $(find tests/language -type f -name '*.fl' | sort); do
    [ -f "$test_file" ] || continue
    base="${test_file%.fl}"
    expected="$base.expected"

    # a .fl with no .expected is a scratch file, not a test
    [ -f "$expected" ] || continue

    # `|| true` because a test may exit non-zero on purpose
    out=$("$FLINT" "$test_file" 2>&1 || true)
    expected_out=$(cat "$expected")

    if [ "$out" = "$expected_out" ]; then
        passed=$((passed + 1))
    else
        echo "FAIL: $test_file"
        echo "Expected:"
        echo "$expected_out"
        echo "Got:"
        echo "$out"
        failed=$((failed + 1))
    fi
done

echo "Tests: $((passed + failed)), Passed: $passed, Failed: $failed"

# a final test failure means a non-zero exit, which is what CI reads.
# written as an if because `test && exit` under `set -e` exits even when the
# test fails, which is the opposite of what it looks like.
if [ "$failed" -gt 0 ]; then
    exit 1
fi
exit 0
