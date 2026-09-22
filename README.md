# SignalLab

SignalLab 是一个面向课程学习的一维离线信号分析程序。首版计划支持模拟信号、CSV/WAV 导入、时域处理、FFT、特征提取、SQLite 追溯、后台任务和 SignalScript。

“阶段 4：SQLite 持久化与追溯”和“阶段 5：后台任务与取消”的既有能力已经过回归复核；当前代码版本为 `0.6.0`，阶段 6 已加入 SignalScript 的语言前端、静态检查、受限解释运行时、文件安全网关和可追溯执行历史。现有主窗口仍提供阶段 5 的 CSV/WAV 导入、后台分析、进度、取消和重试；脚本控制台及其 GUI 接线属于阶段 7，当前窗口中尚不可见。

## 当前可以看到什么

运行程序后，主窗口会显示：

- 48 kHz 采样率、0.02 秒、幅值 1.0 的 1 kHz 正弦信号。
- 左侧项目列表，以及所选信号的名称、采样率、样本数和持续时间。
- CSV 和 WAV 导入按钮、导入前预览、重复逻辑信号提示。
- 由 Qt Charts 绘制的时域波形；大信号显示前会按像素桶保留最小值和最大值。
- “分析所选信号”按钮，以及均值、总体方差、RMS、峰峰值、主频、SNR 和 THD。
- 时域波形与单边幅度谱两个页签；大频谱同样会按像素宽度限制绘图点数并保留峰值。
- “历史任务”页签，可查看任务状态、算法版本、规范化参数 JSON、输入/输出哈希、UTC 时间和特征结果。
- “后台任务”页签，可查看本次运行中任务的排队、运行、成功、失败或取消状态，以及进度、错误和重试来源。
- CSV/WAV 预览、确认后的导入以及信号分析均在后台执行；工作线程只投递事件，实际界面更新回到 Qt GUI 线程。
- 重启后从数据库恢复内置信号和已导入信号，不再只保存在本次运行的内存中。

核心层会拒绝无效采样率、空样本和 NaN/无穷大样本。算法层还会校验窗、FFT 长度、频带、峰值、FIR 和质量指标参数，并拒绝运算后产生的非有限结果。新导入的原始文件按 SHA-256 内容哈希保存为项目相对路径 `data/imports/<sha256>.source`；旧版本已存在的 `<sha256>.csv/.wav` 存档会保持原名并继续复用。SQLite 数据库位于 `data/signallab.sqlite3`。这些都是运行数据，不会提交到 Git。

阶段 5 已加入任务队列/线程池、取消与重试、进度通知、后台适配器、数据库专属线程和真实 FFT/大 BLOB 读取中途取消等自动化测试。阶段 5 的 `0.5.0` 基线在 Debug 与 Release 下均无警告构建成功并通过 164/164 项测试；观察者重入、跨线程委托关闭及并发关闭用例重复 100 轮，批量取消和 GUI 运行时用例重复 10 轮，大 BLOB 取消后排空用例也重复 10 轮，均全部通过。真实窗口已完成启动和一次后台分析冒烟检查。阶段 6 完成后，`0.6.0` 在 Debug 与 Release 下均无警告构建成功并通过 216/216 项测试；资源边界、取消、重复加载、非有限 JSON、安全路径和异常历史收尾等关键用例又在 Release 下重复验证。下面的阶段 5 人工清单仍保留给用户确认交互观感。

## SignalScript 0.6

阶段 6 的脚本服务可以解析并执行受限的离线信号流程。仓库示例位于 `data/samples/example_analysis.sig`：

```signalscript
load "data/samples/example_signal.csv" as raw;
clean = raw |> normalize() |> lowpass(cutoff=100, order=63);
spec = clean |> fft(window="hann");
result = features(spec);
plot clean as "Filtered waveform";
plot spec as "One-sided amplitude spectrum";
save result to "example_signal_features.json";
```

语言支持 `load`、赋值、函数调用、`|>` 管道、`plot` 和 `save`，内建函数为 `normalize`、`lowpass`、`fft` 与 `features`。静态类型包括 `Number`、`String`、`Boolean`、`Signal`、`Spectrum`、`FeatureSet` 和 `Unit`。词法、语法、语义与运行时问题都带从 1 开始的行列位置；语法分析会在分号处恢复，以便报告后续独立错误。

脚本不拥有任意文件访问能力。宿主为每个文件网关分别配置输入根和输出根，脚本只能使用根内相对路径；绝对路径、`..`、符号链接/junction 逃逸和覆盖已有输出都会被拒绝。解释器也没有 Shell、子进程、网络或动态加载入口。每次执行在词法分析前以 `RUNNING` 写入 `script_history`，随后只会进入 `SUCCEEDED`、`FAILED` 或 `CANCELLED`；下次启动会把异常遗留的 `RUNNING` 改为 `INTERRUPTED`。记录包含原始脚本、SHA-256、UTC 时间以及可用的错误行列。

完整语法、参数规则、输出格式、安全边界和阶段 7 的 GUI 分工见 [SignalScript 说明](docs/signalscript.md)。

## 后台分析与任务管理

“分析所选信号”的算法顺序保持不变，但现在由后台计算任务执行：

```text
Signal -> 基本时域特征
       -> 去直流 -> Hann 窗 -> 自动补零 -> 自研 radix-2 FFT
       -> 单边幅值/功率谱 -> 峰值 -> SNR/THD -> 频谱图
```

非 2 次幂长度会补零到下一个 2 次幂。幅值按窗的相干增益校正；主峰再用三点对数抛物线插值细化。SNR 把基频保护频点之外的全部非 DC 功率（包括谐波）视为噪声，THD 则只累计第 2 次到指定最高次数的谐波功率。准确公式、FIR 边界策略、复杂度、版本字符串和 Release 基准见 [算法说明](docs/algorithms.md)。

调度器把分析送入计算线程池，把 CSV/WAV 预览和导入送入小型 I/O 线程池。内存任务按 `PENDING -> RUNNING -> SUCCEEDED/FAILED/CANCELLED` 变化；运行中的长循环通过 `std::stop_token` 协作取消。排队任务可直接移出队列，已结束任务可用新编号重试，并记录来源任务编号。成功返回是提交边界，因此提交完成后才到达的取消请求不会把已经成功的任务改写成“已取消”。“后台任务”只记录本次运行的调度状态；可跨重启查询的分析记录仍在“历史任务”页签中。

持久化分析先在 SQLite 创建 `PENDING` 记录，再以条件更新转为 `RUNNING`。成功时，全部特征和 `SUCCEEDED` 状态在一个事务中提交；取消时记为 `CANCELLED`，其他失败记为 `FAILED`。任何特征写入失败都会整体回滚。数据库连接和具体仓库只在数据库专属线程内创建、使用与销毁，GUI、计算线程和 I/O 线程通过同一个串行代理访问它。归一化、截取以及 FIR 低通/高通仍作为独立算法接口提供，当前 GUI 自动执行的是去直流和默认 Hann 频谱流程。

### 阶段 5 人工验收（工程完成后由用户确认）

1. 按本文 Debug 命令启动当前程序，确认窗口标题为 `SignalLab 0.6.0`，左侧能看到内置 1 kHz 信号，并能打开“后台任务”页签。
2. 导入 `data/samples/example_signal.csv`：选择逗号、有表头、数值列 2、时间列 1。确认先出现独立的预览任务，预览成功并确认后才出现导入任务，成功后信号才加入左侧列表。
3. 分析所选信号，确认后台任务经历排队/运行/成功，运行时能显示非倒退的进度；完成后频谱和特征更新，“历史任务”中出现持久化的成功记录。
4. 使用足够大的输入启动耗时任务，在“后台任务”页选中它并取消。确认界面仍能操作，任务最终显示“已取消”；过小任务可能在取消前已经跨过成功提交边界，不适合这一项验收。
5. 选中一个已结束任务并重试，确认生成新的任务编号，并显示原任务编号；非终态任务不能重试。
6. 在有排队或运行任务时关闭窗口，确认程序停止通知、协作取消并等待工作线程后再关闭数据库，进程能正常结束且不崩溃。
7. 重新启动程序，确认已导入信号和分析历史仍可查询；“后台任务”是本次进程内列表，不要求跨重启恢复。

排队取消竞态、观察者重入或跨线程委托关闭、工作线程触发关闭、重复 100% 进度限频、事务中途失败和异常退出遗留 `PENDING/RUNNING` 等边界不适合依赖人工稳定复现，代码中已有对应自动化测试，并已包含在 Debug/Release 的 164/164 通过结果中。

### 阶段 4 回归检查

1. 打开“历史任务”并选中最新分析，确认算法名/版本、参数 JSON、输入/输出哈希、UTC 时间和特征结果均可读取。
2. 对同一个 CSV 改用“不选时间列、显式填写 1000 Hz”再次导入，确认它能成为第二个逻辑信号，但 `data/imports/` 不会多出第二份相同内容的存档。
3. 用完全相同的文件和选项再导入一次，确认出现重复提示，且信号列表和存档数量不变。

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

仓库中的 `data/samples/example_signal.csv` 可以直接用于练习：选择逗号、表头、数值列 2、时间列 1。后台预览完成并确认后，会再排队执行导入；导入成功后它才出现在左侧列表中。

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

两个 configure preset 已固定 `CMAKE_CXX_SCAN_FOR_MODULES=OFF`，以避免当前 GCC/CMake 组合为普通 C++20 源文件生成不可用的模块扫描规则；无需在命令行重复追加该参数。

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
