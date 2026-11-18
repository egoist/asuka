# std/compress's fixtures

What other tools made, which compress_test.asuka reads: `hello.txt.gz`
(`gzip -k -9 hello.txt`: its name and time in the header), `two.gz` (two
members, `gzip -n` each, one after the other), `hello.zlib` and
`hello.deflate` (Python 3's `zlib.compress(data, 9)`, and a
`compressobj` with wbits -15: raw DEFLATE).
