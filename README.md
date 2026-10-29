# hekon

`hekon` deletes files in `SOURCE` that duplicate files in `TARGET`
(the master, never modified). Derived from `fdupes-master`.

```
hekon SOURCE TARGET        # dry-run: list what would be deleted, delete nothing
hekon -d SOURCE TARGET     # actually delete the SOURCE duplicates
hekon -q SOURCE TARGET     # hide progress indicator
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
* Matching: file size → partial MD5 → full MD5 → byte-for-byte
  confirmation (same pipeline as fdupes).

## Build / install

Requires only a C compiler:

```
make
sudo make install        # PREFIX defaults to /usr/local
make test                # build + run functional tests
```

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
((c) 1999–2022 Adrian Lopez and contributors, MIT). The hashing
(`md5/`), byte confirmation (`confirmmatch`), safe delete
(`removeifnotchanged`), signal and error helpers are reused verbatim
apart from dropping the autoconf `config.h` dependency. Interactive
prompts, size/hidden filters, logging, ncurses and sqlite support
were removed for simplicity.
