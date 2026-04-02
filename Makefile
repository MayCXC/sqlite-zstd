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

$(TARGET): sqlite-zstd.c sqlite-zstd.h $(SEEKABLE_SRC)
	$(CC) -fPIC -shared $(CFLAGS) -I. sqlite-zstd.c $(SEEKABLE_SRC) -o $@ $(LDFLAGS)

static: sqlite-zstd.c sqlite-zstd.h $(SEEKABLE_SRC)
	$(CC) -c -O2 -DSQLITE_CORE -DSQLITE_ZSTD_STATIC $(CFLAGS) -I. sqlite-zstd.c -o sqlite-zstd.o
	$(CC) -c -O2 $(CFLAGS) -I. seekable/zstdseek_compress.c -o zstdseek_compress.o
	$(CC) -c -O2 $(CFLAGS) -I. seekable/zstdseek_decompress.c -o zstdseek_decompress.o

install: $(TARGET)
	install -d $(INSTALL_LIB_DIR)
	install -m 644 $(TARGET) $(INSTALL_LIB_DIR)
	install -m 644 sqlite-zstd.h $(INSTALL_LIB_DIR)/../include/ 2>/dev/null || true

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
	@echo "=== sqlar compat (uncompressed passthrough) ==="
	sqlite3 :memory: ".load ./zstd0" \
	  "SELECT length(zstd_compress(X'AABB'));" \
	  "SELECT length(zstd_uncompress(zstd_compress(X'AABB'), 2));"

clean:
	rm -f $(TARGET) *.o

.PHONY: install test clean static
