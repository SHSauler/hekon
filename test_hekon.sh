#!/bin/sh
# Functional tests for hekon. Builds ./hekon, exercises dry-run and -d
# against fixture trees, and verifies TARGET is never modified.
set -u

HEKON="${HEKON:-./hekon}"
FAIL=0

pass() { echo "ok: $1"; }
fail() { echo "FAIL: $1"; FAIL=1; }

# portable per-file checksums (md5sum on Linux, md5 -r on macOS)
fingerprint() { # $1 = dir
  if command -v md5sum >/dev/null 2>&1; then
    find "$1" -type f | sort | xargs md5sum
  elif command -v md5 >/dev/null 2>&1; then
    find "$1" -type f | sort | xargs md5 -r
  else
    find "$1" -type f | sort | xargs cksum
  fi
}

[ -x "$HEKON" ] || { echo "FAIL: $HEKON not built (run make first)"; exit 1; }

ROOT="$(mktemp -d /tmp/hekon-test.XXXXXX)"
trap 'rm -rf "$ROOT"' EXIT INT TERM
SRC="$ROOT/source"
TGT="$ROOT/target"
mkdir -p "$SRC/sub" "$TGT/sub"

# Fixtures:
# 1. master-copy dup: TGT/keep.jpg == SRC/a.jpg == SRC/sub/b.jpg
# 2. source-only dup: SRC/notes.txt == SRC/notes-copy.txt (no TARGET copy)
# 3. unique files: SRC/unique.txt, TGT/other.txt
# 4. empty files: TGT/empty == SRC/empty-dup (both zero bytes)
# 5. same size, different content: SRC/same1 (aaa) vs SRC/same2 (bbb)
# 6. symlink in SOURCE pointing at a TARGET file (must be ignored)
# 7. nested recursion: TGT/sub/deep == SRC/sub/deep-copy
printf 'photo-content-123' > "$TGT/keep.jpg"
cp "$TGT/keep.jpg" "$SRC/a.jpg"
cp "$TGT/keep.jpg" "$SRC/sub/b.jpg"
printf 'note-note-note' > "$SRC/notes.txt"
cp "$SRC/notes.txt" "$SRC/notes-copy.txt"
printf 'unique-source' > "$SRC/unique.txt"
printf 'unique-target' > "$TGT/other.txt"
: > "$TGT/empty"
: > "$SRC/empty-dup"
printf 'aaa' > "$SRC/same1"
printf 'bbb' > "$SRC/same2"
printf 'deep-content' > "$TGT/sub/deep"
cp "$TGT/sub/deep" "$SRC/sub/deep-copy"
ln -s "$TGT/keep.jpg" "$SRC/link-to-master"
# hardlink in SOURCE pointing at a TARGET file (must be ignored, never a dupe)
ln "$TGT/keep.jpg" "$SRC/hard-to-master"

TGT_BEFORE="$(fingerprint "$TGT")"

# --- dry-run deletes nothing ---
OUT="$("$HEKON" -q "$SRC" "$TGT")"
echo "$OUT" | grep -q "a.jpg" || fail "dry-run lists SRC/a.jpg"
echo "$OUT" | grep -q "notes-copy.txt" || fail "dry-run lists source-only dup"
echo "$OUT" | grep -q "would be deleted" || fail "dry-run summary line"
[ -f "$SRC/a.jpg" ] && [ -f "$SRC/notes-copy.txt" ] || fail "dry-run deleted files"
[ -L "$SRC/link-to-master" ] || fail "dry-run removed symlink"
# symlink target must not appear as a duplicate entry of its own
echo "$OUT" | grep -q "link-to-master" && fail "symlink followed as duplicate"
echo "$OUT" | grep -q "hard-to-master" && fail "hardlink followed as duplicate"
TGT_AFTER="$(fingerprint "$TGT")"
[ "$TGT_BEFORE" = "$TGT_AFTER" ] || fail "dry-run modified TARGET"
pass "dry-run lists candidates and deletes nothing"

# --- same-size different-content files are not flagged ---
echo "$OUT" | grep -q "same1" && fail "same-size different content flagged"
echo "$OUT" | grep -q "same2" && fail "same-size different content flagged"
echo "$OUT" | grep -q "unique" && fail "unique file flagged"
pass "non-duplicates not flagged"

# --- real delete ---
"$HEKON" -q -d "$SRC" "$TGT" > "$ROOT/del.out" || fail "delete run exit code"
[ ! -e "$SRC/a.jpg" ] || fail "a.jpg not deleted"
[ ! -e "$SRC/sub/b.jpg" ] || fail "sub/b.jpg not deleted"
[ ! -e "$SRC/sub/deep-copy" ] || fail "deep-copy not deleted"
[ ! -e "$SRC/empty-dup" ] || fail "empty-dup not deleted"
# source-only set: exactly one of the pair must survive
SURVIVE=0
[ -f "$SRC/notes.txt" ] && SURVIVE=$((SURVIVE+1))
[ -f "$SRC/notes-copy.txt" ] && SURVIVE=$((SURVIVE+1))
[ "$SURVIVE" -eq 1 ] || fail "source-only set kept $SURVIVE copies, want 1"
[ -f "$SRC/unique.txt" ] || fail "unique source file deleted"
[ -f "$SRC/same1" ] && [ -f "$SRC/same2" ] || fail "same-size files touched"
[ -L "$SRC/link-to-master" ] || fail "symlink deleted"
[ -e "$SRC/hard-to-master" ] || fail "hardlink deleted"
[ -f "$TGT/keep.jpg" ] && [ -f "$TGT/sub/deep" ] && [ -f "$TGT/empty" ] || fail "TARGET file missing"
TGT_AFTER2="$(fingerprint "$TGT")"
[ "$TGT_BEFORE" = "$TGT_AFTER2" ] || fail "delete run modified TARGET"
grep -q "Deleted" "$ROOT/del.out" || fail "delete run output missing 'Deleted'"
pass "delete removes SOURCE dupes, keeps TARGET and one source-only copy"

# --- second run finds nothing ---
OUT2="$("$HEKON" -q "$SRC" "$TGT")"
echo "$OUT2" | grep -q "No duplicates found" || fail "second run not clean"
pass "idempotent: second run clean"

# --- overlapping trees refused ---
"$HEKON" -q "$SRC" "$SRC" >/dev/null 2>&1 && fail "same dir accepted" || pass "same dir refused"
"$HEKON" -q "$ROOT" "$TGT" >/dev/null 2>&1 && fail "nested TARGET accepted" || pass "nested trees refused"

# --- bad args ---
"$HEKON" -q "$SRC" >/dev/null 2>&1 && fail "single arg accepted" || pass "arg count validated"
"$HEKON" -q "$SRC/nonexistent" "$TGT" >/dev/null 2>&1 && fail "bad dir accepted" || pass "bad dir rejected"

# --- --stats is machine-parseable and proves the Phase A wins ---
# (fresh fixture: the main one was already deduplicated above)
SSTAT="$ROOT/stats-src"; TSTAT="$ROOT/stats-tgt"
mkdir -p "$SSTAT" "$TSTAT"
printf 'photo-content-123' > "$TSTAT/keep.jpg"
cp "$TSTAT/keep.jpg" "$SSTAT/a.jpg"
printf 'x' > "$TSTAT/tiny"
cp "$TSTAT/tiny" "$SSTAT/tiny-copy"
STAT_OUT="$("$HEKON" -q --stats "$SSTAT" "$TSTAT" 2>&1)"
echo "$STAT_OUT" | grep -q "^stats:" || fail "--stats line missing"
echo "$STAT_OUT" | grep -q "chunk_size=65536" || fail "--stats chunk_size != 65536"
# all fixture files are <=4KB: any dupe comparison reaching the full-hash
# stage must reuse the partial digest instead of re-reading the file
echo "$STAT_OUT" | grep -q "full_reused_small=[1-9]" || fail "--stats full_reused_small==0 (single-read path dead)"
echo "$STAT_OUT" | grep -q "bytes_saved_small=[1-9]" || fail "--stats bytes_saved_small==0"
echo "$STAT_OUT" | grep -q "partial_hashes=[1-9]" || fail "--stats partial_hashes==0"
pass "--stats counters present and single-read reuse observed"

# --- -j equivalence: identical results at any thread count ---
OUT_DEF_J="$("$HEKON" -q "$SSTAT" "$TSTAT")"
[ "$("$HEKON" -q -j1 "$SSTAT" "$TSTAT")" = "$OUT_DEF_J" ] || fail "-j1 output differs from default"
[ "$("$HEKON" -q -j8 "$SSTAT" "$TSTAT")" = "$OUT_DEF_J" ] || fail "-j8 output differs from default"
"$HEKON" -q --jobs 2 --stats "$SSTAT" "$TSTAT" 2>&1 | grep -q "jobs=2" || fail "jobs=2 not in stats"
"$HEKON" -q -j0 "$SSTAT" "$TSTAT" >/dev/null 2>&1 && fail "-j0 accepted" || pass "-j values validated"
"$HEKON" -q -jabc "$SSTAT" "$TSTAT" >/dev/null 2>&1 && fail "-jabc accepted" || true
pass "identical results at -j1/-j8/default"

# --- --cache: second run on an unchanged tree skips hashing ---
export XDG_CACHE_HOME="$ROOT/xdg-cache"
"$HEKON" -q --cache --stats "$SSTAT" "$TSTAT" >/dev/null 2>&1  # populate
CACHE2="$("$HEKON" -q --cache --stats "$SSTAT" "$TSTAT" 2>&1)"
echo "$CACHE2" | grep -q "cache_hits_partial=[1-9]" || fail "warm cache run has no hits"
echo "$CACHE2" | grep -q "partial_hashes=0" || fail "warm cache run still hashed partials"
echo "$CACHE2" | grep -q "full_hashes=0" || fail "warm cache run still hashed fulls"
# staleness: any mtime change invalidates the entry
touch "$SSTAT/a.jpg"
CACHE3="$("$HEKON" -q --cache --stats "$SSTAT" "$TSTAT" 2>&1)"
echo "$CACHE3" | grep -q "partial_hashes=[1-9]" || fail "mtime change did not invalidate entry"
# --cache-clear deletes the cache; next run hashes everything again
"$HEKON" --cache-clear >/dev/null 2>&1
[ ! -f "$ROOT/xdg-cache/hekon/hashes" ] || fail "cache file survives --cache-clear"
CACHE4="$("$HEKON" -q --cache --stats "$SSTAT" "$TSTAT" 2>&1)"
echo "$CACHE4" | grep -q "cache_hits_partial=0" || fail "hits served after clear"
pass "cache skips rehashing, invalidates on mtime, clears on demand"
# plain runs never touch the cache
"$HEKON" --cache-clear >/dev/null 2>&1
"$HEKON" -q "$SSTAT" "$TSTAT" >/dev/null 2>&1
[ ! -f "$ROOT/xdg-cache/hekon/hashes" ] || fail "plain run created cache"
pass "plain runs stay cache-free"

# --- staging hash: same 4KB prefix but different tails must not match ---
BIGT="$ROOT/bigt"; BIGS="$ROOT/bigs"
mkdir -p "$BIGT" "$BIGS"
(dd if=/dev/zero bs=4096 count=1 2>/dev/null; echo TAIL-A; dd if=/dev/zero bs=1000 count=100 2>/dev/null) > "$BIGT/p1"
(dd if=/dev/zero bs=4096 count=1 2>/dev/null; echo TAIL-B; dd if=/dev/zero bs=1000 count=100 2>/dev/null) > "$BIGS/p2"
cp "$BIGT/p1" "$BIGS/p1-copy"
BIGOUT="$("$HEKON" -q "$BIGS" "$BIGT")"
echo "$BIGOUT" | grep -q "p1-copy" || fail "identical large file not flagged"
echo "$BIGOUT" | grep -q "p2" && fail "same-prefix different tail flagged as dupe"
pass "full-hash staging distinguishes same-prefix files"

# --- long options work ---
"$HEKON" --quiet "$SRC" "$TGT" >/dev/null 2>&1 || fail "--quiet long option broken"
pass "long options accepted"

if [ "$FAIL" -eq 0 ]; then echo "ALL TESTS PASSED"; else echo "TESTS FAILED"; fi
exit "$FAIL"
