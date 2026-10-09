#!/bin/sh
# General Championship: add teams and build everything. Run this after dropping zips in GC/submissions
# (or folders in GC/teams). Needs cmake and a C/C++ compiler.
set -e
cd "$(dirname "$0")"
B=build/cmake
cmake -S .. -B "$B" -DCMAKE_BUILD_TYPE=Release -DRR_BUILD_BOTS=OFF
cmake --build "$B" --target rr_gc -j
./rr_gc import || true      # unpack submissions/*.zip into teams/
./rr_gc plan || true        # which bots to compile (teams with problems are left out and listed)
cmake -S .. -B "$B"         # pick up the new bots
cmake --build "$B" --target rr_gc_host -j
echo
./rr_gc check --built || true
./rr_gc smoke || true
echo
echo "Done. Start GC_Race_viewer."
