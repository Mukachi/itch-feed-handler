#!/bin/sh
# Builds and runs the test suite as CI does: the same jobs as lockfree-ipc's
# (its note 009) and the gateway's. Recorded in note 002 of this repository.
#
# Usage: ci/test.sh release|tsan|asan
# Runs from the repository root. No benchmark runs, nothing writes to
# results/, and nothing needs the sample day (data/): checks that do carry the
# CTest label `data`, and checks needing a quiet machine `quiet`; both are left
# out here.
set -eu

kind=$1
build=build-ci-$kind
case $kind in
  release) flags="" type=Release ;;
  tsan)    flags="-fsanitize=thread" type=RelWithDebInfo ;;
  asan)    flags="-fsanitize=address,undefined -fno-sanitize-recover=all" type=RelWithDebInfo ;;
  *) echo "usage: $0 release|tsan|asan" >&2; exit 2 ;;
esac

echo "== $(g++ --version | head -1); $(cmake --version | head -1); $(nproc) CPUs; kernel $(uname -r) $(uname -m)"
cmake -S . -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=$type -DCMAKE_CXX_FLAGS="$flags ${flags:+-fno-omit-frame-pointer}"
cmake --build "$build"
(cd "$build" && ctest -LE "quiet|data" --output-on-failure)
