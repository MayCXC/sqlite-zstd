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
	@echo "=== sqlar compat (uncompressed passthrough) ==="
	sqlite3 :memory: ".load ./zstd0" \
	  "SELECT length(zstd_compress(X'AABB'));" \
	  "SELECT length(zstd_uncompress(zstd_compress(X'AABB'), 2));"

clean:
	rm -f $(TARGET) *.o

.PHONY: install test clean static
