/* hekon file record. Same layout as fdupes file_t; renamed header so the
   standalone tool does not depend on fdupes sources. */

#ifndef HEKON_FILE_H
#define HEKON_FILE_H

#include <sys/stat.h>
#include <stdint.h>

typedef struct _file {
  char *d_name;
  off_t size;
  uint8_t *crcpartial;   /* staging digests (XXH3-64); confirmed byte-for-byte */
  uint8_t *crcsignature; /* before any file is listed or deleted */
  dev_t device;
  ino_t inode;
  time_t mtime;
  time_t ctime;
  long mtime_nsec;
  long ctime_nsec;
  int hasdupes; /* true only if file is first on duplicate chain */
  struct _file *duplicates;
  struct _file *next;
} file_t;

#endif /* HEKON_FILE_H */
