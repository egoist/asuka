# miniz

miniz 3.1.2 (tag `3.1.2`, https://github.com/richgel999/miniz, the
release's `miniz-3.1.2.zip`, SHA-256
f0446d863f9c19926ad9483c523fdc42e42b8d4a6a431d27e09d49c79a140d9a), MIT
licensed (LICENSE): the amalgamated `miniz.c` and `miniz.h`, unchanged.
core/src/std/compress.c compiles them in (std/compress's and
std/archive's DEFLATE, on every platform) without miniz's zlib-style
streams, its zip archives, stdio, time and malloc (`MINIZ_NO_*`): only
the compressor (tdefl), the decompressor (tinfl) and the checksums
(CRC-32, Adler-32), their exported names renamed to the core's
(`ak_mz_*`). They build with the core's flags. To update: replace the
two files with a newer release's and change the version here and in
compress.c.
