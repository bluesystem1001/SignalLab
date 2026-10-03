# Third-Party Notices

This document records the third-party components used to build and run SignalLab on the frozen MSYS2 MINGW64 toolchain. Exact license texts copied from the installed packages are under `licenses/third-party/`; `licenses/SHA256SUMS.txt` records their bytes.

SignalLab's own source notice is in `LICENSE`. It does not replace or restrict any third-party license.

## Distribution boundary

The current GUI links the open-source build of **Qt Charts 6.11.1**, whose installed package supplies `GPL-3.0-only.txt` (or a Qt commercial license). No project-wide open-source license has been selected by this repository. Therefore a Windows archive may be created and tested locally, but it must not be publicly redistributed until the copyright holder has either:

1. granted a project license compatible with GPL-3.0-only and met the corresponding source-distribution obligations; or
2. obtained and documented a suitable commercial Qt license.

This is a release decision, not something that `windeployqt6` or the packaging script can decide automatically.

## Direct runtime dependencies

| Component | Frozen MSYS2 package | Purpose | License source in this repository |
|---|---|---|---|
| Qt 6 Base | `mingw-w64-x86_64-qt6-base 6.11.1-1` | Core, Widgets, GUI, SQL, OpenGL support, deployment tool and plugins | `licenses/third-party/qt6-base/` |
| Qt Charts | `mingw-w64-x86_64-qt6-charts 6.11.1-1` | Time-domain and frequency-domain charts | `licenses/third-party/qt6-charts/` |
| SQLite | `mingw-w64-x86_64-sqlite3 3.53.2-1` | QSQLITE runtime backend | `licenses/third-party/sqlite3/` |
| GCC runtime | `mingw-w64-x86_64-gcc-libs 16.1.0-5` | `libgcc_s_seh-1.dll` and `libstdc++-6.dll` | `licenses/third-party/gcc-libs/` |
| MinGW-w64 runtime | `mingw-w64-x86_64-libwinpthread 14.0.0.r92.g818fa6510-1` plus matching CRT/headers | Win32 C/C++ runtime support and `libwinpthread-1.dll` | `licenses/third-party/libwinpthread/`, `crt/`, `headers/`, `winpthreads/` |

The application uses SQLite only through Qt's QSQLITE driver and does not link a second application-owned SQLite implementation.

## Transitive runtime dependencies

The frozen Qt/MSYS2 DLL closure can also include the components below. The Windows package verifier determines the actual DLL closure; keeping their installed license texts here prevents the license payload from depending on which image-format or platform plugins `windeployqt6` selects on a particular run.

| Installed component | Frozen package version | License directory |
|---|---:|---|
| libb2 | `0.98.1-3` | `licenses/third-party/libb2/` |
| Brotli | `1.2.0-1` | `licenses/third-party/brotli/` |
| bzip2 | `1.0.8-3` | `licenses/third-party/bzip2/` |
| double-conversion | `3.4.0-1` | `licenses/third-party/double-conversion/` |
| FreeType | `2.14.3-1` | `licenses/third-party/freetype/` |
| gettext runtime | `1.0-1` | `licenses/third-party/gettext-runtime/` |
| GLib | `2.88.1-1` | `licenses/third-party/glib2/` |
| Graphite2 | `1.3.15-1` | `licenses/third-party/graphite2/` |
| HarfBuzz | `14.2.1-1` | `licenses/third-party/harfbuzz/` |
| ICU | `78.3-2` | `licenses/third-party/icu/` |
| libiconv | `1.19-1` | `licenses/third-party/libiconv/` |
| libjpeg-turbo | `3.1.4.1-3` | `licenses/third-party/libjpeg-turbo/` |
| libpng | `1.6.58-1` | `licenses/third-party/libpng/` |
| md4c | `0.5.3-1` | `licenses/third-party/md4c/` |
| PCRE2 | `10.47-1` | `licenses/third-party/pcre2/` |
| zlib | `1.3.2-2` | `licenses/third-party/zlib/` |
| zstd | `1.5.7-2` | `licenses/third-party/zstd/` |

Windows system DLLs are supplied by the operating system and are not copied into the SignalLab archive.

## Test-only dependency

GoogleTest `mingw-w64-x86_64-gtest 1.17.0-1` is used only by the automated test executables and is not a SignalLab runtime dependency. Its installed license text is copied to `licenses/third-party/gtest/` for source-delivery completeness.

## Development tools

CLion, CMake, CTest, Ninja, GDB, Git, PowerShell and the `sqlite3` command-line program are build, test, or asset-generation tools. They are not copied into the application package. Their frozen versions are recorded in `docs/msys2-package-snapshot.txt`.

## Provenance and verification

`tools/freeze_stage8_assets.ps1` copies license files from the installed package locations without rewriting them. The exact source paths and package list are documented in `licenses/README.md`. Run `tools/verify_stage8_assets.ps1` before packaging to verify every copied file against `licenses/SHA256SUMS.txt`.
