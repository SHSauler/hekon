#!/bin/sh
# Functional tests for hekon. Builds ./hekon, exercises dry-run and -d
# against fixture trees, and verifies TARGET is never modified.
set -u

HEKON="${HEKON:-./hekon}"
FAIL=0

pass() { echo "ok: $1"; }
fail() { echo "FAIL: $1"; FAIL=1; }

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

TGT_BEFORE="$(find "$TGT" -type f | sort | xargs md5sum)"

# --- dry-run deletes nothing ---
OUT="$("$HEKON" -q "$SRC" "$TGT")"
echo "$OUT" | grep -q "a.jpg" || fail "dry-run lists SRC/a.jpg"
echo "$OUT" | grep -q "notes-copy.txt" || fail "dry-run lists source-only dup"
echo "$OUT" | grep -q "would be deleted" || fail "dry-run summary line"
[ -f "$SRC/a.jpg" ] && [ -f "$SRC/notes-copy.txt" ] || fail "dry-run deleted files"
[ -L "$SRC/link-to-master" ] || fail "dry-run removed symlink"
# symlink target must not appear as a duplicate entry of its own
echo "$OUT" | grep -q "link-to-master" && fail "symlink followed as duplicate"
TGT_AFTER="$(find "$TGT" -type f | sort | xargs md5sum)"
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
[ -f "$TGT/keep.jpg" ] && [ -f "$TGT/sub/deep" ] && [ -f "$TGT/empty" ] || fail "TARGET file missing"
TGT_AFTER2="$(find "$TGT" -type f | sort | xargs md5sum)"
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

if [ "$FAIL" -eq 0 ]; then echo "ALL TESTS PASSED"; else echo "TESTS FAILED"; fi
exit "$FAIL"
