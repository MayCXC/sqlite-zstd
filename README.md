# sqlite-zstd

Zstd compression functions for SQLite, with seekable format support for
efficient range decompression.

## Functions

**Standard compression:**
- `zstd_compress(data)` compress at default level (3)
- `zstd_compress(data, level)` compress at specified level (1-22)
- `zstd_uncompress(data)` decompress (size from frame header)
- `zstd_uncompress(data, sz)` decompress with size hint

**Seekable compression** (independent frames with seek table):
- `zstd_seekable_compress(data)` 4 MiB frames, default level
- `zstd_seekable_compress(data, frame_size)` custom frame size
- `zstd_seekable_compress(data, frame_size, level)` custom frame size + level
- `zstd_seekable_decompress(data, offset, len)` range decompression

**Utilities:**
- `zstd_content_size(data)` decompressed size from frame header

`zstd_uncompress` handles both standard and seekable formats since seekable
is backward-compatible concatenated zstd frames.

## Build

Requires `libzstd` and `libxxhash`.

```sh
make
make test
make install  # installs to /usr/local/lib
```

## Usage

```sql
.load ./zstd0

-- Standard compression
SELECT zstd_compress(readfile('large.bin'));
SELECT writefile('out.bin', zstd_uncompress(data)) FROM archive WHERE name = 'large.bin';

-- Seekable: compress with 1 MiB frames
INSERT INTO archive (name, data, sz)
  VALUES ('log.txt', zstd_seekable_compress(readfile('log.txt'), 1048576), file_size('log.txt'));

-- Range decompress: read 500 bytes starting at offset 10000
SELECT zstd_seekable_decompress(data, 10000, 500) FROM archive WHERE name = 'log.txt';
```

## Seekable format

The seekable format writes independent zstd frames with a seek table appended.
`zstd_seekable_decompress` locates the frame covering the requested range and
decompresses only that frame. For a 165 MB transcript, range extraction takes
~0.5 ms vs ~100 ms for full decompression.

The seekable implementation in `seekable/` is from the
[zstd contrib directory](https://github.com/facebook/zstd/tree/dev/contrib/seekable_format),
licensed under BSD + GPLv2 by Meta Platforms.

## Static linking

For embedding in another SQLite binary:

```sh
make static
# Link sqlite-zstd.o, zstdseek_compress.o, zstdseek_decompress.o with -lzstd -lxxhash
```

Define `SQLITE_CORE` and `SQLITE_ZSTD_STATIC` when compiling.

## License

BSD 3-Clause. See [LICENSE](LICENSE).

The seekable format files in `seekable/` are Copyright Meta Platforms,
licensed under BSD + GPLv2.
