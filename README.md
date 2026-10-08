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
- `zstd_seekable_decompress(table, column, rowid, offset, len)` the same range, read from the
  row in place (see [Reading a row in place](#reading-a-row-in-place))
- `zstd_frame_decompress(table, column, rowid, offset, len)` a range of a row that holds one
  frame, read in place from its first byte and only as far as the range ends

**Utilities:**
- `zstd_content_size(data)` decompressed size from frame header

`zstd_uncompress` handles both standard and seekable formats since seekable
is backward-compatible concatenated zstd frames.

## Build

Requires `libzstd`, `libxxhash` and SQLite's headers (`sqlite3ext.h`).

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

-- The same range, the row read in place rather than passed as a value
SELECT zstd_seekable_decompress('archive', 'data', rowid, 10000, 500) FROM archive WHERE name = 'log.txt';
```

## Seekable format

The seekable format writes independent zstd frames with a seek table appended.
`zstd_seekable_decompress` locates the frame covering the requested range and
decompresses only that frame. For a 165 MB transcript, range extraction takes
~0.5 ms vs ~100 ms for full decompression.

A range that starts at or past the end of the data is an empty blob; one that runs past the
end stops there.

### Reading a row in place

A column passed to a function arrives as a value, and SQLite reads the whole value before the
call, so `zstd_seekable_decompress(data, offset, len)` decompresses only the frames the range
covers but reads the entire row to do it. The five-argument form names the row instead (`table`
and `column` in the `main` schema, and its `rowid`) and opens it with SQLite's
[incremental blob API](https://www.sqlite.org/c3ref/blob_open.html), handing it to the seekable
decoder as a reader (`ZSTD_seekable_initAdvanced`, the interface `ZSTD_seekable_initBuff` wraps),
so a read pulls the seek table at the end of the row and only the compressed bytes of the frames
it needs. Within a statement the open handle and the row's parsed seek table are kept from call
to call as [auxiliary data](https://www.sqlite.org/c3ref/get_auxdata.html) on the table argument
(SQLite keeps it while that argument is a constant, as a table name is) and moved from row to
row with `sqlite3_blob_reopen`; they are freed when the statement finishes, so the handle's read
transaction ends with the query.

SQLite stores a value larger than a page as a
[chain of overflow pages](https://www.sqlite.org/fileformat2.html#ovflpgs), and the first read at
an offset goes through the pages of the chain before it, so reaching the seek table at the end
of a row walks the whole row. In an auto-vacuum database whose pages sit in order, SQLite finds
each next page in the pointer map instead (`getOverflowPage` in `btree.c`): on a 37 MB row,
reaching the seek table read 36 MB, and 0.06 MB once the same row was written in order into a
fresh auto-vacuum database. A table that keeps each row to one frame or a few keeps the walk to
the size of a frame.

A row that holds one frame needs no seek table at all. A frame decodes only from its start, so
the compressed bytes before the end of the range are all a read of it uses, and
`zstd_frame_decompress(table, column, rowid, offset, len)` reads exactly those: it opens the row
the same way, feeds it from its first byte to zstd's
[streaming decoder](https://facebook.github.io/zstd/zstd_manual.html), and stops once the range
is complete, so the seek table after the frame and the frame's tail are never read. A seekable
blob of one frame is such a row, as is a plain zstd frame. A range past the frame's end returns
what the frame holds of it, and a row cut off inside its frame is an error. Over 30 ranges read
from an archive of one 4 MiB frame per row (837 KB compressed on average), it read 434 KB per
range, where the value form read 848 KB and the seekable row form 902 KB.

Range decompression pairs well with a byte-offset index like
[`sqlite-fts5x`](https://github.com/MayCXC/sqlite-fts5x) to pull a snippet out of a compressed
archive without decompressing the whole thing.

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
