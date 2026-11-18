# std/archive's fixtures

Archives other tools made, which archive_test.asuka reads (TZ=UTC,
COPYFILE_DISABLE=1, every file's time 2024-01-02 15:30:44 UTC):

- `tools.zip`: Info-ZIP's zip 3.0 (macOS's), `zip -r -y` of a directory
  with a file, an executable, a symlink (`-y`), an empty directory, a
  path past 100 bytes and a name in UTF-8; `zip64.zip`: the same with
  `-fz` (zip64's records).
- `pax.tar.gz`, `gnu.tar.gz`, `ustar.tar`: bsdtar 3.7 (`--format pax -z`,
  `--format gnutar`, then `gzip -n -9`, `--format ustar`) of the same,
  `--no-mac-metadata`.
- Python 3's zipfile and tarfile, made to be hostile or odd:
  `slip.zip` and `slip.tar.gz` (an entry `../evil.txt`), `abs.zip`
  (`/abs.txt`), `backslash.zip` (`..\evil.txt`), `linkout.tar.gz` (a link
  `out` to `../..`, then `out/evil.txt`), `linkin.tar.gz` (a link `l` to
  `d`, then `l/b.txt`), `liar.zip` (its headers say 10 bytes; it
  inflates to 100000), `badcrc.zip` (a CRC-32 that doesn't match),
  `dos.zip` (an MS-DOS time, 2001-02-03 04:05:06, and no extended
  timestamp; the name `caf\x82.txt` in code page 437, without the UTF-8
  flag).
