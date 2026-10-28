# Third-party license payload

The files below `third-party/` are copies of license material installed by the frozen MSYS2 MINGW64 packages and the MariaDB backup extension. Full license files preserve their source bytes; the libidn2 notice is the leading license block of its installed header. They are not summaries written by SignalLab.

## Sources

Most sources were copied from:

```text
E:\MSYS2\mingw64\share\licenses\<component>\
```

ICU installs its license at:

```text
E:\MSYS2\mingw64\share\icu\78.3\LICENSE
```

The copied component directories are:

```text
brotli, bzip2, crt, double-conversion, freetype, gcc-libs,
gettext-runtime, glib2, graphite2, gtest, harfbuzz, headers,
libb2, libiconv, libjpeg-turbo, libpng, libwinpthread, md4c,
pcre2, qt6-base, qt6-charts, sqlite3, winpthreads, zlib, zstd
```

The package versions are recorded in `THIRD_PARTY_NOTICES.md` and in `docs/msys2-package-snapshot.txt`. `SHA256SUMS.txt` contains the SHA-256 of every copied license file and its repository-relative path below `licenses/`.

The 2026-10-11 MariaDB backup extension adds `libmariadbclient` and the already installed transitive dependencies `curl`, `libpsl`, `libssh2`, `libunistring`, `nghttp2`, `nghttp3`, `ngtcp2` and `openssl` from the same installed license root. Their versions are listed in `THIRD_PARTY_NOTICES.md`; the historical Stage 8 package snapshot remains unchanged. New checksum entries are appended without replacing existing frozen entries.

The 2026-10-13 switch to native MariaDB Connector/C (`mysql.h`) reuses these same libraries and license files. New packages link `libmariadb.dll` directly and exclude the QMYSQL plugin; Qt SQL remains in use for local SQLite. No copied license bytes or historical license checksums change for this switch.

MSYS2's `libidn2 2.3.8-4` package contains no separate license file. `libidn2/NOTICE` preserves the leading license notice from `E:\MSYS2\mingw64\include\idn2.h`, which offers LGPLv3-or-later or GPLv2-or-later. The corresponding standard license texts are already included as `libunistring/LICENSE.LIB` (LGPLv3), `libunistring/LICENSE` (GPLv3) and `qt6-base/GPL-2.0-or-later.txt` (GPLv2).

## Reproducing the payload

From the project root, with the frozen MSYS2 installation present:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass `
  -File .\tools\freeze_stage8_assets.ps1 -Force

powershell.exe -NoProfile -ExecutionPolicy Bypass `
  -File .\tools\verify_stage8_assets.ps1
```

The freeze command deliberately fails if an expected installed license source is missing. It must not be replaced by license text copied from an unverified web page.

This directory is a compliance payload, not legal advice. The Qt Charts/project-license blocker described in `THIRD_PARTY_NOTICES.md` must be resolved by the copyright holder before public redistribution.
