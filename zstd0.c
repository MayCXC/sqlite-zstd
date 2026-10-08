/*
** zstd0.c: zstd compression functions for SQLite.
**
** Standard compression:
**   zstd_compress(data)              compress at default level (3)
**   zstd_compress(data, level)       compress at specified level (1-22)
**   zstd_uncompress(data)            decompress (size from frame header)
**   zstd_uncompress(data, sz)        decompress with size hint
**
** Seekable compression (independent frames with seek table):
**   zstd_seekable_compress(data)              4 MiB frames, default level
**   zstd_seekable_compress(data, frame_size)  custom frame size, default level
**   zstd_seekable_compress(data, frame_size, level)  custom frame size + level
**   zstd_seekable_decompress(data, offset, len)      range decompression
**   zstd_seekable_decompress(table, column, rowid, offset, len)
**                                    the same, the row read in place
**
** Utilities:
**   zstd_content_size(data)          decompressed size from frame header
**
** zstd_uncompress handles both standard and seekable formats since
** seekable is backward-compatible concatenated zstd frames.
**
** BSD 3-Clause License. See LICENSE for details.
*/

#include "zstd0.h"

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <zstd.h>
#include "seekable/zstd_seekable.h"

#ifndef SQLITE_CORE
#include "sqlite3ext.h"
SQLITE_EXTENSION_INIT1
#else
#include "sqlite3.h"
#endif

#define ZSTD_DEFAULT_LEVEL 3
#define ZSTD_DEFAULT_FRAME_SIZE (4 << 20)  /* 4 MiB */

/* ---- zstd_compress(data [, level]) ----------------------------------- */

static void fn_compress(sqlite3_context *ctx, int argc, sqlite3_value **argv) {
  const void *src = sqlite3_value_blob(argv[0]);
  int srcLen = sqlite3_value_bytes(argv[0]);
  if (!src || srcLen == 0) {
    sqlite3_result_zeroblob(ctx, 0);
    return;
  }

  int level = ZSTD_DEFAULT_LEVEL;
  if (argc >= 2 && sqlite3_value_type(argv[1]) == SQLITE_INTEGER)
    level = sqlite3_value_int(argv[1]);

  size_t bound = ZSTD_compressBound(srcLen);
  void *dst = sqlite3_malloc64(bound);
  if (!dst) {
    sqlite3_result_error_nomem(ctx);
    return;
  }

  size_t cSize = ZSTD_compress(dst, bound, src, srcLen, level);
  if (ZSTD_isError(cSize)) {
    sqlite3_free(dst);
    sqlite3_result_error(ctx, ZSTD_getErrorName(cSize), -1);
    return;
  }

  /* If compressed is not smaller, return original (sqlar convention). */
  if ((int)cSize >= srcLen) {
    sqlite3_free(dst);
    sqlite3_result_blob(ctx, src, srcLen, SQLITE_TRANSIENT);
  } else {
    sqlite3_result_blob(ctx, dst, (int)cSize, sqlite3_free);
  }
}

/* ---- zstd_uncompress(data [, sz]) ------------------------------------ */

static void fn_uncompress(sqlite3_context *ctx, int argc, sqlite3_value **argv) {
  const void *src = sqlite3_value_blob(argv[0]);
  int srcLen = sqlite3_value_bytes(argv[0]);
  if (!src || srcLen == 0) {
    sqlite3_result_zeroblob(ctx, 0);
    return;
  }

  /* If sz is provided and blob is at least that large, it was stored
     uncompressed (sqlar convention: compressed >= original means raw). */
  if (argc >= 2 && sqlite3_value_type(argv[1]) == SQLITE_INTEGER) {
    int origSz = sqlite3_value_int(argv[1]);
    if (origSz <= 0) {
      sqlite3_result_zeroblob(ctx, 0);
      return;
    }
    if (srcLen >= origSz) {
      sqlite3_result_blob(ctx, src, origSz, SQLITE_TRANSIENT);
      return;
    }
  }

  /* Verify this is actually a zstd frame. If not, return as-is
     (data was stored uncompressed because it didn't compress smaller). */
  unsigned magic = 0;
  if (srcLen >= 4) memcpy(&magic, src, 4);
  if (magic != ZSTD_MAGICNUMBER
      && (magic & 0xFFFFFFF0) != ZSTD_MAGIC_SKIPPABLE_START) {
    sqlite3_result_blob(ctx, src, srcLen, SQLITE_TRANSIENT);
    return;
  }

  /* Get decompressed size from frame header. */
  unsigned long long dSize = ZSTD_getFrameContentSize(src, srcLen);
  if (dSize == ZSTD_CONTENTSIZE_UNKNOWN || dSize == ZSTD_CONTENTSIZE_ERROR) {
    /* Seekable format: multiple frames. Use streaming decompression. */
    size_t capacity = srcLen * 4;
    if (capacity < 65536) capacity = 65536;
    void *dst = sqlite3_malloc64(capacity);
    if (!dst) { sqlite3_result_error_nomem(ctx); return; }

    ZSTD_DCtx *dctx = ZSTD_createDCtx();
    if (!dctx) { sqlite3_free(dst); sqlite3_result_error_nomem(ctx); return; }

    ZSTD_inBuffer in = { src, (size_t)srcLen, 0 };
    size_t written = 0;

    while (in.pos < in.size) {
      if (written >= capacity) {
        capacity *= 2;
        void *p = sqlite3_realloc64(dst, capacity);
        if (!p) {
          sqlite3_free(dst); ZSTD_freeDCtx(dctx);
          sqlite3_result_error_nomem(ctx); return;
        }
        dst = p;
      }
      ZSTD_outBuffer out = { (char *)dst + written, capacity - written, 0 };
      size_t ret = ZSTD_decompressStream(dctx, &out, &in);
      if (ZSTD_isError(ret)) {
        sqlite3_free(dst); ZSTD_freeDCtx(dctx);
        sqlite3_result_error(ctx, ZSTD_getErrorName(ret), -1);
        return;
      }
      written += out.pos;
    }

    ZSTD_freeDCtx(dctx);
    sqlite3_result_blob(ctx, dst, (int)written, sqlite3_free);
    return;
  }

  /* Single frame with known size. */
  void *dst = sqlite3_malloc64(dSize);
  if (!dst) {
    sqlite3_result_error_nomem(ctx);
    return;
  }

  size_t ret = ZSTD_decompress(dst, dSize, src, srcLen);
  if (ZSTD_isError(ret)) {
    sqlite3_free(dst);
    sqlite3_result_error(ctx, ZSTD_getErrorName(ret), -1);
    return;
  }

  sqlite3_result_blob(ctx, dst, (int)ret, sqlite3_free);
}

/* ---- zstd_seekable_compress(data [, frame_size [, level]]) ----------- */

static void fn_seekable_compress(sqlite3_context *ctx, int argc,
                                 sqlite3_value **argv) {
  const void *src = sqlite3_value_blob(argv[0]);
  int srcLen = sqlite3_value_bytes(argv[0]);
  if (!src || srcLen == 0) {
    sqlite3_result_zeroblob(ctx, 0);
    return;
  }

  unsigned frameSize = ZSTD_DEFAULT_FRAME_SIZE;
  int level = ZSTD_DEFAULT_LEVEL;
  if (argc >= 2 && sqlite3_value_type(argv[1]) == SQLITE_INTEGER)
    frameSize = (unsigned)sqlite3_value_int(argv[1]);
  if (argc >= 3 && sqlite3_value_type(argv[2]) == SQLITE_INTEGER)
    level = sqlite3_value_int(argv[2]);
  if (frameSize == 0) frameSize = ZSTD_DEFAULT_FRAME_SIZE;

  ZSTD_seekable_CStream *zcs = ZSTD_seekable_createCStream();
  if (!zcs) { sqlite3_result_error_nomem(ctx); return; }

  size_t ret = ZSTD_seekable_initCStream(zcs, level, 0, frameSize);
  if (ZSTD_isError(ret)) {
    ZSTD_seekable_freeCStream(zcs);
    sqlite3_result_error(ctx, ZSTD_getErrorName(ret), -1);
    return;
  }

  /* Worst case: each frame has its own overhead. */
  size_t nFrames = ((size_t)srcLen + frameSize - 1) / frameSize;
  size_t bound = ZSTD_compressBound(srcLen)
               + nFrames * 64  /* per-frame overhead */
               + ZSTD_seekTableFooterSize
               + nFrames * 8;  /* seek table entries */
  void *dst = sqlite3_malloc64(bound);
  if (!dst) {
    ZSTD_seekable_freeCStream(zcs);
    sqlite3_result_error_nomem(ctx);
    return;
  }

  ZSTD_inBuffer in = { src, (size_t)srcLen, 0 };
  ZSTD_outBuffer out = { dst, bound, 0 };

  while (in.pos < in.size) {
    ret = ZSTD_seekable_compressStream(zcs, &out, &in);
    if (ZSTD_isError(ret)) {
      sqlite3_free(dst); ZSTD_seekable_freeCStream(zcs);
      sqlite3_result_error(ctx, ZSTD_getErrorName(ret), -1);
      return;
    }
  }

  /* Finalize: write remaining frame + seek table. */
  while (1) {
    ret = ZSTD_seekable_endStream(zcs, &out);
    if (ZSTD_isError(ret)) {
      sqlite3_free(dst); ZSTD_seekable_freeCStream(zcs);
      sqlite3_result_error(ctx, ZSTD_getErrorName(ret), -1);
      return;
    }
    if (ret == 0) break;
  }

  ZSTD_seekable_freeCStream(zcs);

  /* If seekable compressed is not smaller, return original. */
  if ((int)out.pos >= srcLen) {
    sqlite3_free(dst);
    sqlite3_result_blob(ctx, src, srcLen, SQLITE_TRANSIENT);
  } else {
    sqlite3_result_blob(ctx, dst, (int)out.pos, sqlite3_free);
  }
}

/* ---- zstd_seekable_decompress(data, offset, len) --------------------- */

/* Decompress len bytes at offset from an initialized seekable stream into the
** function's result. ZSTD_seekable_decompress clamps a range that runs past
** the end of the data but not one that starts at or past it, whose length
** would wrap, so that range is empty here. */
static void result_seekable_range(sqlite3_context *ctx, ZSTD_seekable *zs,
                                  unsigned long long offset, int len) {
  unsigned nFrames = ZSTD_seekable_getNumFrames(zs);
  unsigned long long end = nFrames == 0 ? 0
      : ZSTD_seekable_getFrameDecompressedOffset(zs, nFrames - 1)
        + ZSTD_seekable_getFrameDecompressedSize(zs, nFrames - 1);
  if (offset >= end) {
    sqlite3_result_zeroblob(ctx, 0);
    return;
  }
  if ((unsigned long long)len > end - offset) len = (int)(end - offset);

  void *dst = sqlite3_malloc(len);
  if (!dst) {
    sqlite3_result_error_nomem(ctx);
    return;
  }

  size_t ret = ZSTD_seekable_decompress(zs, dst, len, offset);
  if (ZSTD_isError(ret)) {
    sqlite3_free(dst);
    sqlite3_result_error(ctx, ZSTD_getErrorName(ret), -1);
    return;
  }

  sqlite3_result_blob(ctx, dst, (int)ret, sqlite3_free);
}

static void fn_seekable_decompress(sqlite3_context *ctx, int argc,
                                   sqlite3_value **argv) {
  (void)argc;
  const void *src = sqlite3_value_blob(argv[0]);
  int srcLen = sqlite3_value_bytes(argv[0]);
  unsigned long long offset = (unsigned long long)sqlite3_value_int64(argv[1]);
  int len = sqlite3_value_int(argv[2]);

  if (!src || srcLen == 0 || len <= 0) {
    sqlite3_result_zeroblob(ctx, 0);
    return;
  }

  /* If data is uncompressed (stored raw), extract the range directly. */
  unsigned magic2 = 0;
  if (srcLen >= 4) memcpy(&magic2, src, 4);
  if (magic2 != ZSTD_MAGICNUMBER
      && (magic2 & 0xFFFFFFF0) != ZSTD_MAGIC_SKIPPABLE_START) {
    if (offset >= (unsigned long long)srcLen) { sqlite3_result_zeroblob(ctx, 0); return; }
    int avail = srcLen - (int)offset;
    if (len > avail) len = avail;
    sqlite3_result_blob(ctx, (const char *)src + offset, len, SQLITE_TRANSIENT);
    return;
  }

  ZSTD_seekable *zs = ZSTD_seekable_create();
  if (!zs) { sqlite3_result_error_nomem(ctx); return; }

  size_t ret = ZSTD_seekable_initBuff(zs, src, srcLen);
  if (ZSTD_isError(ret)) {
    ZSTD_seekable_free(zs);
    sqlite3_result_error(ctx, ZSTD_getErrorName(ret), -1);
    return;
  }

  result_seekable_range(ctx, zs, offset, len);
  ZSTD_seekable_free(zs);
}

/* ---- zstd_seekable_decompress(table, column, rowid, offset, len) ----- */

/*
** The same range read with the row read in place. A column passed as an
** argument arrives as a value, which SQLite reads whole before the call; this
** form opens the row with SQLite's incremental blob API instead and hands it to
** the seekable decoder as a reader through ZSTD_seekable_initAdvanced, the
** interface ZSTD_seekable_initBuff and ZSTD_seekable_initFile wrap, so a read
** pulls the seek table and only the compressed bytes of the frames it needs.
** The callbacks are ZSTD_seekable_read_buff and ZSTD_seekable_seek_buff
** (seekable/zstdseek_decompress.c) over sqlite3_blob_read. The open handle and
** the row's parsed seek table are kept as auxiliary data on the table argument,
** which SQLite holds from call to call while that argument is a constant, and
** moved to the next row with sqlite3_blob_reopen. SQLite frees them when the
** statement halts (closeAllCursors in sqlite3VdbeHalt, at the end of the query
** or on a reset or finalize), so no read transaction outlives the query. A row
** stored raw is read in place directly.
** https://www.sqlite.org/c3ref/blob_open.html
** https://www.sqlite.org/c3ref/get_auxdata.html
*/

typedef struct RowSource {
  char *table;
  char *column;
  sqlite3_blob *blob;
  int loaded;           /* blob, size and zs describe rowid */
  sqlite3_int64 rowid;
  long long size;
  long long pos;
  ZSTD_seekable *zs;    /* the row's seek table; NULL for a raw row */
} RowSource;

static int rowsource_read(void *opaque, void *buffer, size_t n) {
  RowSource *rs = (RowSource *)opaque;
  if (rs->pos + (long long)n > rs->size) return -1;
  if (sqlite3_blob_read(rs->blob, buffer, (int)n, (int)rs->pos) != SQLITE_OK)
    return -1;
  rs->pos += (long long)n;
  return 0;
}

static int rowsource_seek(void *opaque, long long offset, int origin) {
  RowSource *rs = (RowSource *)opaque;
  long long newOffset;
  switch (origin) {
    case SEEK_SET: newOffset = offset; break;
    case SEEK_CUR: newOffset = rs->pos + offset; break;
    case SEEK_END: newOffset = rs->size + offset; break;
    default: return -1;
  }
  if (newOffset < 0 || newOffset > rs->size) return -1;
  rs->pos = newOffset;
  return 0;
}

static void rowsource_free(void *p) {
  RowSource *rs = (RowSource *)p;
  if (rs->blob) sqlite3_blob_close(rs->blob);
  ZSTD_seekable_free(rs->zs);
  sqlite3_free(rs->table);
  sqlite3_free(rs->column);
  sqlite3_free(rs);
}

/* Point rs at rowid: move the open handle there or open one, then load the
** row's seek table into a fresh ZSTD_seekable, since loading a table into one
** that holds a table leaks the old one. On failure the function's error is set,
** the handle closed, and SQLITE_ERROR returned. */
static int rowsource_load(sqlite3_context *ctx, RowSource *rs,
                          sqlite3_int64 rowid) {
  sqlite3 *db = sqlite3_context_db_handle(ctx);
  rs->loaded = 0;
  ZSTD_seekable_free(rs->zs);
  rs->zs = NULL;

  /* A handle whose reopen fails is aborted and only good for closing. */
  if (rs->blob && sqlite3_blob_reopen(rs->blob, rowid) != SQLITE_OK) {
    sqlite3_blob_close(rs->blob);
    rs->blob = NULL;
  }
  if (!rs->blob
      && sqlite3_blob_open(db, "main", rs->table, rs->column, rowid, 0,
                           &rs->blob) != SQLITE_OK) {
    sqlite3_result_error(ctx, sqlite3_errmsg(db), -1);
    if (rs->blob) sqlite3_blob_close(rs->blob);
    rs->blob = NULL;
    return SQLITE_ERROR;
  }
  rs->size = sqlite3_blob_bytes(rs->blob);
  rs->pos = 0;

  unsigned magic = 0;
  if (rs->size >= 4 && sqlite3_blob_read(rs->blob, &magic, 4, 0) != SQLITE_OK) {
    sqlite3_result_error(ctx, sqlite3_errmsg(db), -1);
    sqlite3_blob_close(rs->blob);
    rs->blob = NULL;
    return SQLITE_ERROR;
  }
  if (magic == ZSTD_MAGICNUMBER
      || (magic & 0xFFFFFFF0) == ZSTD_MAGIC_SKIPPABLE_START) {
    rs->zs = ZSTD_seekable_create();
    if (!rs->zs) {
      sqlite3_result_error_nomem(ctx);
      return SQLITE_ERROR;
    }
    ZSTD_seekable_customFile src = { rs, rowsource_read, rowsource_seek };
    size_t ret = ZSTD_seekable_initAdvanced(rs->zs, src);
    if (ZSTD_isError(ret)) {
      sqlite3_result_error(ctx, ZSTD_getErrorName(ret), -1);
      ZSTD_seekable_free(rs->zs);
      rs->zs = NULL;
      return SQLITE_ERROR;
    }
  }
  rs->rowid = rowid;
  rs->loaded = 1;
  return SQLITE_OK;
}

static void fn_seekable_decompress_row(sqlite3_context *ctx, int argc,
                                       sqlite3_value **argv) {
  (void)argc;
  const char *table = (const char *)sqlite3_value_text(argv[0]);
  const char *column = (const char *)sqlite3_value_text(argv[1]);
  sqlite3_int64 rowid = sqlite3_value_int64(argv[2]);
  unsigned long long offset = (unsigned long long)sqlite3_value_int64(argv[3]);
  int len = sqlite3_value_int(argv[4]);

  if (!table || !column) {
    sqlite3_result_error(ctx,
        "zstd_seekable_decompress: table and column must be names", -1);
    return;
  }
  if (len <= 0) {
    sqlite3_result_zeroblob(ctx, 0);
    return;
  }

  /* The statement's source for this table and column, or a new one that the
  ** statement keeps once this call is done with it. */
  RowSource *rs = (RowSource *)sqlite3_get_auxdata(ctx, 0);
  int fresh = 0;
  if (!rs || strcmp(rs->table, table) != 0 || strcmp(rs->column, column) != 0) {
    rs = sqlite3_malloc(sizeof(*rs));
    if (!rs) { sqlite3_result_error_nomem(ctx); return; }
    memset(rs, 0, sizeof(*rs));
    rs->table = sqlite3_mprintf("%s", table);
    rs->column = sqlite3_mprintf("%s", column);
    if (!rs->table || !rs->column) {
      rowsource_free(rs);
      sqlite3_result_error_nomem(ctx);
      return;
    }
    fresh = 1;
  }

  if (!rs->loaded || rs->rowid != rowid) {
    if (rowsource_load(ctx, rs, rowid) != SQLITE_OK) {
      if (fresh) rowsource_free(rs);
      return;
    }
  }

  if (rs->size == 0) {
    sqlite3_result_zeroblob(ctx, 0);
  } else if (!rs->zs) {
    if (offset >= (unsigned long long)rs->size) {
      sqlite3_result_zeroblob(ctx, 0);
    } else {
      long long avail = rs->size - (long long)offset;
      if (len > avail) len = (int)avail;
      void *dst = sqlite3_malloc(len);
      if (!dst) {
        sqlite3_result_error_nomem(ctx);
      } else if (sqlite3_blob_read(rs->blob, dst, len, (int)offset) != SQLITE_OK) {
        sqlite3_free(dst);
        sqlite3_result_error(ctx,
            sqlite3_errmsg(sqlite3_context_db_handle(ctx)), -1);
      } else {
        sqlite3_result_blob(ctx, dst, len, sqlite3_free);
      }
    }
  } else {
    result_seekable_range(ctx, rs->zs, offset, len);
  }

  if (fresh) sqlite3_set_auxdata(ctx, 0, rs, rowsource_free);
}

/* ---- zstd_content_size(data) ----------------------------------------- */

static void fn_content_size(sqlite3_context *ctx, int argc,
                            sqlite3_value **argv) {
  (void)argc;
  const void *src = sqlite3_value_blob(argv[0]);
  int srcLen = sqlite3_value_bytes(argv[0]);
  if (!src || srcLen == 0) {
    sqlite3_result_null(ctx);
    return;
  }

  unsigned long long sz = ZSTD_getFrameContentSize(src, srcLen);
  if (sz == ZSTD_CONTENTSIZE_UNKNOWN || sz == ZSTD_CONTENTSIZE_ERROR) {
    /* For seekable format, sum all frame sizes via seek table. */
    ZSTD_seekable *zs = ZSTD_seekable_create();
    if (!zs) { sqlite3_result_null(ctx); return; }
    size_t ret = ZSTD_seekable_initBuff(zs, src, srcLen);
    if (ZSTD_isError(ret)) {
      ZSTD_seekable_free(zs);
      sqlite3_result_null(ctx);
      return;
    }
    unsigned nFrames = ZSTD_seekable_getNumFrames(zs);
    unsigned long long total = 0;
    for (unsigned i = 0; i < nFrames; i++)
      total += ZSTD_seekable_getFrameDecompressedSize(zs, i);
    ZSTD_seekable_free(zs);
    sqlite3_result_int64(ctx, (sqlite3_int64)total);
    return;
  }

  sqlite3_result_int64(ctx, (sqlite3_int64)sz);
}

/* ---- sqlar drop-in aliases ------------------------------------------- */
/*
** sqlar_compress -> zstd_seekable_compress (default frame size)
** sqlar_uncompress -> zstd_uncompress (2-arg, handles seekable transparently)
** Allows the archive table to keep the sqlar schema while using zstd.
*/

/* ---- Entry point ----------------------------------------------------- */

SQLITE_ZSTD_API int sqlite3_zstd_init(sqlite3 *db, char **pzErrMsg,
                                       const sqlite3_api_routines *pApi) {
  (void)pzErrMsg;
#ifndef SQLITE_CORE
  SQLITE_EXTENSION_INIT2(pApi);
#else
  (void)pApi;
#endif

  int rc = SQLITE_OK;

  /* Standard compression */
  rc = sqlite3_create_function(db, "zstd_compress", 1, SQLITE_UTF8, 0,
                               fn_compress, 0, 0);
  if (rc != SQLITE_OK) return rc;
  rc = sqlite3_create_function(db, "zstd_compress", 2, SQLITE_UTF8, 0,
                               fn_compress, 0, 0);
  if (rc != SQLITE_OK) return rc;

  /* Standard decompression (1-arg and 2-arg) */
  rc = sqlite3_create_function(db, "zstd_uncompress", 1, SQLITE_UTF8, 0,
                               fn_uncompress, 0, 0);
  if (rc != SQLITE_OK) return rc;
  rc = sqlite3_create_function(db, "zstd_uncompress", 2, SQLITE_UTF8, 0,
                               fn_uncompress, 0, 0);
  if (rc != SQLITE_OK) return rc;

  /* Seekable compression */
  rc = sqlite3_create_function(db, "zstd_seekable_compress", 1, SQLITE_UTF8, 0,
                               fn_seekable_compress, 0, 0);
  if (rc != SQLITE_OK) return rc;
  rc = sqlite3_create_function(db, "zstd_seekable_compress", 2, SQLITE_UTF8, 0,
                               fn_seekable_compress, 0, 0);
  if (rc != SQLITE_OK) return rc;
  rc = sqlite3_create_function(db, "zstd_seekable_compress", 3, SQLITE_UTF8, 0,
                               fn_seekable_compress, 0, 0);
  if (rc != SQLITE_OK) return rc;

  /* Seekable range decompression, from a value or from a row in place */
  rc = sqlite3_create_function(db, "zstd_seekable_decompress", 3, SQLITE_UTF8,
                               0, fn_seekable_decompress, 0, 0);
  if (rc != SQLITE_OK) return rc;
  rc = sqlite3_create_function(db, "zstd_seekable_decompress", 5, SQLITE_UTF8,
                               0, fn_seekable_decompress_row, 0, 0);
  if (rc != SQLITE_OK) return rc;

  /* Content size */
  rc = sqlite3_create_function(db, "zstd_content_size", 1, SQLITE_UTF8, 0,
                               fn_content_size, 0, 0);
  if (rc != SQLITE_OK) return rc;

  return SQLITE_OK;
}
