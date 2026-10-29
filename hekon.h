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
#define PARTIAL_MD5_SIZE 4096

/* time interval to refresh progress indicator (milliseconds) */
#define FDUPES_PROGRESS_REFRESH_MS 100

/* stat supports nanosecond precision (Linux) */
#define HAVE_NSEC_TIMES 1

/* getopt_long lives in getopt.h on glibc/musl/macOS; enable long options
   by default. Builds on exotic platforms can compile with -UHAVE_GETOPT_H. */
#ifndef HAVE_GETOPT_H
#define HAVE_GETOPT_H 1
#endif

#define HEKON_VERSION "1.0.0"

#endif /* HEKON_H */
