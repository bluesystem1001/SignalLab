# SignalLab

SignalLab 是一个面向课程学习的一维离线信号分析程序。首版计划支持模拟信号、CSV/WAV 导入、时域处理、FFT、特征提取、SQLite 追溯、后台任务和 SignalScript。

“阶段 2：文件导入与信号管理”已经实现。程序启动后会生成一个固定的 1 kHz 正弦信号，也可以预览并导入 CSV 或 PCM16 WAV；后续功能将继续严格按照阶段计划逐步加入。

## 当前可以看到什么

运行程序后，主窗口会显示：

- 48 kHz 采样率、0.02 秒、幅值 1.0 的 1 kHz 正弦信号。
- 左侧项目列表，以及所选信号的名称、采样率、样本数和持续时间。
- CSV 和 WAV 导入按钮、导入前预览、重复文件提示。
- 由 Qt Charts 绘制的时域波形；大信号显示前会按像素桶保留最小值和最大值。

核心层会拒绝无效采样率、空样本和 NaN/无穷大样本。模拟器还会检查时长、幅值和奈奎斯特频率。导入成功的原始文件按 SHA-256 内容哈希保存到项目相对目录 `data/imports/`；该运行目录不会提交到 Git。

当前 Debug 与 Release 都自动发现并通过 41 项测试。除顺序执行外，两种配置还通过了 `-j 8` 并行测试复验。

## 导入文件

CSV 首版支持：

- 逗号或制表符分隔。
- 有表头或无表头。
- 指定从 1 开始的数值列编号。
- 使用单位为秒、严格递增且近似等间隔的时间列推导采样率，或直接填写采样率。

仓库中的 `data/samples/example_signal.csv` 可以直接用于练习：选择逗号、表头、数值列 2、时间列 1。确认预览后，它会出现在左侧列表中。

WAV 首版只支持 PCM 16-bit 的单声道或双声道文件。双声道可选择左声道、右声道或左右平均。格式错误会显示源文件和字节位置；CSV 内容错误会显示源文件及从 1 开始的行、列位置。

阶段 2 有两个有意保留的范围限制：信号列表仅存在于本次运行中，重启恢复会在阶段 4 随 SQLite 持久化实现；同一内容的原始文件按 SHA-256 去重，因此一个 CSV/WAV 在本阶段只能采用一组列或声道选项。若要比较不同选项，请修改原始文件内容后另存，或等待后续阶段把“原始文件”和“逻辑信号记录”分离。

### 阶段 2 人工验收

1. 启动程序，确认左侧有内置正弦信号且右侧显示波形。
2. 导入 `data/samples/example_signal.csv`，按“逗号、表头、数值列 2、时间列 1”填写，确认预览为 21 个样本、1000 Hz。
3. 在左侧切换内置信号和导入信号，确认名称、采样率、样本数及波形随选择变化。
4. 再次导入同一 CSV，确认出现重复提示，并检查 `data/imports/` 中没有第二份原始文件或 `.tmp` 文件。
5. 如手边有 PCM16 单/双声道 WAV，再验证预览及左/右/平均选择；WAV 格式和截断分支已由自动化测试覆盖，不要求为此准备课程外部数据。

## 开发环境

本项目固定使用下面这套 Windows 工具链。开发期间不要混用其他 MinGW、MSVC 或 UCRT64 文件。

| 项目 | 当前冻结值 |
|---|---|
| 操作系统 | Windows 10/11 x64 |
| C++ 标准 | C++20 |
| C/C++ 编译器 | MSYS2 MINGW64 GCC 16.1.0 |
| Qt / Qt Charts | MSYS2 MINGW64 6.11.1-1 |
| GoogleTest | MSYS2 MINGW64 1.17.0-1 |
| SQLite | MSYS2 MINGW64 包 3.53.2-1，通过 QSQLITE 使用 |
| CMake / CTest | CLion 自带 4.2.2 |
| Ninja | CLion 自带 Windows x64 1.13.2 |
| 调试器 | CLion 自带 GDB 16.3 |

调试器是唯一经批准的工具链偏差：编译器和所有 C/C++ 库仍来自 `E:\MSYS2\mingw64`。详情见 [ADR 0005](docs/adr/0005-clion-bundled-gdb.md)。

## 第一次使用

以下命令都在 PowerShell 中执行。路径含有空格，所以命令前面的 `&` 不能省略。

### 1. 进入项目目录

```powershell
Set-Location -LiteralPath 'E:\programme\project_c++\SignalLab'
```

### 2. 检查工具是否存在

逐条执行：

```powershell
& 'E:\MSYS2\mingw64\bin\g++.exe' --version
& 'E:\CLion\CLion 2026.1.4\bin\gdb\win\x64\bin\gdb.exe' --version
& 'E:\CLion\CLion 2026.1.4\bin\cmake\win\x64\bin\cmake.exe' --version
& 'E:\CLion\CLion 2026.1.4\bin\ninja\win\x64\ninja.exe' --version
```

检查项目依赖包：

```powershell
& 'E:\MSYS2\usr\bin\pacman.exe' -Q mingw-w64-x86_64-qt6-base mingw-w64-x86_64-qt6-charts mingw-w64-x86_64-gtest mingw-w64-x86_64-sqlite3 mingw-w64-x86_64-git
```

如果任一命令提示文件或包缺失，请先停止配置和构建，补全环境后再重新检查。

需要在一台新机器上安装这些包时，可在 PowerShell 中执行：

```powershell
& 'E:\MSYS2\usr\bin\pacman.exe' -S --needed mingw-w64-x86_64-qt6-base mingw-w64-x86_64-qt6-charts mingw-w64-x86_64-gtest mingw-w64-x86_64-sqlite3 mingw-w64-x86_64-git
```

不要为了本项目执行局部升级，也不要通过 Qt 官方安装器、vcpkg、Conan 或 CMake `FetchContent` 再安装第二套依赖。

### 3. 配置 CLion Toolchain

打开 `Settings | Build, Execution, Deployment | Toolchains`，按下表填写：

| CLion 项目 | 路径 |
|---|---|
| Toolset / Environment | `E:\MSYS2\mingw64` |
| C Compiler | `E:\MSYS2\mingw64\bin\gcc.exe` |
| C++ Compiler | `E:\MSYS2\mingw64\bin\g++.exe` |
| Debugger | `E:\CLion\CLion 2026.1.4\bin\gdb\win\x64\bin\gdb.exe` |
| CMake | `E:\CLion\CLion 2026.1.4\bin\cmake\win\x64\bin\cmake.exe` |
| Build Tool | `E:\CLion\CLion 2026.1.4\bin\ninja\win\x64\ninja.exe` |

这里使用的是 CLion 自带的 GDB，不是 CLion Bundled MinGW。不要把 C/C++ Compiler 改为 CLion 或其他目录中的编译器。

## Debug：配置、构建、测试和运行

先进入项目目录，然后依次执行四条命令。

配置：

```powershell
& 'E:\CLion\CLion 2026.1.4\bin\cmake\win\x64\bin\cmake.exe' --preset dev-mingw64
```

构建：

```powershell
& 'E:\CLion\CLion 2026.1.4\bin\cmake\win\x64\bin\cmake.exe' --build --preset dev-mingw64
```

测试：

```powershell
& 'E:\CLion\CLion 2026.1.4\bin\cmake\win\x64\bin\ctest.exe' --preset dev-mingw64 --output-on-failure
```

运行：

```powershell
$env:PATH = 'E:\MSYS2\mingw64\bin;' + $env:PATH
& '.\cmake-build-debug-mingw64\bin\SignalLab.exe'
```

这里把 MINGW64 放到本次 PowerShell 会话的 `PATH` 首位，避免系统中其他 MinGW 运行库被程序误加载。

## Release：配置、构建、测试和运行

性能记录和最终发布包必须使用 Release。

```powershell
& 'E:\CLion\CLion 2026.1.4\bin\cmake\win\x64\bin\cmake.exe' --preset release-mingw64
& 'E:\CLion\CLion 2026.1.4\bin\cmake\win\x64\bin\cmake.exe' --build --preset release-mingw64
& 'E:\CLion\CLion 2026.1.4\bin\cmake\win\x64\bin\ctest.exe' --preset release-mingw64 --output-on-failure
$env:PATH = 'E:\MSYS2\mingw64\bin;' + $env:PATH
& '.\cmake-build-release-mingw64\bin\SignalLab.exe'
```

## 给初学者的修改约定

- 一次只完成当前阶段，不提前创建大量空类或空文件。
- 优先使用清楚的变量名、普通函数和直接的控制流程。
- 只有确实减少重复时才增加模板、宏或复杂抽象。
- GUI 只收集输入和显示结果，计算逻辑放入相应模块。
- 修改行为时同时增加或更新测试。
- 不要编辑生成目录 `cmake-build-*` 中的文件。

工程决策记录位于 `docs/adr/`，环境版本位于 `docs/msys2-package-snapshot.txt`，完整阶段计划见 `docs/IMPLEMENTATION_PLAN.md`。

## 阶段备份

每个阶段只有在 Debug/Release 均能构建、全量测试通过并同步文档后才算完成。完成后创建一个小提交和一个清晰的阶段标签，例如 `stage-0-complete`，以便初学者能够安全回退。
