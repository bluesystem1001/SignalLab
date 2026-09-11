# SignalLab

SignalLab 是一个面向课程学习的一维离线信号分析程序。首版计划支持模拟信号、CSV/WAV 导入、时域处理、FFT、特征提取、SQLite 追溯、后台任务和 SignalScript。

“阶段 4：SQLite 持久化与追溯”已完成工程实现与验证，当前停在用户验收点，阶段 5 尚未开始。程序版本为 `0.4.0`：除了阶段 3 的同步分析能力，还会把信号、分析任务、特征结果、参数和哈希保存到 SQLite，并在重启后恢复和查询。

## 当前可以看到什么

运行程序后，主窗口会显示：

- 48 kHz 采样率、0.02 秒、幅值 1.0 的 1 kHz 正弦信号。
- 左侧项目列表，以及所选信号的名称、采样率、样本数和持续时间。
- CSV 和 WAV 导入按钮、导入前预览、重复逻辑信号提示。
- 由 Qt Charts 绘制的时域波形；大信号显示前会按像素桶保留最小值和最大值。
- “分析所选信号”按钮，以及均值、总体方差、RMS、峰峰值、主频、SNR 和 THD。
- 时域波形与单边幅度谱两个页签；大频谱同样会按像素宽度限制绘图点数并保留峰值。
- “历史任务”页签，可查看任务状态、算法版本、规范化参数 JSON、输入/输出哈希、UTC 时间和特征结果。
- 重启后从数据库恢复内置信号和已导入信号，不再只保存在本次运行的内存中。

核心层会拒绝无效采样率、空样本和 NaN/无穷大样本。算法层还会校验窗、FFT 长度、频带、峰值、FIR 和质量指标参数，并拒绝运算后产生的非有限结果。新导入的原始文件按 SHA-256 内容哈希保存为项目相对路径 `data/imports/<sha256>.source`；旧版本已存在的 `<sha256>.csv/.wav` 存档会保持原名并继续复用。SQLite 数据库位于 `data/signallab.sqlite3`。这些都是运行数据，不会提交到 Git。

阶段 4 的 Debug 与 Release 均已自动发现并通过 127/127 项测试，GUI 两次启动/重启与 SQLite 完整性、迁移幂等和内置信号幂等检查也已通过。阶段版本已备份并停在本节人工验收点，不会提前进入阶段 5。

## 同步分析

阶段 3 的“分析所选信号”按以下顺序工作：

```text
Signal -> 基本时域特征
       -> 去直流 -> Hann 窗 -> 自动补零 -> 自研 radix-2 FFT
       -> 单边幅值/功率谱 -> 峰值 -> SNR/THD -> 频谱图
```

非 2 次幂长度会补零到下一个 2 次幂。幅值按窗的相干增益校正；主峰再用三点对数抛物线插值细化。SNR 把基频保护频点之外的全部非 DC 功率（包括谐波）视为噪声，THD 则只累计第 2 次到指定最高次数的谐波功率。准确公式、FIR 边界策略、复杂度、版本字符串和 Release 基准见 [算法说明](docs/algorithms.md)。

分析当前仍有意在 GUI 线程中同步执行，便于先看清完整调用链；阶段 5 才会把它移到可取消的后台任务。阶段 4 为这条同步调用链增加了持久化：分析开始时创建 `RUNNING` 任务，成功时在一个事务中写入全部特征并改为 `SUCCEEDED`；任何特征写入失败都会回滚，任务最终记为 `FAILED`。归一化、截取以及 FIR 低通/高通已经作为独立算法接口提供，当前 GUI 只自动执行去直流和默认 Hann 频谱流程。

### 阶段 4 人工验收

1. 按本文 Debug 命令启动程序，确认窗口标题为 `SignalLab 0.4.0`，左侧能看到内置 1 kHz 信号。
2. 导入 `data/samples/example_signal.csv`：选择逗号、有表头、数值列 2、时间列 1，然后分析该信号。
3. 打开“历史任务”页签并选中最新任务，确认状态显示为“成功”（数据库状态为 `SUCCEEDED`），且算法名/版本、参数 JSON、输入/输出哈希、UTC 时间和特征结果都不是空白。
4. 正常关闭并重新启动程序，确认已导入信号仍在左侧列表中，刚才的历史任务仍可查询。
5. 再次导入同一个 CSV，但改用“不选时间列、显式填写 1000 Hz”，仍使用数值列 2。确认它能作为第二个逻辑信号导入，但 `data/imports/` 中不会多出第二份相同内容的原始文件。
6. 用与第 5 步完全相同的选项再导入一次，确认程序给出重复提示，且信号列表和存档文件数量不变。

人工界面无法稳定模拟“事务中途写入失败”和“上次运行遗留 `RUNNING` 任务”；这两条由自动化测试注入失败并检查回滚/启动中断语义。

### 阶段 3 人工验收

1. 启动程序，在左侧选择“内置 1 kHz 正弦信号”，单击“分析所选信号”。
2. 确认时域特征约为：均值 0、总体方差 0.5、RMS 0.707、峰峰值 2。
3. 切换到“单边频谱”，确认主要峰值位于约 1 kHz，图表横轴为 Hz、纵轴为幅值。
4. 切回“时域波形”，再分析 `data/samples/example_signal.csv` 等导入信号，确认特征和频谱随所选信号变化。
5. 确认状态栏显示 FFT 点数和峰值数量；没有可用主峰时，SNR/THD 显示“不可计算”，而不是让程序退出。

## 导入文件

CSV 首版支持：

- 逗号或制表符分隔。
- 有表头或无表头。
- 指定从 1 开始的数值列编号。
- 使用单位为秒、严格递增且近似等间隔的时间列推导采样率，或直接填写采样率。

仓库中的 `data/samples/example_signal.csv` 可以直接用于练习：选择逗号、表头、数值列 2、时间列 1。确认预览后，它会出现在左侧列表中。

WAV 首版只支持 PCM 16-bit 的单声道或双声道文件。双声道可选择左声道、右声道或左右平均。格式错误会显示源文件和字节位置；CSV 内容错误会显示源文件及从 1 开始的行、列位置。

阶段 2 的两个历史限制已在阶段 4 移除。原始文件仍按 SHA-256 只存档一份，但“文件版本”和“逻辑信号”已分开：同一 CSV 可按不同列/采样率选项导入，同一双声道 WAV 也可按左、右或平均声道生成不同逻辑信号。只有“文件内容 + 解析选项”都相同时才视为重复。信号列表现在会从 SQLite 恢复。

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
