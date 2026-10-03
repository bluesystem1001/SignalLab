# Third-party license payload

The files below `third-party/` are byte-for-byte copies of license material installed by the frozen MSYS2 MINGW64 packages. They are not summaries written by SignalLab.

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

