# ADR 0005：使用 CLion 自带 GDB 16.3

- 状态：已接受
- 日期：2026-09-04
- 批准者：项目用户

## 背景

实施计划要求 GDB 16.3，但当前 MSYS2 MINGW64 的 `E:\MSYS2\mingw64\bin\gdb.exe` 为 17.2。

CLion 2026.1.4 自带的下列调试器实测为 GDB 16.3：

```text
E:\CLion\CLion 2026.1.4\bin\gdb\win\x64\bin\gdb.exe
```

用户于 2026-09-04 批准使用该调试器。

## 决定

只把 CLion Toolchain 的 Debugger 设置为上述 GDB 16.3。以下内容保持原计划不变：

- C 编译器：`E:\MSYS2\mingw64\bin\gcc.exe`
- C++ 编译器：`E:\MSYS2\mingw64\bin\g++.exe`
- Qt、GoogleTest、SQLite：全部来自 MSYS2 MINGW64
- CMake、Ninja：使用 CLion 2026.1.4 自带版本
- 不使用 CLion Bundled MinGW 的编译器、链接器、头文件或库

## 边界

CLion 自带 GDB 只用于本地调试：

- 不参与编译或链接。
- 不写入 CMake 配置。
- 不复制到源码仓库或发布包。
- PowerShell 构建和 CTest 不依赖该 GDB。

## 验证

- 在环境快照中记录完整路径和 `--version` 输出。
- Debug 构建中能够命中断点、查看局部变量和调用栈。
- Debug、Release 构建及全部测试均通过。

## 结果与回退

这是对实施计划中“调试器来自 MINGW64”的最小偏差，不改变任何编译或链接 ABI。若出现调试兼容问题，不静默切换版本；先记录问题，再重新评估并验证其他 GDB。
