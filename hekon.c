/* hekon - delete SOURCE files that duplicate files in a master TARGET.
   "Hekon" from Greek "hekon" (willingly / she who works her will).

   Usage: hekon SOURCE TARGET        (dry-run: list, delete nothing)
          hekon -d SOURCE TARGET     (actually delete SOURCE duplicates)

   Derived from fdupes-master (fdupes family, (c) 1999-2022 Adrian Lopez,
   plus fdupes-master authors 2024). Matching pipeline reused verbatim:
   file size -> partial MD5 -> full MD5 -> byte-for-byte confirmation.

   Rules:
   * TARGET (master) is never modified or deleted.
   * Always recurses into subdirectories of SOURCE and TARGET.
   * Symlinks are not followed; only regular files are considered.
   * Same inode+device (hardlinks) are never treated as duplicates.
   * A duplicate set containing a TARGET file: every SOURCE copy is a
     deletion candidate. Extra TARGET copies of the same content are kept.
   * A duplicate set existing only in SOURCE: one copy is kept, the rest
     are deletion candidates.

   Permission is hereby granted, free of charge, to any person obtaining a
   copy of this software and associated documentation files (the
   "Software"), to deal in the Software without restriction, including
   without limitation the rights to use, copy, modify, merge, publish,
   distribute, sublicense, and/or sell copies of the Software, and to
   permit persons to whom the Software is furnished to do so, subject to
   the following conditions:

   The above copyright notice and this permission notice shall be
   included in all copies or substantial portions of the Software.

   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
   EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
   MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
   NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
   BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
   ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
   CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
   SOFTWARE. */

#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <stdint.h>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <locale.h>
#include <pthread.h>

#include "hekon.h"
#ifdef HAVE_GETOPT_H
#include <getopt.h>
#endif

#include "file.h"
#include "confirmmatch.h"
#include "errormsg.h"
#include "sigint.h"
#include "removeifnotchanged.h"

char *program_name;

static int opt_delete = 0;
static int opt_quiet = 0;
static int opt_stats = 0;
static int opt_jobs = 0; /* 0 = auto (online CPUs, capped) */
static int opt_cache = 0;
static int opt_cache_clear = 0;

/* Measurable counters for Phase A (and later phases). All increments for
   hashing happen on the paths that actually read file bytes, so
   --stats proves the small-file single-read win (full_reused) instead of
   asserting it. Timings are monotonic wall-clock milliseconds. */
static struct {
  long long partial_hashes;
  long long full_hashes;
  long long full_reused_small;
  long long bytes_partial;
  long long bytes_full;
  long long bytes_saved_small;
  long long confirms;
  long long size_singletons;    /* Phase B: files skipped, unique size */
  long long partial_singletons; /* Phase B: files skipped, unique partial */
  long long cache_hits_partial; /* Phase C: partial digests served from cache */
  long long cache_hits_full;    /* Phase C: full digests served from cache */
  int jobs_used;                /* Phase B: worker threads in the pool */
  uint64_t scan_ms;
  uint64_t match_ms;
  uint64_t total_ms;
} g_stats;

static int num_threads(void)
{
  long n;
  if (opt_jobs > 0)
    return opt_jobs;
  n = sysconf(_SC_NPROCESSORS_ONLN);
  if (n < 1)
    n = 1;
  if (n > AUTO_JOBS_MAX)
    n = AUTO_JOBS_MAX;
  return (int)n;
}

typedef struct {
  file_t file;        /* must remain the first member so file_t* casts are valid */
  int is_master;      /* 1 = inside TARGET (protected), 0 = inside SOURCE */
} mfile_t;

#define MFILE(f) ((mfile_t*)(void*)(f))

#define MD5_DIGEST_LENGTH 16

/* ---------------------------------------------------------------- current time */

uint64_t now64(void)
{
    struct timespec timecheck;
    clock_gettime(CLOCK_MONOTONIC, &timecheck);
    return (uint64_t)timecheck.tv_sec * 1000 + (uint64_t)timecheck.tv_nsec / (1000 * 1000);
}

/* ---------------------------------------------------------------- file stats */

void getfilestats(file_t *file, struct stat *info)
{
  file->size = info->st_size;
  file->inode = info->st_ino;
  file->device = info->st_dev;
  file->ctime = info->st_ctime;
  file->mtime = info->st_mtime;
#ifdef HAVE_NSEC_TIMES
  file->ctime_nsec = info->st_ctim.tv_nsec;
  file->mtime_nsec = info->st_mtim.tv_nsec;
#else
  file->ctime_nsec = 0;
  file->mtime_nsec = 0;
#endif
}

/* ---------------------------------------------------------------- scanning */

int scan_directory(char *dir, file_t **filelistp, int is_master)
{
  DIR *cd;
  mfile_t *newfile;
  struct dirent *dirinfo;
  int lastchar;
  int filecount = 0;
  struct stat info;
  struct stat linfo;
  static int progress = 0;
  static char indicator[] = "-\\|/";
  static uint64_t last_progress = 0;
  static uint64_t now = 0;

  cd = opendir(dir);

  if (!cd) {
    errormsg("could not open directory %s\n", dir);
    return 0;
  }

  while ((dirinfo = readdir(cd)) != NULL) {
    if (got_sigint) {
      closedir(cd);
      printf("\n");
      exit(0);
    }

    if (strcmp(dirinfo->d_name, ".") && strcmp(dirinfo->d_name, "..")) {
      if (!opt_quiet) {
        now = now64();
        if ( now - last_progress > FDUPES_PROGRESS_REFRESH_MS ) {
          fprintf(stderr, "\rBuilding file list %c ", indicator[progress % 4]);
          last_progress = now;
          progress++;
        }
      }

      newfile = (mfile_t*) malloc(sizeof(mfile_t));
      if (!newfile) {
        errormsg("out of memory!\n");
        closedir(cd);
        exit(1);
      }

      newfile->file.next = *filelistp;
      newfile->file.device = 0;
      newfile->file.inode = 0;
      newfile->file.crcsignature = NULL;
      newfile->file.crcpartial = NULL;
      newfile->file.duplicates = NULL;
      newfile->file.hasdupes = 0;
      newfile->is_master = is_master;

      newfile->file.d_name = (char*) malloc(strlen(dir) + strlen(dirinfo->d_name) + 2);
      if (!newfile->file.d_name) {
        errormsg("out of memory!\n");
        free(newfile);
        closedir(cd);
        exit(1);
      }

      strcpy(newfile->file.d_name, dir);
      lastchar = strlen(dir) - 1;
      if (lastchar >= 0 && dir[lastchar] != '/')
        strcat(newfile->file.d_name, "/");
      strcat(newfile->file.d_name, dirinfo->d_name);

      if (stat(newfile->file.d_name, &info) == -1) {
        free(newfile->file.d_name);
        free(newfile);
        continue;
      }

      if (lstat(newfile->file.d_name, &linfo) == -1) {
        free(newfile->file.d_name);
        free(newfile);
        continue;
      }

      if (S_ISDIR(info.st_mode)) {
        /* always recurse; do not follow symlinks */
        if (!S_ISLNK(linfo.st_mode)) {
          filecount += scan_directory(newfile->file.d_name, filelistp, is_master);
        }
        free(newfile->file.d_name);
        free(newfile);
      } else {
        /* regular files only; symlinks to files are not followed */
        if (S_ISREG(linfo.st_mode)) {
          getfilestats(&newfile->file, &info);
          *filelistp = &newfile->file;
          filecount++;
        } else {
          free(newfile->file.d_name);
          free(newfile);
        }
      }
    }
  }

  closedir(cd);
  return filecount;
}

/* ---------------------------------------------------------------- md5 helpers */

md5_byte_t *getcrcsignatureuntil(char *filename, off_t fsize, off_t max_read)
{
  off_t toread;
  md5_state_t state;
  md5_byte_t *digest;
  md5_byte_t *chunk; /* per-call buffer: the old static buffer was not
                        thread-safe once hashing moved to a worker pool */
  FILE *file;

  digest = (md5_byte_t*) malloc(MD5_DIGEST_LENGTH * sizeof(md5_byte_t));
  if (digest == NULL) {
    errormsg("out of memory\n");
    exit(1);
  }

  chunk = (md5_byte_t*) malloc(CHUNK_SIZE);
  if (chunk == NULL) {
    errormsg("out of memory\n");
    free(digest);
    exit(1);
  }

  md5_init(&state);

  if (max_read != 0 && fsize > max_read)
    fsize = max_read;

  file = fopen(filename, "rb");
  if (file == NULL) {
    errormsg("error opening file %s\n", filename);
    free(chunk);
    free(digest);
    return NULL;
  }

  /* Sequential readahead hint; best-effort, ignore failures and platforms
     without posix_fadvise. fileno() needs stdio.h (already included). */
  {
    int fd = fileno(file);
    if (fd != -1) {
#ifdef POSIX_FADV_SEQUENTIAL
      posix_fadvise(fd, 0, 0, POSIX_FADV_SEQUENTIAL);
#endif
    }
  }

  while (fsize > 0) {
    if (got_sigint) {
      fclose(file);
      free(chunk);
      printf("\n");
      exit(0);
    }

    toread = (fsize >= CHUNK_SIZE) ? CHUNK_SIZE : fsize;
    if (fread(chunk, toread, 1, file) != 1) {
      errormsg("error reading from file %s\n", filename);
      fclose(file);
      free(chunk);
      free(digest);
      return NULL;
    }
    md5_append(&state, chunk, toread);
    fsize -= toread;
  }

  md5_finish(&state, digest);
  fclose(file);
  free(chunk);
  return digest;
}

/* Hash one file without touching g_stats (workers accumulate into the
   pool struct instead; the main thread folds the totals in after join).
   Returns 1 on success, 0 when the file could not be read (digests stay
   NULL and the caller skips the file, as the old tree walk did). */
static int hash_one(file_t *file, int use_full, long long *bytes_used)
{
  md5_byte_t *d;
  if (use_full) {
    d = getcrcsignatureuntil(file->d_name, file->size, 0);
    if (d == NULL)
      return 0;
    file->crcsignature = d;
    *bytes_used = (long long)file->size;
  } else {
    d = getcrcsignatureuntil(file->d_name, file->size, PARTIAL_MD5_SIZE);
    if (d == NULL)
      return 0;
    file->crcpartial = d;
    *bytes_used = file->size < PARTIAL_MD5_SIZE ?
      (long long)file->size : (long long)PARTIAL_MD5_SIZE;
  }
  return 1;
}

/* Small-file fast path (Phase A.3): files with size <= PARTIAL_MD5_SIZE
   are read once. partial == full by definition, so copy the 16-byte
   digest instead of re-reading the file. Returns 1 when reused. */
int reuse_partial_as_full(file_t *file)
{
  if (file->size > PARTIAL_MD5_SIZE || file->crcpartial == NULL)
    return 0;
  file->crcsignature = (md5_byte_t*) malloc(MD5_DIGEST_LENGTH * sizeof(md5_byte_t));
  if (file->crcsignature == NULL) {
    errormsg("out of memory\n");
    exit(1);
  }
  memcpy(file->crcsignature, file->crcpartial, MD5_DIGEST_LENGTH);
  g_stats.full_reused_small++;
  g_stats.bytes_saved_small += (long long)file->size;
  return 1;
}

int md5cmp(const md5_byte_t *a, const md5_byte_t *b)
{
  int x;
  for (x = 0; x < MD5_DIGEST_LENGTH; ++x) {
    if (a[x] < b[x])
      return -1;
    else if (a[x] > b[x])
      return 1;
  }
  return 0;
}

/* ------------------------------------------------------- persistent hash cache
   Phase C, Option 1: flat file, no dependencies. Records are keyed by
   (device, inode, size, mtime, mtime_nsec, ctime, ctime_nsec) so any
   change invalidates the entry; the stored path is not trusted.
   Entries carry a hash-alg tag so a future algorithm swap (Phase D)
   ignores stale records instead of misreading them.
   Pruning is lazy: save rewrites only entries still referenced by a
   file seen during the scan, so vanished files fall out on their own. */

#define CACHE_MAGIC "HEKONC1"
#define CACHE_MAGIC_LEN 8 /* includes trailing NUL */
#define CACHE_VERSION 1u
#define CACHE_ALG_MD5 1u
#define CACHE_BUCKETS 4096
#define CACHE_FILENAME "hashes"

typedef struct {
  uint64_t dev;
  uint64_t ino;
  int64_t size;
  int64_t mtime;
  int64_t mtime_nsec;
  int64_t ctime;
  int64_t ctime_nsec;
} cachekey_t;

typedef struct cacheent {
  cachekey_t key;
  unsigned char alg;
  int has_partial;
  int has_full;
  int live; /* referenced this run: only live entries are saved */
  md5_byte_t partial[MD5_DIGEST_LENGTH];
  md5_byte_t full[MD5_DIGEST_LENGTH];
  struct cacheent *next;
} cacheent_t;

static cacheent_t *cache_tab[CACHE_BUCKETS];
static int cache_have = 0; /* table populated from disk */
static int cache_dirty = 0; /* new entries since load: worth saving */
static char *cache_path = NULL;

static void cache_keyof(file_t *f, cachekey_t *k)
{
  k->dev = (uint64_t)f->device;
  k->ino = (uint64_t)f->inode;
  k->size = (int64_t)f->size;
  k->mtime = (int64_t)f->mtime;
  k->mtime_nsec = (int64_t)f->mtime_nsec;
  k->ctime = (int64_t)f->ctime;
  k->ctime_nsec = (int64_t)f->ctime_nsec;
}

static int cache_keyeq(const cachekey_t *a, const cachekey_t *b)
{
  return a->dev == b->dev && a->ino == b->ino &&
    a->size == b->size && a->mtime == b->mtime &&
    a->mtime_nsec == b->mtime_nsec && a->ctime == b->ctime &&
    a->ctime_nsec == b->ctime_nsec;
}

static unsigned cache_hash(const cachekey_t *k)
{
  uint64_t h = k->dev ^ (k->dev >> 33) ^ k->ino ^ (k->ino >> 33);
  return (unsigned)(h % CACHE_BUCKETS);
}

static cacheent_t *cache_find(const cachekey_t *k)
{
  cacheent_t *e = cache_tab[cache_hash(k)];
  while (e != NULL) {
    if (cache_keyeq(&e->key, k))
      return e;
    e = e->next;
  }
  return NULL;
}

/* path for this user; creates $dir/hekon on demand. NULL when no home. */
static char *cache_default_path(void)
{
  const char *xdg = getenv("XDG_CACHE_HOME");
  const char *home = getenv("HOME");
  const char *base;
  char *dir;
  char *path;
  size_t need;

  base = (xdg != NULL && *xdg != '\0') ? xdg : NULL;
  if (base == NULL) {
    static char fb[PATH_MAX];
    if (home == NULL || *home == '\0')
      return NULL;
    if (snprintf(fb, sizeof fb, "%s/.cache", home) >= (int)sizeof fb)
      return NULL;
    base = fb;
  }

  need = strlen(base) + strlen("/hekon") + 1;
  dir = (char*) malloc(need);
  if (dir == NULL) {
    errormsg("out of memory\n");
    exit(1);
  }
  snprintf(dir, need, "%s/hekon", base);
  mkdir(base, 0700); /* best-effort; fopen below reports real failures */
  mkdir(dir, 0700);

  need = strlen(dir) + 1 + strlen(CACHE_FILENAME) + 1;
  path = (char*) malloc(need);
  if (path == NULL) {
    errormsg("out of memory\n");
    exit(1);
  }
  snprintf(path, need, "%s/%s", dir, CACHE_FILENAME);
  free(dir);
  return path;
}

static int cache_rd32(FILE *f, uint32_t *v)
{
  unsigned char b[4];
  if (fread(b, 1, 4, f) != 4)
    return 0;
  *v = (uint32_t)b[0] | ((uint32_t)b[1] << 8) |
    ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
  return 1;
}

static int cache_rd64(FILE *f, uint64_t *v)
{
  unsigned char b[8];
  int i;
  uint64_t r = 0;
  if (fread(b, 1, 8, f) != 8)
    return 0;
  for (i = 7; i >= 0; i--)
    r = (r << 8) | b[i];
  return (*v = r), 1;
}

static void cache_wr32(FILE *f, uint32_t v)
{
  unsigned char b[4];
  b[0] = v & 0xff;
  b[1] = (v >> 8) & 0xff;
  b[2] = (v >> 16) & 0xff;
  b[3] = (v >> 24) & 0xff;
  fwrite(b, 1, 4, f);
}

static void cache_wr64(FILE *f, uint64_t v)
{
  unsigned char b[8];
  int i;
  for (i = 0; i < 8; i++)
    b[i] = (v >> (8 * i)) & 0xff;
  fwrite(b, 1, 8, f);
}

static int cache_rdkey(FILE *f, cachekey_t *k)
{
  uint64_t u[7];
  int i;
  for (i = 0; i < 7; i++) {
    if (!cache_rd64(f, &u[i]))
      return 0;
  }
  k->dev = u[0];
  k->ino = u[1];
  k->size = (int64_t)u[2];
  k->mtime = (int64_t)u[3];
  k->mtime_nsec = (int64_t)u[4];
  k->ctime = (int64_t)u[5];
  k->ctime_nsec = (int64_t)u[6];
  return 1;
}

static void cache_load(void)
{
  FILE *f;
  char magic[CACHE_MAGIC_LEN];
  uint32_t ver;

  if (cache_have)
    return;
  cache_have = 1;

  cache_path = cache_default_path();
  if (cache_path == NULL)
    return; /* no home: run uncached */

  f = fopen(cache_path, "rb");
  if (f == NULL)
    return; /* cold start: nothing cached yet */

  if (fread(magic, 1, CACHE_MAGIC_LEN, f) != CACHE_MAGIC_LEN ||
      memcmp(magic, CACHE_MAGIC, CACHE_MAGIC_LEN) != 0 ||
      !cache_rd32(f, &ver) || ver != CACHE_VERSION) {
    fclose(f); /* foreign or future file: start fresh, overwrite on save */
    cache_dirty = 1;
    return;
  }

  for (;;) {
    cachekey_t k;
    unsigned char hdr[4];
    unsigned bucket;
    cacheent_t *e;
    if (!cache_rdkey(f, &k))
      break; /* clean EOF or truncated tail: stop */
    if (fread(hdr, 1, 4, f) != 4)
      break;
    e = (cacheent_t*) malloc(sizeof(cacheent_t));
    if (e == NULL) {
      errormsg("out of memory\n");
      exit(1);
    }
    e->key = k;
    e->alg = hdr[0];
    e->has_partial = hdr[1] ? 1 : 0;
    e->has_full = hdr[2] ? 1 : 0;
    e->live = 0;
    if (fread(e->partial, 1, MD5_DIGEST_LENGTH, f) != MD5_DIGEST_LENGTH ||
        fread(e->full, 1, MD5_DIGEST_LENGTH, f) != MD5_DIGEST_LENGTH) {
      free(e);
      break; /* truncated tail: ignore it */
    }
    if (e->alg != CACHE_ALG_MD5 || (!e->has_partial && !e->has_full)) {
      free(e);
      continue; /* unknown-alg entries are never trusted */
    }
    /* newest record wins: file is append-mostly, so prepend here */
    bucket = cache_hash(&k);
    e->next = cache_tab[bucket];
    cache_tab[bucket] = e;
  }
  fclose(f);
}

/* keep a loaded entry alive when its file is still on disk (see above) */
static void cache_touch(file_t *f)
{
  cachekey_t k;
  cacheent_t *e;
  cache_keyof(f, &k);
  e = cache_find(&k);
  if (e != NULL)
    e->live = 1;
}

/* partial digest from cache; 1 on hit (digest installed), 0 on miss */
static int cache_fetch_partial(file_t *f)
{
  cachekey_t k;
  cacheent_t *e;
  cache_keyof(f, &k);
  e = cache_find(&k);
  if (e == NULL || !e->has_partial)
    return 0;
  f->crcpartial = (md5_byte_t*) malloc(MD5_DIGEST_LENGTH);
  if (f->crcpartial == NULL) {
    errormsg("out of memory\n");
    exit(1);
  }
  memcpy(f->crcpartial, e->partial, MD5_DIGEST_LENGTH);
  e->live = 1;
  g_stats.cache_hits_partial++;
  return 1;
}

/* full digest from cache; 1 on hit, 0 on miss */
static int cache_fetch_full(file_t *f)
{
  cachekey_t k;
  cacheent_t *e;
  cache_keyof(f, &k);
  e = cache_find(&k);
  if (e == NULL || !e->has_full)
    return 0;
  f->crcsignature = (md5_byte_t*) malloc(MD5_DIGEST_LENGTH);
  if (f->crcsignature == NULL) {
    errormsg("out of memory\n");
    exit(1);
  }
  memcpy(f->crcsignature, e->full, MD5_DIGEST_LENGTH);
  e->live = 1;
  g_stats.cache_hits_full++;
  return 1;
}

static void cache_store(file_t *f, int is_full)
{
  cachekey_t k;
  cacheent_t *e;
  cache_keyof(f, &k);
  e = cache_find(&k);
  if (e == NULL) {
    e = (cacheent_t*) malloc(sizeof(cacheent_t));
    if (e == NULL) {
      errormsg("out of memory\n");
      exit(1);
    }
    e->key = k;
    e->alg = CACHE_ALG_MD5;
    e->has_partial = 0;
    e->has_full = 0;
    e->next = cache_tab[cache_hash(&k)];
    cache_tab[cache_hash(&k)] = e;
  }
  if (is_full) {
    if (f->crcsignature == NULL)
      return;
    memcpy(e->full, f->crcsignature, MD5_DIGEST_LENGTH);
    e->has_full = 1;
  } else {
    if (f->crcpartial == NULL)
      return;
    memcpy(e->partial, f->crcpartial, MD5_DIGEST_LENGTH);
    e->has_partial = 1;
  }
  e->live = 1;
  cache_dirty = 1;
}

/* atomic replace (tmp + rename); skips when nothing new was learned */
static void cache_save(void)
{
  FILE *f;
  char *tmp;
  size_t need;
  int b;

  if (!cache_dirty || cache_path == NULL)
    return;

  need = strlen(cache_path) + 16;
  tmp = (char*) malloc(need);
  if (tmp == NULL) {
    errormsg("out of memory\n");
    exit(1);
  }
  snprintf(tmp, need, "%s.tmp.%d", cache_path, (int)getpid());
  f = fopen(tmp, "wb");
  if (f == NULL) {
    errormsg("cannot write cache %s\n", tmp);
    free(tmp);
    return;
  }

  fwrite(CACHE_MAGIC, 1, CACHE_MAGIC_LEN, f);
  cache_wr32(f, CACHE_VERSION);
  for (b = 0; b < CACHE_BUCKETS; b++) {
    cacheent_t *e = cache_tab[b];
    while (e != NULL) {
      if (e->live) {
        unsigned char hdr[4];
        cache_wr64(f, e->key.dev);
        cache_wr64(f, e->key.ino);
        cache_wr64(f, (uint64_t)e->key.size);
        cache_wr64(f, (uint64_t)e->key.mtime);
        cache_wr64(f, (uint64_t)e->key.mtime_nsec);
        cache_wr64(f, (uint64_t)e->key.ctime);
        cache_wr64(f, (uint64_t)e->key.ctime_nsec);
        hdr[0] = e->alg;
        hdr[1] = (unsigned char)e->has_partial;
        hdr[2] = (unsigned char)e->has_full;
        hdr[3] = 0;
        fwrite(hdr, 1, 4, f);
        fwrite(e->partial, 1, MD5_DIGEST_LENGTH, f);
        fwrite(e->full, 1, MD5_DIGEST_LENGTH, f);
      }
      e = e->next;
    }
  }
  if (fclose(f) != 0) {
    errormsg("cannot write cache %s\n", tmp);
    free(tmp);
    return;
  }
  if (rename(tmp, cache_path) != 0)
    errormsg("cannot write cache %s\n", cache_path);
  free(tmp);
  cache_dirty = 0;
}

/* --------------------------------------------- two-phase parallel matching */

int confirmpair(file_t *a, file_t *b); /* defined below; keep deletion
                                          logic single-threaded */

typedef struct {
  file_t *f;
  int idx; /* position in pre-sort (TARGET-first) order: keeps every sort
              stable so output is deterministic at any -j */
} ord_t;

static int cmp_size(const void *a, const void *b)
{
  const ord_t *x = (const ord_t*)a;
  const ord_t *y = (const ord_t*)b;
  if (x->f->size < y->f->size)
    return -1;
  if (x->f->size > y->f->size)
    return 1;
  if (x->idx < y->idx)
    return -1;
  if (x->idx > y->idx)
    return 1;
  return 0;
}

static int cmp_partial(const void *a, const void *b)
{
  const ord_t *x = (const ord_t*)a;
  const ord_t *y = (const ord_t*)b;
  int r;
  if (x->f->size < y->f->size)
    return -1;
  if (x->f->size > y->f->size)
    return 1;
  r = md5cmp(x->f->crcpartial, y->f->crcpartial);
  if (r != 0)
    return r;
  if (x->idx < y->idx)
    return -1;
  if (x->idx > y->idx)
    return 1;
  return 0;
}

static int cmp_full(const void *a, const void *b)
{
  const ord_t *x = (const ord_t*)a;
  const ord_t *y = (const ord_t*)b;
  int r;
  if (x->f->size < y->f->size)
    return -1;
  if (x->f->size > y->f->size)
    return 1;
  r = md5cmp(x->f->crcsignature, y->f->crcsignature);
  if (r != 0)
    return r;
  if (x->idx < y->idx)
    return -1;
  if (x->idx > y->idx)
    return 1;
  return 0;
}

typedef struct {
  ord_t *items; /* files to hash */
  int n;
  int next;     /* next index to claim (under lock) */
  int use_full; /* 0 = partial stage, 1 = full stage */
  long long hashes;
  long long bytes;
  pthread_mutex_t lock;
} hashwork_t;

static void *hash_worker(void *arg)
{
  hashwork_t *w = (hashwork_t*)arg;
  for (;;) {
    file_t *f;
    long long used = 0;
    int ok;

    pthread_mutex_lock(&w->lock);
    if (w->next >= w->n) {
      pthread_mutex_unlock(&w->lock);
      break;
    }
    f = w->items[w->next++].f;
    pthread_mutex_unlock(&w->lock);

    if (got_sigint)
      break;

    ok = hash_one(f, w->use_full, &used);
    if (!ok)
      continue; /* unreadable: digests stay NULL, main thread skips it */

    pthread_mutex_lock(&w->lock);
    w->hashes++;
    w->bytes += used;
    pthread_mutex_unlock(&w->lock);
  }
  return NULL;
}

/* Hash a stage with the worker pool (main thread polls progress so the
   indicator keeps moving through the slow part of the run). Falls back
   to inline hashing if no thread starts, so a -j run still completes. */
static void run_hash_pool(ord_t *items, int n, int use_full, const char *label)
{
  hashwork_t w;
  pthread_t *tids = NULL;
  int nthreads = num_threads();
  int started = 0;
  int i;

  if (n <= 0)
    return;
  if (nthreads > n)
    nthreads = n;

  w.items = items;
  w.n = n;
  w.next = 0;
  w.use_full = use_full;
  w.hashes = 0;
  w.bytes = 0;
  pthread_mutex_init(&w.lock, NULL);

  tids = (pthread_t*) malloc((size_t)nthreads * sizeof(pthread_t));
  if (tids == NULL) {
    errormsg("out of memory\n");
    exit(1);
  }

  for (i = 0; i < nthreads; i++) {
    if (pthread_create(&tids[i], NULL, hash_worker, &w) != 0)
      break;
    started++;
  }

  if (started == 0) {
    long long used = 0;
    for (i = 0; i < n; i++) {
      if (got_sigint)
        break;
      if (hash_one(items[i].f, use_full, &used)) {
        w.hashes++;
        w.bytes += used;
      }
    }
  } else {
    if (!opt_quiet) {
      for (;;) {
        int done;
        pthread_mutex_lock(&w.lock);
        done = w.next;
        pthread_mutex_unlock(&w.lock);
        if (done >= n || got_sigint)
          break;
        fprintf(stderr, "\r%s [%d/%d] ", label, done, n);
        usleep(100000);
      }
      fprintf(stderr, "\r%-40s\r", " ");
    }
    for (i = 0; i < started; i++)
      pthread_join(tids[i], NULL);
  }

  if (use_full) {
    g_stats.full_hashes += w.hashes;
    g_stats.bytes_full += w.bytes;
  } else {
    g_stats.partial_hashes += w.hashes;
    g_stats.bytes_partial += w.bytes;
  }

  pthread_mutex_destroy(&w.lock);
  free(tids);
}

/* true when f shares a device+inode with the head or any chain member */
static int chain_has_hardlink(file_t *head, file_t *f)
{
  file_t *d;
  if (head->inode == f->inode && head->device == f->device)
    return 1;
  for (d = head->duplicates; d != NULL; d = d->duplicates) {
    if (d->inode == f->inode && d->device == f->device)
      return 1;
  }
  return 0;
}

/* A set containing a TARGET file must be headed by one (stable sort
   guarantees it; enforce it anyway so the invariant holds by
   construction, not by luck). Only the flag moves: membership is the
   same, so deletion candidates are unchanged. */
static void promote_target_head(file_t *head)
{
  file_t *prev;
  file_t *d;
  if (MFILE(head)->is_master)
    return;
  prev = head;
  d = head->duplicates;
  while (d != NULL) {
    if (MFILE(d)->is_master) {
      prev->duplicates = d->duplicates;
      d->duplicates = head;
      head->hasdupes = 0;
      d->hasdupes = 1;
      return;
    }
    prev = d;
    d = d->duplicates;
  }
}

/* Chain one confirmed content group (members in TARGET-first list order).
   Same prepend order the old tree walk produced, so dry-run output is
   identical at any -j. MD5 collisions (hashes equal, bytes differ) just
   start a new chain, like the old descend-left path. */
static void confirm_group(ord_t *g, int n)
{
  file_t **heads;
  int nh = 0;
  int i;
  int h;

  heads = (file_t**) malloc((size_t)n * sizeof(file_t*));
  if (heads == NULL) {
    errormsg("out of memory\n");
    exit(1);
  }

  for (i = 0; i < n; i++) {
    file_t *f = g[i].f;
    file_t *hit = NULL;
    int skip = 0;
    if (got_sigint) {
      free(heads);
      printf("\n");
      exit(0);
    }
    for (h = 0; h < nh; h++) {
      if (chain_has_hardlink(heads[h], f)) {
        skip = 1; /* same file by another name: never a duplicate */
        break;
      }
      if (confirmpair(heads[h], f)) {
        hit = heads[h];
        break;
      }
    }
    if (skip)
      continue;
    if (hit != NULL) {
      hit->hasdupes = 1;
      f->duplicates = hit->duplicates;
      hit->duplicates = f;
    } else {
      heads[nh++] = f;
    }
  }

  for (h = 0; h < nh; h++)
    promote_target_head(heads[h]);

  free(heads);
}

/* Two phases: (1) stable sort by size, uniques can never be duplicates;
   (2) hash each size-group with the worker pool (partial, then full only
   within matching partial-groups); the main thread does the byte-for-byte
   confirmation, so deletion logic stays single-threaded and safe. */
static void find_duplicates(file_t *files, int filecount)
{
  ord_t *ord;
  ord_t *stage;
  ord_t *todo;
  ord_t *full;
  int n = 0;
  int ncand;
  int ntodo;
  int nfull;
  int i;
  file_t *f;

  if (files == NULL || filecount <= 0)
    return;

  g_stats.jobs_used = num_threads();

  ord = (ord_t*) malloc((size_t)filecount * sizeof(ord_t));
  stage = (ord_t*) malloc((size_t)filecount * sizeof(ord_t));
  todo = (ord_t*) malloc((size_t)filecount * sizeof(ord_t));
  full = (ord_t*) malloc((size_t)filecount * sizeof(ord_t));
  if (ord == NULL || stage == NULL || todo == NULL || full == NULL) {
    errormsg("out of memory\n");
    exit(1);
  }

  for (f = files; f != NULL; f = f->next) {
    ord[n].f = f;
    ord[n].idx = n;
    n++;
  }

  qsort(ord, (size_t)n, sizeof(ord_t), cmp_size);

  /* non-unique size groups are the only hashing candidates */
  ncand = 0;
  i = 0;
  while (i < n) {
    int j = i + 1;
    int k;
    while (j < n && ord[j].f->size == ord[i].f->size)
      j++;
    if (j - i == 1) {
      g_stats.size_singletons++;
    } else {
      for (k = i; k < j; k++)
        stage[ncand++] = ord[k];
    }
    i = j;
  }

  if (opt_cache) {
    /* cache first: hits skip the pool entirely (single-threaded, no I/O) */
    ntodo = 0;
    {
      int k;
      for (k = 0; k < ncand; k++) {
        if (cache_fetch_partial(stage[k].f))
          continue;
        todo[ntodo++] = stage[k];
      }
    }
    run_hash_pool(todo, ntodo, 0, "Hashing");
    {
      int k;
      for (k = 0; k < ntodo; k++) {
        if (todo[k].f->crcpartial != NULL)
          cache_store(todo[k].f, 0);
      }
    }
  } else {
    run_hash_pool(stage, ncand, 0, "Hashing");
  }
  if (got_sigint) {
    printf("\n");
    exit(0);
  }

  /* regroup by (size, partial): unreadable files drop out, unique
     partials need no full hash */
  {
    int np = 0;
    int k;
    for (k = 0; k < ncand; k++) {
      if (stage[k].f->crcpartial == NULL)
        continue;
      stage[np++] = stage[k];
    }
    qsort(stage, (size_t)np, sizeof(ord_t), cmp_partial);

    ntodo = 0;
    nfull = 0;
    i = 0;
    while (i < np) {
      int j = i + 1;
      while (j < np && stage[j].f->size == stage[i].f->size &&
             md5cmp(stage[j].f->crcpartial, stage[i].f->crcpartial) == 0)
        j++;
      if (j - i == 1) {
        g_stats.partial_singletons++;
      } else {
        int k2;
        for (k2 = i; k2 < j; k2++) {
          if (reuse_partial_as_full(stage[k2].f)) {
            full[nfull++] = stage[k2]; /* small file: digest copied, still confirm */
            if (opt_cache)
              cache_store(stage[k2].f, 1);
          } else if (opt_cache && cache_fetch_full(stage[k2].f)) {
            full[nfull++] = stage[k2]; /* full digest served from cache */
          } else {
            todo[ntodo++] = stage[k2];
          }
        }
      }
      i = j;
    }
  }

  run_hash_pool(todo, ntodo, 1, "Hashing");
  if (got_sigint) {
    printf("\n");
    exit(0);
  }
  if (opt_cache) {
    int k;
    for (k = 0; k < ntodo; k++) {
      if (todo[k].f->crcsignature != NULL)
        cache_store(todo[k].f, 1);
    }
  }

  /* regroup by (size, full): unreadable files drop out, the rest confirm */
  {
    int k;
    for (k = 0; k < ntodo; k++) {
      if (todo[k].f->crcsignature == NULL)
        continue;
      full[nfull++] = todo[k];
    }
    qsort(full, (size_t)nfull, sizeof(ord_t), cmp_full);

    i = 0;
    while (i < nfull) {
      int j = i + 1;
      while (j < nfull && full[j].f->size == full[i].f->size &&
             md5cmp(full[j].f->crcsignature, full[i].f->crcsignature) == 0)
        j++;
      if (j - i > 1)
        confirm_group(full + i, j - i);
      i = j;
    }
  }

  free(ord);
  free(stage);
  free(todo);
  free(full);
}

/* byte-for-byte confirmation of two files by path */
int confirmpair(file_t *a, file_t *b)
{
  FILE *f1;
  FILE *f2;
  int r;

  f1 = fopen(a->d_name, "rb");
  if (!f1)
    return 0;
  f2 = fopen(b->d_name, "rb");
  if (!f2) {
    fclose(f1);
    return 0;
  }

  g_stats.confirms++;
  r = confirmmatch(f1, f2); /* seeks to 0 internally */

  fclose(f1);
  fclose(f2);
  return r;
}

/* ---------------------------------------------------------------- formatting */

void formatbytes(double bytes, char *buf, size_t buflen)
{
  const char *units[] = { "B", "KB", "MB", "GB", "TB" };
  double v = bytes;
  int u = 0;
  while (v >= 1024.0 && u < 4) {
    v /= 1024.0;
    u++;
  }
  if (u == 0)
    snprintf(buf, buflen, "%d %s", (int)v, units[u]);
  else
    snprintf(buf, buflen, "%.1f %s", v, units[u]);
}

/* count the number of duplicate sets (chains with hasdupes) */
int countsets(file_t *files)
{
  int n = 0;
  while (files) {
    if (files->hasdupes)
      n++;
    files = files->next;
  }
  return n;
}

/* Machine-parseable counters on stderr. Every value is exact, not sampled,
   so bench runs can assert on them: e.g. full_reused_small == number of
   <=4KB files that reached the full-hash stage, size_singletons == files
   that never needed hashing at all. */
void print_stats(int filecount)
{
  fprintf(stderr, "stats: files=%d partial_hashes=%lld full_hashes=%lld "
    "full_reused_small=%lld bytes_partial=%lld bytes_full=%lld "
    "bytes_saved_small=%lld confirms=%lld "
    "size_singletons=%lld partial_singletons=%lld jobs=%d "
    "cache_hits_partial=%lld cache_hits_full=%lld "
    "scan_ms=%llu match_ms=%llu total_ms=%llu chunk_size=%d partial_size=%d\n",
    filecount,
    g_stats.partial_hashes, g_stats.full_hashes,
    g_stats.full_reused_small,
    g_stats.bytes_partial, g_stats.bytes_full,
    g_stats.bytes_saved_small, g_stats.confirms,
    g_stats.size_singletons, g_stats.partial_singletons,
    g_stats.jobs_used,
    g_stats.cache_hits_partial, g_stats.cache_hits_full,
    (unsigned long long)g_stats.scan_ms,
    (unsigned long long)g_stats.match_ms,
    (unsigned long long)g_stats.total_ms,
    CHUNK_SIZE, PARTIAL_MD5_SIZE);
}

/* A chain member is deletable only if it lives in SOURCE. TARGET members
   (including extra TARGET copies of the same content) are always kept. */
int is_deletion_candidate(file_t *f)
{
  return !MFILE(f)->is_master;
}

/* ---------------------------------------------------------------- dry-run listing */

void print_dryrun(file_t *files)
{
  file_t *cur;
  file_t *dupe;
  int totalsets = countsets(files);
  long long candidate_files = 0;
  long long reclaim_bytes = 0;
  long long master_copies = 0;
  char sizebuf[64];

  cur = files;
  while (cur) {
    if (cur->hasdupes) {
      int ndel = 0;
      file_t *d;

      /* count SOURCE candidates in this set first; skip sets with none
         (e.g. duplicates existing only within TARGET) */
      d = cur->duplicates;
      while (d) {
        if (is_deletion_candidate(d))
          ndel++;
        d = d->duplicates;
      }
      if (!is_deletion_candidate(cur) && ndel == 0) {
        /* set lives entirely in TARGET: nothing to do, stay silent */
        cur = cur->next;
        continue;
      }

      if (MFILE(cur)->is_master)
        printf("[MASTER] %s\n", cur->d_name);
      else
        printf("[KEEP] %s\n", cur->d_name);

      dupe = cur->duplicates;
      while (dupe) {
        if (is_deletion_candidate(dupe)) {
          printf("  [DELETE] %s\n", dupe->d_name);
          candidate_files++;
          reclaim_bytes += dupe->size;
        } else {
          printf("  [MASTER] %s\n", dupe->d_name);
          master_copies++;
        }
        dupe = dupe->duplicates;
      }

      printf("\n");
    }
    cur = cur->next;
  }

  (void)totalsets;
  (void)master_copies;

  if (candidate_files == 0) {
    printf("No duplicates found.\n");
  } else {
    formatbytes((double)reclaim_bytes, sizebuf, sizeof(sizebuf));
    printf("%lld file%s (%lld bytes, %s) would be deleted.\n",
      candidate_files, candidate_files == 1 ? "" : "s",
      reclaim_bytes, sizebuf);
    printf("Run with -d/--delete to delete them. TARGET files are never modified.\n");
  }
}

/* ---------------------------------------------------------------- deletion */

void delete_candidates(file_t *files)
{
  file_t *cur;
  file_t *dupe;
  char *errorstring = NULL;
  char sizebuf[64];
  long long deleted = 0;
  long long freed_bytes = 0;
  int failed = 0;

  cur = files;
  while (cur) {
    if (cur->hasdupes) {
      dupe = cur->duplicates;
      while (dupe) {
        if (is_deletion_candidate(dupe)) {
          /* safety: never delete a TARGET file */
          if (MFILE(dupe)->is_master) {
            errormsg("internal error: refusing to delete TARGET file %s\n",
              dupe->d_name);
            failed++;
          } else {
            int result = removeifnotchanged(dupe, &errorstring);
            if (result == 0) {
              deleted++;
              freed_bytes += dupe->size;
              printf("Deleted %s\n", dupe->d_name);
            } else {
              failed++;
              errormsg("could not delete %s: %s\n", dupe->d_name,
                errorstring ? errorstring : "unknown error");
            }
          }
        }
        dupe = dupe->duplicates;
      }
    }
    cur = cur->next;
  }

  formatbytes((double)freed_bytes, sizebuf, sizeof(sizebuf));
  if (deleted == 0 && failed == 0) {
    printf("No duplicates found; nothing deleted.\n");
  } else {
    printf("\nDeleted %lld file%s (%lld bytes, %s).",
      deleted, deleted == 1 ? "" : "s",
      freed_bytes, sizebuf);
    if (failed > 0)
      printf(" %d file%s could not be deleted.", failed, failed == 1 ? "" : "s");
    printf("\n");
  }
}

/* ---------------------------------------------------------------- path safety */

/* true if path == dir or path is strictly inside dir (dir prefix + '/') */
int path_inside_or_equal(const char *dir, const char *path)
{
  size_t len = strlen(dir);
  if (strcmp(dir, path) == 0)
    return 1;
  if (strncmp(dir, path, len) != 0)
    return 0;
  return path[len] == '/';
}

/* ---------------------------------------------------------------- help / version */

void help_text(void)
{
  printf(
    "Usage: hekon [options] SOURCE TARGET\n\n"
    "Delete files in SOURCE that duplicate files in TARGET (the master,\n"
    "which is never modified). Without options, only list what would be\n"
    "deleted; delete nothing.\n\n"
    "If identical files exist only within SOURCE (no copy in TARGET),\n"
    "one copy is kept and the rest are treated as duplicates.\n\n"
    "  -d --delete   actually delete the SOURCE duplicates\n"
    "  -q --quiet    hide progress indicator\n"
    "  -j --jobs N   hash with N threads (default: online CPUs, max 8)\n"
    "  --cache       reuse hashes across runs (flat file under $XDG_CACHE_HOME/hekon/)\n"
    "  --cache-clear delete the hash cache and exit\n"
    "  --stats       print machine-parseable counters/timings to stderr\n"
    "  -v --version  display version\n"
    "  -h --help     display this help message\n\n"
    "Both directories are recursed. Symlinks are not followed.\n"
    "Matching: file size, partial/full MD5, then byte-for-byte confirm.\n");
}

/* ---------------------------------------------------------------- main */

int main(int argc, char **argv)
{
  int opt;
  file_t *files = NULL;
  file_t *curfile;
  int filecount = 0;
  char *sourcedir;
  char *targetdir;
  struct stat sourcestat;
  struct stat targetstat;
  char *realsource = NULL;
  char *realtarget = NULL;

#ifdef HAVE_GETOPT_H
  static struct option long_options[] = {
    { "delete",  0, 0, 'd' },
    { "quiet",   0, 0, 'q' },
    { "stats",   0, 0, 1000 },
    { "jobs",    1, 0, 'j' },
    { "cache",   0, 0, 1001 },
    { "cache-clear", 0, 0, 1002 },
    { "version", 0, 0, 'v' },
    { "help",    0, 0, 'h' },
    { 0, 0, 0, 0 }
  };
#define GETOPT getopt_long
#else
#define GETOPT getopt
#endif

  program_name = argv[0];
  setlocale(LC_CTYPE, "");

  /* --stats/--cache/--cache-clear must also work when built without
     getopt_long (HAVE_GETOPT_H unset): pre-scan argv and strip them so
     plain getopt never sees them. */
  {
    int ai;
    for (ai = 1; ai < argc; ai++) {
      int aj;
      int strip = 0;
      if (strcmp(argv[ai], "--stats") == 0) {
        opt_stats = 1;
        strip = 1;
      } else if (strcmp(argv[ai], "--cache") == 0) {
        opt_cache = 1;
        strip = 1;
      } else if (strcmp(argv[ai], "--cache-clear") == 0) {
        opt_cache_clear = 1;
        strip = 1;
      }
      if (strip) {
        for (aj = ai; aj < argc - 1; aj++)
          argv[aj] = argv[aj + 1];
        argc--;
        ai--;
      }
    }
  }

  while ((opt = GETOPT(argc, argv, "dqvhj:"
#ifdef HAVE_GETOPT_H
          , long_options, NULL
#endif
          )) != EOF) {
    switch (opt) {
    case 'd':
      opt_delete = 1;
      break;
    case 'q':
      opt_quiet = 1;
      break;
    case 1000:
      opt_stats = 1;
      break;
    case 1001:
      opt_cache = 1;
      break;
    case 1002:
      opt_cache_clear = 1;
      break;
    case 'j': {
      char *end = NULL;
      long v = strtol(optarg, &end, 10);
      if (end == optarg || *end != '\0' || v < JOBS_MIN) {
        errormsg("invalid --jobs value '%s' (want %d..%d)\n",
          optarg, JOBS_MIN, JOBS_MAX);
        exit(1);
      }
      opt_jobs = v > JOBS_MAX ? JOBS_MAX : (int)v;
      break;
    }
    case 'v':
      printf("hekon %s\n", HEKON_VERSION);
      exit(0);
    case 'h':
      help_text();
      exit(0);
    default:
      help_text();
      exit(1);
    }
  }

  if (opt_cache_clear) {
    char *p = cache_default_path();
    if (p == NULL) {
      printf("No cache to clear (no home directory).\n");
      exit(0);
    }
    if (unlink(p) == 0)
      printf("Cache cleared (%s).\n", p);
    else
      printf("No cache to clear (%s).\n", p);
    free(p);
    exit(0);
  }

  if (optind + 2 != argc) {
    errormsg("expected exactly SOURCE and TARGET directories\n");
    help_text();
    exit(1);
  }

  sourcedir = argv[optind];
  targetdir = argv[optind + 1];

  if (stat(sourcedir, &sourcestat) != 0 || !S_ISDIR(sourcestat.st_mode)) {
    errormsg("SOURCE '%s' is not an accessible directory\n", sourcedir);
    exit(1);
  }

  if (stat(targetdir, &targetstat) != 0 || !S_ISDIR(targetstat.st_mode)) {
    errormsg("TARGET '%s' is not an accessible directory\n", targetdir);
    exit(1);
  }

  /* refuse overlapping trees: scanning one inside the other would mix
     SOURCE and TARGET roles and risk deleting the wrong files */
  realsource = realpath(sourcedir, NULL);
  realtarget = realpath(targetdir, NULL);
  if (realsource == NULL || realtarget == NULL) {
    errormsg("could not resolve SOURCE/TARGET paths: %s\n", strerror(errno));
    free(realsource);
    free(realtarget);
    exit(1);
  }
  if (strcmp(realsource, realtarget) == 0 ||
      (sourcestat.st_dev == targetstat.st_dev &&
       sourcestat.st_ino == targetstat.st_ino)) {
    errormsg("SOURCE and TARGET must not be the same directory\n");
    free(realsource);
    free(realtarget);
    exit(1);
  }
  if (path_inside_or_equal(realsource, realtarget) ||
      path_inside_or_equal(realtarget, realsource)) {
    errormsg("SOURCE and TARGET must not overlap (one inside the other)\n");
    free(realsource);
    free(realtarget);
    exit(1);
  }
  free(realsource);
  free(realtarget);

  register_sigint_handler();

  {
    uint64_t t_total_start = now64();
    uint64_t t_scan_start = now64();
  /* scan SOURCE first, then TARGET last. The file list is built by
     prepending, so scanning TARGET last puts TARGET files at the head of
     the list; the matching loop then registers TARGET files first, making
     them the set heads (protected) wherever a set contains a TARGET copy. */
  filecount += scan_directory(sourcedir, &files, 0);
  filecount += scan_directory(targetdir, &files, 1);
  g_stats.scan_ms = now64() - t_scan_start;

  if (opt_cache) {
    file_t *tf;
    cache_load();
    /* keep entries for files still on disk alive, so only vanished
       files fall out of the cache on save */
    for (tf = files; tf != NULL; tf = tf->next)
      cache_touch(tf);
  }

  if (!opt_quiet)
    fprintf(stderr, "\r%-40s\r", " ");

  /* match: group by size, hash groups with the pool, confirm inline */
  {
    uint64_t t_match_start = now64();
    find_duplicates(files, filecount);
    g_stats.match_ms = now64() - t_match_start;
  }

  if (opt_cache)
    cache_save();

  if (!opt_quiet)
    fprintf(stderr, "\r%-40s\r", " ");

  if (opt_delete)
    delete_candidates(files);
  else
    print_dryrun(files);

  g_stats.total_ms = now64() - t_total_start;
  if (opt_stats) {
    fflush(stdout);
    print_stats(filecount);
  }

  /* cleanup */
  while (files) {
    curfile = files->next;
    free(files->d_name);
    free(files->crcsignature);
    free(files->crcpartial);
    free(files);
    files = curfile;
  }
  }

  return 0;
}
