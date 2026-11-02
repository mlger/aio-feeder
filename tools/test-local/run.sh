#!/usr/bin/env bash
# Local host-test runner — same sources CI runs with `pio test -d test`.
# Exists because PlatformIO's CDN is blocked in some dev environments;
# this compiles each Unity suite directly with g++ (see unity.h shim).
#
# usage: tools/test-local/run.sh [suite ...]   e.g. run.sh test_e2e_sim
set -euo pipefail
cd "$(dirname "$0")/../.."

OUT=.pio-local
mkdir -p "$OUT"

INC="-Ilib/proto -Inode_core -Ilink_core -Ihub/src -Itools/sim -Itools/test-local"
CXXFLAGS="-std=c++17 -fpermissive -Wall -Wextra -Wno-unused-parameter -O1 -g"

suites=("$@")
if [ ${#suites[@]} -eq 0 ]; then
  suites=(test_proto test_link_core test_node_core test_hub_core test_e2e_sim test_ui_model test_pairing test_calib test_settings test_backup test_wizard test_entry)
fi

rc=0
for s in "${suites[@]}"; do
  src="test/$s/test_main.cpp"
  if [ ! -f "$src" ]; then
    echo "skip $s (no $src)"; continue
  fi
  echo "=== $s"
  g++ $CXXFLAGS $INC "$src" tools/test-local/unity_impl.cpp -o "$OUT/$s"
  if ! "$OUT/$s"; then rc=1; fi
done
exit $rc
