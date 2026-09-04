# ADR 0002：固定使用 MSYS2 MINGW64

- 状态：已接受
- 日期：2026-09-04

## 背景

Qt、GoogleTest 和 SQLite 的二进制文件必须与编译器兼容。混用多个 MinGW、MSVC 或 UCRT64 容易产生链接错误和难以解释的运行问题。

## 决定

- 编译器固定为 `E:\MSYS2\mingw64\bin\g++.exe`，当前 GCC 版本为 16.1.0。
- Qt、GoogleTest、SQLite 及其他 C/C++ 库只使用 `mingw-w64-x86_64-*` 包。
- CMake 4.2.2 和 Windows x64 Ninja 1.13.2 使用 CLion 2026.1.4 自带版本。
- 环境冻结到本项目 8 周开发结束；不做无计划的局部升级。
- 每个里程碑保存一次 `pacman -Q` 快照。

MINGW64 已被 MSYS2 标为弃用，但本课程项目优先保持现有环境稳定，不在开发中途迁移到 UCRT64。

## 不允许的组合

同一次构建不得使用 CLion Bundled MinGW、MSVC、UCRT64、其他 MinGW 安装或它们产生的对象文件和静态库。也不通过 vcpkg、Conan 或 `FetchContent` 混装依赖。

## 已批准的例外

调试器采用 CLion 自带 GDB 16.3。GDB 不参与编译和链接，因此该例外不改变 MINGW64 的 ABI 边界。详情见 [ADR 0005](0005-clion-bundled-gdb.md)。

## 结果

固定来源让初学者只需检查一组路径。代价是不能随意升级单个包；确需升级时要先备份包清单、完整升级、重新配置并运行全量测试。
