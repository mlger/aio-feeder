#!/usr/bin/env bash
#
# AIO Feeder — host unit tests without PlatformIO (PLAN.md §11.1).
#
# The PIO registry CDN is unreachable from this bench (see AGENTS.md), so this
# script compiles the same test TUs the CI `pio test -d test` job runs, with
# the same include set, using g++ + a vendored Unity. CI keeps using PIO; this
# is the desk fallback.
#
# Usage: tools/hosttest.sh [test_dir_name ...]   (default: all)
#
set -u
cd "$(dirname "$0")/.."

UNITY=tools/unity
if [ ! -f "$UNITY/src/unity.c" ]; then
  echo "== fetching Unity v2.6.1 (gitignored) =="
  rm -rf "$UNITY"
  git clone --depth 1 --branch v2.6.1 \
    https://github.com/ThrowTheSwitch/Unity "$UNITY" || exit 1
fi

INC="-Ilib/proto -Inode_core -Ilink_core -Ihub/src -Itools/sim -I$UNITY/src"
OUT=build/hosttest
mkdir -p "$OUT"

dirs=("$@")
if [ ${#dirs[@]} -eq 0 ]; then
  dirs=(test/test_*/)
  dirs=("${dirs[@]%/}")
else
  # accept bare names (test_node_core) or paths (test/test_node_core)
  dirs=("${dirs[@]/test\//}")
  dirs=("${dirs[@]/#/test/}")
fi

pass=0; fail=0; failed=()
for d in "${dirs[@]}"; do
  name=$(basename "$d")
  bin="$OUT/$name"
  if g++ -std=c++17 $INC -o "$bin" "$d/test_main.cpp" "$UNITY/src/unity.c" 2> "$OUT/$name.err"; then
    if "$bin" > "$OUT/$name.out" 2>&1; then
      n=$(grep -o '[0-9]* Tests' "$OUT/$name.out" | head -1)
      echo "PASS $name ($n)"
      pass=$((pass+1))
    else
      echo "FAIL $name (run) — see $OUT/$name.out"
      tail -12 "$OUT/$name.out"
      fail=$((fail+1)); failed+=("$name")
    fi
  else
    echo "FAIL $name (build) — see $OUT/$name.err"
    tail -12 "$OUT/$name.err"
    fail=$((fail+1)); failed+=("$name")
  fi
done

echo "----------------------------------------"
msg="$pass passed, $fail failed"
if [ ${#failed[@]} -gt 0 ]; then msg="$msg (${failed[*]})"; fi
echo "$msg"
[ "$fail" -eq 0 ]
