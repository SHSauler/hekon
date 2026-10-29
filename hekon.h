/* hekon - delete SOURCE files duplicated in a master TARGET directory.
   Derived from fdupes-master (fdupes family by Adrian Lopez).

   Standalone tool: no autoconf, no ncurses, no sqlite. All tuning
   constants that used to come from config.h live here. */

#ifndef HEKON_H
#define HEKON_H

/* allow handling files greater than 2GB on 32-bit platforms */
#ifndef _FILE_OFFSET_BITS
#define _FILE_OFFSET_BITS 64
#endif

/* number of bytes to read per read call */
#define CHUNK_SIZE 65536

/* max bytes used when calculating partial hashes */
#define PARTIAL_HASH_SIZE 4096

/* time interval to refresh progress indicator (milliseconds) */
#define FDUPES_PROGRESS_REFRESH_MS 100

/* stat supports nanosecond precision (Linux) */
#define HAVE_NSEC_TIMES 1

/* Nanosecond fields are named differently per platform: st_ctim on
   Linux, st_ctimespec on Apple. Use these, not the raw field names. */
#if defined(__APPLE__)
#define STAT_CTIME_NSEC(st) ((st).st_ctimespec.tv_nsec)
#define STAT_MTIME_NSEC(st) ((st).st_mtimespec.tv_nsec)
#elif defined(HAVE_NSEC_TIMES)
#define STAT_CTIME_NSEC(st) ((st).st_ctim.tv_nsec)
#define STAT_MTIME_NSEC(st) ((st).st_mtim.tv_nsec)
#else
#define STAT_CTIME_NSEC(st) 0L
#define STAT_MTIME_NSEC(st) 0L
#endif

/* thread pool sizing (Phase B): auto = online CPUs capped here */
#define AUTO_JOBS_MAX 8

/* explicit -j/--jobs bounds */
#define JOBS_MIN 1
#define JOBS_MAX 64

/* getopt_long lives in getopt.h on glibc/musl/macOS; enable long options
   by default. Builds on exotic platforms can compile with -UHAVE_GETOPT_H. */
#ifndef HAVE_GETOPT_H
#define HAVE_GETOPT_H 1
#endif

#define HEKON_VERSION "1.1.0"

#endif /* HEKON_H */
