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
#include <limits.h>
#include <locale.h>
#ifdef HAVE_GETOPT_H
#include <getopt.h>
#endif

#include "hekon.h"
#include "file.h"
#include "confirmmatch.h"
#include "errormsg.h"
#include "sigint.h"
#include "removeifnotchanged.h"

char *program_name;

static int opt_delete = 0;
static int opt_quiet = 0;

typedef struct {
  file_t file;        /* must remain the first member so file_t* casts are valid */
  int is_master;      /* 1 = inside TARGET (protected), 0 = inside SOURCE */
} mfile_t;

#define MFILE(f) ((mfile_t*)(void*)(f))

typedef struct _filetree {
  file_t *file;
  struct _filetree *left;
  struct _filetree *right;
} filetree_t;

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
  static md5_byte_t chunk[CHUNK_SIZE];
  FILE *file;

  digest = (md5_byte_t*) malloc(MD5_DIGEST_LENGTH * sizeof(md5_byte_t));
  if (digest == NULL) {
    errormsg("out of memory\n");
    exit(1);
  }

  md5_init(&state);

  if (max_read != 0 && fsize > max_read)
    fsize = max_read;

  file = fopen(filename, "rb");
  if (file == NULL) {
    errormsg("error opening file %s\n", filename);
    free(digest);
    return NULL;
  }

  while (fsize > 0) {
    if (got_sigint) {
      fclose(file);
      printf("\n");
      exit(0);
    }

    toread = (fsize >= CHUNK_SIZE) ? CHUNK_SIZE : fsize;
    if (fread(chunk, toread, 1, file) != 1) {
      errormsg("error reading from file %s\n", filename);
      fclose(file);
      free(digest);
      return NULL;
    }
    md5_append(&state, chunk, toread);
    fsize -= toread;
  }

  md5_finish(&state, digest);
  fclose(file);
  return digest;
}

md5_byte_t *getcrcsignature(char *filename, off_t fsize)
{
  return getcrcsignatureuntil(filename, fsize, 0);
}

md5_byte_t *getcrcpartialsignature(char *filename, off_t fsize)
{
  return getcrcsignatureuntil(filename, fsize, PARTIAL_MD5_SIZE);
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

/* ---------------------------------------------------------------- tree */

void purgetree(filetree_t *checktree)
{
  if (checktree == NULL)
    return;
  if (checktree->left != NULL) purgetree(checktree->left);
  if (checktree->right != NULL) purgetree(checktree->right);
  free(checktree);
}

int registerfile(filetree_t **branch, file_t *file)
{
  *branch = (filetree_t*) malloc(sizeof(filetree_t));
  if (*branch == NULL) {
    errormsg("out of memory!\n");
    exit(1);
  }
  (*branch)->file = file;
  (*branch)->left = NULL;
  (*branch)->right = NULL;
  return 1;
}

int is_hardlink(filetree_t *checktree, file_t *file)
{
  file_t *dupe;

  if ((file->inode == checktree->file->inode) &&
      (file->device == checktree->file->device))
    return 1;

  if (checktree->file->hasdupes) {
    dupe = checktree->file->duplicates;
    do {
      if ((file->inode == dupe->inode) &&
          (file->device == dupe->device))
        return 1;
      dupe = dupe->duplicates;
    } while (dupe != NULL);
  }

  return 0;
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

  r = confirmmatch(f1, f2); /* seeks to 0 internally */

  fclose(f1);
  fclose(f2);
  return r;
}

/* Compare a candidate file against the tree. Returns &checktree->file (pointer
   to the chain-head pointer) when a confirmed duplicate is found, NULL when
   the file has been registered into the tree as unique.

   Pipeline: size -> partial md5 -> full md5 -> byte-for-byte confirmation.
   On the practically-impossible md5 collision (hashes equal but bytes differ),
   we descend the tree (treated as less-than) so each true content group still
   ends up correctly grouped. */
file_t **checkmatch(filetree_t *checktree, file_t *file)
{
  int cmpresult;
  int confirmed;

  if (is_hardlink(checktree, file))
    return NULL;

  if (file->size < checktree->file->size)
    cmpresult = -1;
  else if (file->size > checktree->file->size)
    cmpresult = 1;
  else {
    if (checktree->file->crcpartial == NULL) {
      checktree->file->crcpartial = getcrcpartialsignature(checktree->file->d_name, checktree->file->size);
      if (checktree->file->crcpartial == NULL) {
        errormsg("cannot read file %s\n", checktree->file->d_name);
        return NULL;
      }
    }

    if (file->crcpartial == NULL) {
      file->crcpartial = getcrcpartialsignature(file->d_name, file->size);
      if (file->crcpartial == NULL) {
        errormsg("cannot read file %s\n", file->d_name);
        return NULL;
      }
    }

    cmpresult = md5cmp(file->crcpartial, checktree->file->crcpartial);

    if (cmpresult == 0) {
      if (checktree->file->crcsignature == NULL) {
        checktree->file->crcsignature = getcrcsignature(checktree->file->d_name, checktree->file->size);
        if (checktree->file->crcsignature == NULL)
          return NULL;
      }
      if (file->crcsignature == NULL) {
        file->crcsignature = getcrcsignature(file->d_name, file->size);
        if (file->crcsignature == NULL)
          return NULL;
      }

      cmpresult = md5cmp(file->crcsignature, checktree->file->crcsignature);
    }
  }

  if (cmpresult < 0) {
    if (checktree->left != NULL)
      return checkmatch(checktree->left, file);
    registerfile(&(checktree->left), file);
    return NULL;
  } else if (cmpresult > 0) {
    if (checktree->right != NULL)
      return checkmatch(checktree->right, file);
    registerfile(&(checktree->right), file);
    return NULL;
  } else {
    /* hashes match: confirm byte-for-byte before declaring a duplicate */
    confirmed = confirmpair(checktree->file, file);
    if (!confirmed) {
      /* hash collision with different content: descend left and retry so the
         file can still be matched against its true content twin if present. */
      if (checktree->left != NULL)
        return checkmatch(checktree->left, file);
      registerfile(&(checktree->left), file);
      return NULL;
    }
    return &checktree->file;
  }
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
  file_t **match = NULL;
  filetree_t *checktree = NULL;
  int filecount = 0;
  int progress = 0;
  uint64_t last_progress = 0;
  uint64_t now = 0;
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

  while ((opt = GETOPT(argc, argv, "dqvh"
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

  /* scan SOURCE first, then TARGET last. The file list is built by
     prepending, so scanning TARGET last puts TARGET files at the head of
     the list; the matching loop then registers TARGET files first, making
     them the set heads (protected) wherever a set contains a TARGET copy. */
  filecount += scan_directory(sourcedir, &files, 0);
  filecount += scan_directory(targetdir, &files, 1);

  if (!opt_quiet)
    fprintf(stderr, "\r%-40s\r", " ");

  /* match: build duplicate chains */
  curfile = files;
  while (curfile) {
    if (got_sigint) {
      printf("\n");
      exit(0);
    }

    if (!checktree)
      registerfile(&checktree, curfile);
    else {
      match = checkmatch(checktree, curfile);
      if (match != NULL) {
        (*match)->hasdupes = 1;
        curfile->duplicates = (*match)->duplicates;
        (*match)->duplicates = curfile;
      }
    }

    curfile = curfile->next;

    if (!opt_quiet) {
      now = now64();
      if (now - last_progress > FDUPES_PROGRESS_REFRESH_MS) {
        last_progress = now;
        fprintf(stderr, "\rComparing [%d/%d] %d%% ", progress, filecount,
          filecount ? (int)((float)progress / (float)filecount * 100.0) : 100);
      }
      progress++;
    }
  }

  if (!opt_quiet)
    fprintf(stderr, "\r%-40s\r", " ");

  if (opt_delete)
    delete_candidates(files);
  else
    print_dryrun(files);

  /* cleanup */
  while (files) {
    curfile = files->next;
    free(files->d_name);
    free(files->crcsignature);
    free(files->crcpartial);
    free(files);
    files = curfile;
  }
  purgetree(checktree);

  return 0;
}
