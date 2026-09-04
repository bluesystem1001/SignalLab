# Third-Party Notices

本文件记录 SignalLab 使用的第三方组件。版本来自 2026-09-04 的 MSYS2 MINGW64 环境快照。最终发布包只应包含实际需要的运行文件，并应同时附带对应组件要求的许可证文本。

## 程序依赖

### Qt 6 Base

- MSYS2 包：`mingw-w64-x86_64-qt6-base 6.11.1-1`
- 用途：Qt Core、Qt Widgets、Qt SQL、QSQLITE 和 Windows 部署工具
- 许可证：Qt 商业许可证或适用的开源许可证；以安装包内许可证文件和 Qt 官方说明为准
- 项目地址：<https://www.qt.io/>

### Qt Charts

- MSYS2 包：`mingw-w64-x86_64-qt6-charts 6.11.1-1`
- 用途：时域和频域图表
- 许可证注意事项：实施计划按 GPLv3 约束记录。若需要闭源分发，必须在编码和发布前重新确认商业许可或选择替代绘图库
- 项目地址：<https://doc.qt.io/qt-6/qtcharts-index.html>

### SQLite

- MSYS2 包：`mingw-w64-x86_64-sqlite3 3.53.2-1`
- 用途：由 Qt 的 QSQLITE 驱动访问项目数据库
- 使用约束：应用不再直接链接第二份 SQLite
- 许可证：Public Domain
- 项目地址：<https://www.sqlite.org/>

### MinGW-w64 与 GCC 运行库

- MSYS2 GCC 包：`mingw-w64-x86_64-gcc 16.1.0-5`
- 用途：编译程序；阶段 8 的 Windows 发布包可能包含所需运行库
- 许可证：各组件许可证不同，GCC 运行库适用 GCC Runtime Library Exception；打包时必须以实际随包许可证为准
- 项目地址：<https://www.mingw-w64.org/>、<https://gcc.gnu.org/>

## 仅用于测试

### GoogleTest

- MSYS2 包：`mingw-w64-x86_64-gtest 1.17.0-1`
- 用途：自动化测试，不是 SignalLab 运行时依赖
- 许可证：BSD 3-Clause
- 项目地址：<https://github.com/google/googletest>

## 仅用于开发

CLion、CMake、CTest、Ninja、GDB 和 Git 是开发工具，不应被复制进 SignalLab 发布包。它们的实际版本记录在 `docs/msys2-package-snapshot.txt`。

本仓库当前不复制上述项目的第三方源代码。若以后加入新的依赖，应在引入依赖的同一个提交中更新本文件。
