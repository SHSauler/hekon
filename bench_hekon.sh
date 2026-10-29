#!/bin/sh
# bench_hekon.sh - synthetic measurable benchmark for PLAN.md Phase A.
# Generates a deterministic fixture (large media + many small files +
# known duplicates), runs hekon -q --stats, and reports exact counters.
# No real user data needed.
#
# Usage:
#   ./bench_hekon.sh                    # default ~64 MB, fast (<10s)
#   BENCH_MB=1024 ./bench_hekon.sh      # ~1 GB
#   BENCH_MB=10240 ./bench_hekon.sh     # ~10 GB (PLAN.md full fixture)
#   HEKON=./hekon BENCH_DIR=/tmp/hb ./bench_hekon.sh
set -u

HEKON="${HEKON:-./hekon}"
BENCH_MB="${BENCH_MB:-64}"
BENCH_DIR="${BENCH_DIR:-/tmp/hekon-bench.$$}"

[ -x "$HEKON" ] || { echo "FAIL: $HEKON not built (run make first)"; exit 1; }

# Scale fixture to requested MB: split budget 70% large / 20% small / 10% tiny.
# Keep file counts bounded so even 10 GB finishes (fewer, bigger files).
if [ "$BENCH_MB" -ge 1000 ]; then
  N_LARGE=8; LARGE_MB=$(( BENCH_MB * 70 / 100 / N_LARGE ))
  N_SMALL=400; SMALL_KB=512
  N_TINY=400
else
  N_LARGE=4
  LARGE_MB=$(( BENCH_MB * 70 / 100 / N_LARGE ))
  [ "$LARGE_MB" -lt 1 ] && LARGE_MB=1
  N_SMALL=200; SMALL_KB=32
  N_TINY=200
fi

SRC="$BENCH_DIR/source"; TGT="$BENCH_DIR/target"
rm -rf "$BENCH_DIR"; mkdir -p "$SRC" "$TGT"

echo "fixture: target=${BENCH_MB}MB budget large=${N_LARGE}x${LARGE_MB}MB small=${N_SMALL}x${SMALL_KB}KB tiny=${N_TINY}x1KB dir=$BENCH_DIR"

# Deterministic pseudorandom base (fast, incompressible enough): use awk to
# emit varying bytes so same-size files differ where intended.
gen_blob() { # $1=bytes $2=seed $3=outfile
  awk -v n="$1" -v s="$2" 'BEGIN { for (i=0;i<n;i++) printf "%c", (s*7919+i*31+i*i) % 256 }' > "$3"
}

# 1. Large media: half the large files duplicated SRC<->TGT, half unique.
i=0
while [ "$i" -lt "$N_LARGE" ]; do
  gen_blob $(( LARGE_MB * 1024 * 1024 )) "$i" "$TGT/big-$i.bin"
  if [ $(( i % 2 )) -eq 0 ]; then
    cp "$TGT/big-$i.bin" "$SRC/big-$i-copy.bin"   # duplicate candidate
  else
    gen_blob $(( LARGE_MB * 1024 * 1024 )) "$(( i + 1000 ))" "$SRC/big-unique-$i.bin"
  fi
  i=$(( i + 1 ))
done
# same-size different-content pair (must NOT match: exercises partial-hash path)
gen_blob $(( 1024 * 1024 )) 7777 "$SRC/samesize-a.bin"
gen_blob $(( 1024 * 1024 )) 8888 "$SRC/samesize-b.bin"
cp "$SRC/samesize-a.bin" "$TGT/samesize-master.bin"  # one of them has a TARGET twin

# uniquely-sized files: never hashed, prove the size-skip path
gen_blob 12345 4242 "$SRC/odd-src-blob"
gen_blob 6789 4243 "$TGT/odd-tgt-blob"

# 2. Small files (>4KB partial window, exercises full-hash path)
i=0
while [ "$i" -lt "$N_SMALL" ]; do
  gen_blob $(( SMALL_KB * 1024 )) "$i" "$TGT/small-$i.dat"
  if [ $(( i % 2 )) -eq 0 ]; then
    cp "$TGT/small-$i.dat" "$SRC/small-$i-copy.dat"
  else
    gen_blob $(( SMALL_KB * 1024 )) "$(( i + 5000 ))" "$SRC/small-unique-$i.dat"
  fi
  i=$(( i + 1 ))
done

# 3. Tiny files (<=4KB: must hit the single-read reuse path, never full_hash)
i=0
while [ "$i" -lt "$N_TINY" ]; do
  gen_blob 1024 "$i" "$TGT/tiny-$i.dat"
  if [ $(( i % 2 )) -eq 0 ]; then
    cp "$TGT/tiny-$i.dat" "$SRC/tiny-$i-copy.dat"
  else
    gen_blob 1024 "$(( i + 9000 ))" "$SRC/tiny-unique-$i.dat"
  fi
  i=$(( i + 1 ))
done

DU_BYTES="$(du -sb "$BENCH_DIR" | cut -f1)"
echo "fixture_bytes=$DU_BYTES"

# 3 timed runs, keep the stats line of each.
RUN=0
FAIL=0
while [ "$RUN" -lt 3 ]; do
  RUN=$(( RUN + 1 ))
  START="$(date +%s%N)"
  OUT="$("$HEKON" -q --stats "$SRC" "$TGT" 2>"$BENCH_DIR/stats-$RUN.txt")"
  END="$(date +%s%N)"
  WALL_MS=$(( (END - START) / 1000000 ))
  STATS="$(grep '^stats:' "$BENCH_DIR/stats-$RUN.txt" || true)"
  echo "run$RUN wall_ms=$WALL_MS $STATS"
  echo "$OUT" | grep -q "would be deleted" || { echo "FAIL: run$RUN missing summary"; FAIL=1; }
  [ -n "$STATS" ] || { echo "FAIL: run$RUN missing stats: line"; FAIL=1; }
  eval "$(echo "$STATS" | sed 's/^stats: //; s/ \([a-z_]*\)=/; \1=/g')"
  # shellcheck disable=SC2154
  {
    [ "${chunk_size:-0}" = "65536" ] || { echo "FAIL: chunk_size=$chunk_size want 65536"; FAIL=1; };
    [ "${full_reused_small:-0}" -gt 0 ] || { echo "FAIL: full_reused_small=0 (tiny-file reuse path dead)"; FAIL=1; };
    [ "${bytes_saved_small:-0}" -gt 0 ] || { echo "FAIL: bytes_saved_small=0"; FAIL=1; };
    [ "${size_singletons:-0}" -gt 0 ] || { echo "FAIL: size_singletons=0 (size-skip path dead)"; FAIL=1; };
    [ "${partial_singletons:-0}" -gt 0 ] || { echo "FAIL: partial_singletons=0"; FAIL=1; };
    [ "${partial_hashes:-0}" -gt 0 ] || { echo "FAIL: no partial hashes counted"; FAIL=1; };
  }
done

# Correctness spot checks on last run output.
echo "$OUT" | grep -q "big-0-copy" || { echo "FAIL: large dupe not listed"; FAIL=1; }
echo "$OUT" | grep -q "tiny-0-copy" || { echo "FAIL: tiny dupe not listed"; FAIL=1; }
if echo "$OUT" | grep -q "big-unique"; then echo "FAIL: unique large file flagged"; FAIL=1; fi
if echo "$OUT" | grep -q "samesize-b"; then echo "FAIL: same-size different content flagged"; FAIL=1; fi

rm -rf "$BENCH_DIR"
if [ "$FAIL" -eq 0 ]; then echo "BENCH OK (~${BENCH_MB}MB budget, 3 runs, counters asserted)"; else echo "BENCH FAILED"; fi
exit "$FAIL"
