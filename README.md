# hekon

`hekon` deletes files in `SOURCE` that duplicate files in `TARGET`
(the master, never modified). Derived from `fdupes-master`.

```
hekon SOURCE TARGET        # dry-run: list what would be deleted, delete nothing
hekon -d SOURCE TARGET     # actually delete the SOURCE duplicates
hekon -q SOURCE TARGET     # hide progress indicator
hekon -j8 SOURCE TARGET    # hash with 8 threads (default: online CPUs, max 8)
hekon -q --stats SOURCE TARGET  # + machine-parseable counters/timings on stderr
hekon -h / -v              # help / version
```

Rules:

* `TARGET` is never modified or deleted. Extra copies of the same
  content inside `TARGET` are kept, never candidates.
* Always recurses into subdirectories of both trees.
* Symlinks are not followed; only regular files count (hidden and
  empty files included).
* Hardlinks (same device+inode) are never treated as duplicates.
* Identical files existing only in `SOURCE` are deduplicated too:
  one copy is kept, the rest deleted.
* `SOURCE` and `TARGET` must not be the same directory or overlap
  (one inside the other); `hekon` refuses to run in that case.
* Matching: file size → partial XXH3 → full XXH3 → byte-for-byte
  confirmation (same staged pipeline as fdupes, but with xxHash instead
  of MD5). Unique-size files are never
  hashed; hashing runs on a worker pool (`-j/--jobs`).

## Build / install

Requires only a C compiler:

```
make
sudo make install        # PREFIX defaults to /usr/local
make test                # build + run functional tests
make bench               # synthetic benchmark (~64 MB default, BENCH_MB=10240 for ~10 GB)
```

`--stats` prints one `stats:` line to stderr with exact counters, e.g.:

```
stats: files=811 partial_hashes=811 full_hashes=350 full_reused_small=360 \
  bytes_partial=2297856 bytes_full=89260032 bytes_saved_small=552960 \
  confirms=355 scan_ms=2 match_ms=139 total_ms=141 chunk_size=65536 partial_size=4096
```

`full_reused_small` / `bytes_saved_small` measure the small-file
single-read win directly: files ≤ 4 KB whose full digest is copied from the
partial digest instead of re-read. `bench_hekon.sh` asserts these are
non-zero on its fixture, so `make bench` fails if the optimization
regresses.

## Hash cache (`--cache`)

Repeat runs on an unchanged tree skip hashing: digests are keyed by
`(device, inode, size, mtime, ctime)` and stored in one flat file at
`$XDG_CACHE_HOME/hekon/hashes` (or `~/.cache/hekon/hashes`). Any
metadata change invalidates the entry; files that vanished fall out on
the next save. The byte-for-byte confirmation still runs every time, so
a stale hit can never cause a wrong delete.

```
hekon --cache -q SRC TGT          # first run populates, later runs reuse
hekon --cache --stats -q SRC TGT  # warm run: partial_hashes=0 full_hashes=0,
                                  # cache_hits_partial/full > 0
hekon --cache-clear               # delete the cache file and exit
```

`--cache` is off by default; plain runs never read or write the cache.

## Example

```
$ hekon ~/downloads ~/photos
[MASTER] /home/user/photos/cat.jpg
  [DELETE] /home/user/downloads/cat.jpg

[KEEP] /home/user/downloads/notes.txt
  [DELETE] /home/user/downloads/notes-copy.txt

2 files (1.0 MB) would be deleted.
Run with -d/--delete to delete them. TARGET files are never modified.

$ hekon -d ~/downloads ~/photos
Deleted /home/user/downloads/cat.jpg
Deleted /home/user/downloads/notes-copy.txt

Deleted 2 files (1.0 MB).
```

## Origin

Fork-and-trim of `fdupes-master` from the fdupes family
((c) 1999–2022 Adrian Lopez and contributors, MIT). Byte confirmation
(`confirmmatch`), safe delete (`removeifnotchanged`), signal and error
helpers are reused verbatim apart from dropping the autoconf `config.h`
dependency; the MD5 staging hashes were replaced with vendored xxHash
(`xxhash.h`, BSD-2-Clause, (c) Yann Collet). Interactive
prompts, size/hidden filters, logging, ncurses and sqlite support
were removed for simplicity.
