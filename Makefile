CC ?= gcc
PREFIX ?= /usr/local
INSTALL_LIB_DIR = $(PREFIX)/lib

CFLAGS += -Wall -Wextra -O2
LDFLAGS = -lzstd -lxxhash

TARGET = zstd0.so
ifeq ($(shell uname),Darwin)
  TARGET = zstd0.dylib
endif

SEEKABLE_SRC = seekable/zstdseek_compress.c seekable/zstdseek_decompress.c

$(TARGET): zstd0.c zstd0.h $(SEEKABLE_SRC)
	$(CC) -fPIC -shared $(CFLAGS) -I. zstd0.c $(SEEKABLE_SRC) -o $@ $(LDFLAGS)

static: zstd0.c zstd0.h $(SEEKABLE_SRC)
	$(CC) -c -O2 -DSQLITE_CORE -DSQLITE_ZSTD_STATIC $(CFLAGS) -I. zstd0.c -o zstd0.o
	$(CC) -c -O2 $(CFLAGS) -I. seekable/zstdseek_compress.c -o zstdseek_compress.o
	$(CC) -c -O2 $(CFLAGS) -I. seekable/zstdseek_decompress.c -o zstdseek_decompress.o

install: $(TARGET)
	install -d $(INSTALL_LIB_DIR)
	install -m 644 $(TARGET) $(INSTALL_LIB_DIR)
	install -m 644 zstd0.h $(INSTALL_LIB_DIR)/../include/ 2>/dev/null || true

test: $(TARGET)
	@echo "=== zstd_compress / zstd_uncompress ==="
	sqlite3 :memory: ".load ./zstd0" \
	  "SELECT length(zstd_compress(zeroblob(10000)));" \
	  "SELECT length(zstd_uncompress(zstd_compress(zeroblob(10000))));" \
	  "SELECT length(zstd_uncompress(zstd_compress(zeroblob(10000)), 10000));" \
	  "SELECT zstd_content_size(zstd_compress(zeroblob(10000)));"
	@echo "=== zstd_seekable_compress / zstd_seekable_decompress ==="
	sqlite3 :memory: ".load ./zstd0" \
	  "SELECT length(zstd_seekable_compress(zeroblob(100000), 16384));" \
	  "SELECT length(zstd_uncompress(zstd_seekable_compress(zeroblob(100000), 16384)));" \
	  "SELECT length(zstd_seekable_decompress(zstd_seekable_compress(zeroblob(100000), 16384), 0, 500));" \
	  "SELECT zstd_content_size(zstd_seekable_compress(zeroblob(100000), 16384));"
	@echo "=== zstd_seekable_decompress at and past the end (each check fails the target) ==="
	sqlite3 -bail :memory: ".load ./zstd0" \
	  "CREATE TABLE s AS SELECT zstd_seekable_compress(zeroblob(100000), 16384) AS d;" \
	  "SELECT 'offset at the end: ' || (SELECT CASE WHEN typeof(v) = 'blob' AND length(v) = 0 THEN 'ok' ELSE abs(-9223372036854775808) END FROM (SELECT zstd_seekable_decompress(d, 100000, 10) AS v FROM s));" \
	  "SELECT 'offset just past the end: ' || (SELECT CASE WHEN typeof(v) = 'blob' AND length(v) = 0 THEN 'ok' ELSE abs(-9223372036854775808) END FROM (SELECT zstd_seekable_decompress(d, 100001, 1) AS v FROM s));" \
	  "SELECT 'offset far past the end: ' || (SELECT CASE WHEN typeof(v) = 'blob' AND length(v) = 0 THEN 'ok' ELSE abs(-9223372036854775808) END FROM (SELECT zstd_seekable_decompress(d, 200000, 10) AS v FROM s));" \
	  "SELECT 'range over the end: ' || (SELECT CASE WHEN length(v) = 5 THEN 'ok' ELSE abs(-9223372036854775808) END FROM (SELECT zstd_seekable_decompress(d, 99995, 10) AS v FROM s));" \
	  "SELECT 'raw, offset past the end: ' || CASE WHEN length(zstd_seekable_decompress(X'0102030405', 10, 2)) = 0 THEN 'ok' ELSE abs(-9223372036854775808) END;" \
	  "SELECT 'raw, offset past 2^31: ' || CASE WHEN length(zstd_seekable_decompress(X'0102030405', 3000000000, 2)) = 0 THEN 'ok' ELSE abs(-9223372036854775808) END;" \
	  "SELECT 'raw, range over the end: ' || CASE WHEN zstd_seekable_decompress(X'0102030405', 3, 10) = X'0405' THEN 'ok' ELSE abs(-9223372036854775808) END;"
	@echo "=== zstd_seekable_decompress(table, column, rowid, offset, len) (each check fails the target) ==="
	sqlite3 -bail :memory: ".load ./zstd0" \
	  "CREATE TABLE t(name TEXT PRIMARY KEY, src BLOB, data BLOB);" \
	  "WITH RECURSIVE n(i) AS (SELECT 1 UNION ALL SELECT i + 1 FROM n WHERE i < 20000) INSERT INTO t SELECT 'text', s, zstd_seekable_compress(s, 16384) FROM (SELECT CAST(group_concat(i || ' a line of text', char(10)) AS BLOB) AS s FROM n);" \
	  "INSERT INTO t VALUES ('raw', X'0102030405', zstd_seekable_compress(X'0102030405')), ('empty', X'', X'');" \
	  "CREATE TABLE r(off INTEGER, len INTEGER);" \
	  "INSERT INTO r SELECT o, l FROM (SELECT length(src) AS n FROM t WHERE name = 'text'), (SELECT 0 AS o, 100 AS l UNION ALL SELECT 16380, 10 UNION ALL SELECT 16384, 1 UNION ALL SELECT 50000, 40000 UNION ALL SELECT 3000000000, 2) UNION ALL SELECT n - 5, 10 FROM (SELECT length(src) AS n FROM t WHERE name = 'text') UNION ALL SELECT n, 10 FROM (SELECT length(src) AS n FROM t WHERE name = 'text') UNION ALL SELECT n + 1, 1 FROM (SELECT length(src) AS n FROM t WHERE name = 'text') UNION ALL SELECT 0, n FROM (SELECT length(src) AS n FROM t WHERE name = 'text');" \
	  "SELECT 'one row, every range, against the source bytes: ' || CASE WHEN sum(zstd_seekable_decompress('t', 'data', t.rowid, r.off, r.len) = substr(t.src, r.off + 1, r.len)) = count(*) THEN 'ok' ELSE abs(-9223372036854775808) END FROM t, r WHERE t.name = 'text';" \
	  "CREATE TABLE calls AS SELECT t.rowid AS id, r.off, r.len, zstd_seekable_decompress(t.data, r.off, r.len) AS w FROM r, t ORDER BY r.rowid, t.rowid;" \
	  "SELECT 'another row on every call, against the value form: ' || CASE WHEN sum(zstd_seekable_decompress('t', 'data', id, off, len) = w) = count(*) THEN 'ok' ELSE abs(-9223372036854775808) END FROM calls;"
	@echo "=== the row form's errors (each must fail) ==="
	! sqlite3 -bail :memory: ".load ./zstd0" "CREATE TABLE t(data BLOB);" "SELECT zstd_seekable_decompress('t', 'data', 7, 0, 1);"
	! sqlite3 -bail :memory: ".load ./zstd0" "SELECT zstd_seekable_decompress('missing', 'data', 1, 0, 1);"
	! sqlite3 -bail :memory: ".load ./zstd0" "CREATE TABLE t(data BLOB);" "INSERT INTO t VALUES (NULL);" "SELECT zstd_seekable_decompress('t', 'data', 1, 0, 1);"
	@echo "=== zstd_frame_decompress(table, column, rowid, offset, len) (each check fails the target) ==="
	sqlite3 -bail :memory: ".load ./zstd0" \
	  "CREATE TABLE t(name TEXT PRIMARY KEY, src BLOB, data BLOB);" \
	  "WITH RECURSIVE n(i) AS (SELECT 1 UNION ALL SELECT i + 1 FROM n WHERE i < 20000) INSERT INTO t SELECT 'text', s, zstd_seekable_compress(s, 1048576) FROM (SELECT CAST(group_concat(i || ' a line of text', char(10)) AS BLOB) AS s FROM n);" \
	  "INSERT INTO t VALUES ('raw', X'0102030405', zstd_seekable_compress(X'0102030405')), ('empty', X'', X'');" \
	  "SELECT 'the text row is one frame: ' || CASE WHEN zstd_content_size(data) = length(src) AND length(zstd_seekable_decompress(data, 0, 1048576)) = length(src) THEN 'ok' ELSE abs(-9223372036854775808) END FROM t WHERE name = 'text';" \
	  "CREATE TABLE r(off INTEGER, len INTEGER);" \
	  "INSERT INTO r SELECT o, l FROM (SELECT length(src) AS n FROM t WHERE name = 'text'), (SELECT 0 AS o, 100 AS l UNION ALL SELECT 16380, 10 UNION ALL SELECT 131071, 2 UNION ALL SELECT 131072, 1 UNION ALL SELECT 50000, 40000 UNION ALL SELECT 3000000000, 2) UNION ALL SELECT n - 5, 10 FROM (SELECT length(src) AS n FROM t WHERE name = 'text') UNION ALL SELECT n, 10 FROM (SELECT length(src) AS n FROM t WHERE name = 'text') UNION ALL SELECT n + 1, 1 FROM (SELECT length(src) AS n FROM t WHERE name = 'text') UNION ALL SELECT 0, n FROM (SELECT length(src) AS n FROM t WHERE name = 'text');" \
	  "SELECT 'one frame, every range, against the source bytes: ' || CASE WHEN sum(zstd_frame_decompress('t', 'data', t.rowid, r.off, r.len) = substr(t.src, r.off + 1, r.len)) = count(*) THEN 'ok' ELSE abs(-9223372036854775808) END FROM t, r WHERE t.name = 'text';" \
	  "CREATE TABLE calls AS SELECT t.rowid AS id, r.off, r.len, zstd_seekable_decompress(t.data, r.off, r.len) AS w FROM r, t ORDER BY r.rowid, t.rowid;" \
	  "SELECT 'another row on every call, against the value form: ' || CASE WHEN sum(zstd_frame_decompress('t', 'data', id, off, len) = w) = count(*) THEN 'ok' ELSE abs(-9223372036854775808) END FROM calls;"
	@echo "=== the frame form's errors (each must fail) ==="
	! sqlite3 -bail :memory: ".load ./zstd0" "CREATE TABLE t(data BLOB);" "SELECT zstd_frame_decompress('t', 'data', 7, 0, 1);"
	! sqlite3 -bail :memory: ".load ./zstd0" "SELECT zstd_frame_decompress('missing', 'data', 1, 0, 1);"
	! sqlite3 -bail :memory: ".load ./zstd0" "CREATE TABLE t(data BLOB);" "INSERT INTO t VALUES (NULL);" "SELECT zstd_frame_decompress('t', 'data', 1, 0, 1);"
	! sqlite3 -bail :memory: ".load ./zstd0" "CREATE TABLE t(data BLOB);" "WITH RECURSIVE n(i) AS (SELECT 1 UNION ALL SELECT i + 1 FROM n WHERE i < 20000) INSERT INTO t SELECT substr(zstd_seekable_compress(CAST(group_concat(i || ' a line of text', char(10)) AS BLOB), 1048576), 1, 1000) FROM n;" "SELECT zstd_frame_decompress('t', 'data', 1, 300000, 10);"
	@echo "=== sqlar compat (uncompressed passthrough) ==="
	sqlite3 :memory: ".load ./zstd0" \
	  "SELECT length(zstd_compress(X'AABB'));" \
	  "SELECT length(zstd_uncompress(zstd_compress(X'AABB'), 2));"

clean:
	rm -f $(TARGET) *.o

.PHONY: install test clean static
