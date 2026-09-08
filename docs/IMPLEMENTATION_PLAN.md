# SignalLab 分阶段实施计划

> 文档状态：实施基线草案  
> 编制日期：2026-09-02  
> 最近同步：2026-09-08（阶段 3 已完成工程验证与备份，等待用户验收后再启动阶段 4）
> 依据：`SignalLab_智能信号分析平台_课程大作业项目设计文档.docx`

## 当前实施进度

- 阶段 0：已完成并备份。
- 阶段 1：已完成并备份。
- 阶段 2：已完成、已验收并备份。
- 阶段 3：代码、文档、Debug/Release 构建与测试均已完成；当前停在用户验收点。
- 阶段 4—8：尚未开始，不得在阶段 3 验收前提前实施。

## 1. 目标与实施原则

首版只完成一维、等间隔采样、离线信号分析。MVP 的完整闭环是：

`CSV/WAV/模拟信号 → 校验与导入 → Signal → 预处理 → FFT/特征 → 图表 → SQLite → 导出`

同时满足以下课程展示要求：

- 自研 FFT、窗函数、FIR、峰值/特征算法。
- 自研任务队列、线程池、取消令牌和任务状态机。
- 自研 Lexer、递归下降 Parser、AST、语义检查与解释执行。
- GUI、脚本调用同一套应用服务和算法实现。
- 每次分析保存输入哈希、算法版本、规范化参数和执行结果。
- 至少 30 个自动化测试，能在干净环境一键构建与执行。

首版明确不做：实时采集、多通道同步、深度学习、网络协作、动态插件和复杂报告系统。这些功能不得阻塞 MVP。

## 2. 开始前先固定的技术决定

### 2.1 C++ 标准

项目统一采用 **C++20**：

- 根 CMake 设置 `CMAKE_CXX_STANDARD 20`、`CMAKE_CXX_STANDARD_REQUIRED ON` 和 `CMAKE_CXX_EXTENSIONS OFF`。
- 所有自研库通过 `target_compile_features(... PUBLIC cxx_std_20)` 传播标准要求。
- `Signal::samples()` 可以直接返回 `std::span<const double>`，底层缓冲区仍由不可变 `std::vector<double>` 或共享只读缓冲区持有。
- 可使用 `std::jthread`、`std::stop_token`、`std::numbers` 和 `std::bit_ceil`，但要统一项目线程取消模型，不能同时维护两套取消令牌。
- 公共接口不得返回指向临时容器的 `std::span`；拥有缓冲区的对象必须覆盖视图的完整生命周期。

### 2.2 平台、IDE 与编译器

- 主验收平台：Windows 10/11 x64。
- IDE：CLion 2026.1.4，安装于 `E:\CLion\CLion 2026.1.4`；项目模型只使用 CMake，不依赖 `.idea` 文件。
- 工具链：MinGW-w64 x86_64 GCC，由 MSYS2 管理。
- MSYS2 环境：固定使用 **MINGW64**，安装根目录为 `E:\MSYS2`，工具链前缀为 `E:\MSYS2\mingw64`。
- 构建工具：使用 CLion 自带的 CMake + Ninja；编译器使用 MSYS2 MINGW64 GCC，调试器使用经批准的 CLion 自带 GDB 16.3。
- 构建类型：日常开发使用 Debug；性能报告和答辩包必须使用 Release。

MSYS2 已将 `MINGW64` 环境列为弃用，但本项目根据既有开发环境固定使用它，并在 8 周开发周期内保持工具链不变。所有库必须来自同一个 MINGW64 仓库前缀；同一个构建中绝不能混用 UCRT64、CLion Bundled MinGW、MSVC 或其他 MinGW 安装的对象文件和静态库。

### 2.3 CLion 工具链配置

在 `Settings | Build, Execution, Deployment | Toolchains` 中新建 MinGW 或 System 工具链，明确填写：

| 项目 | 固定路径 |
|---|---|
| Toolset / Environment | `E:\MSYS2\mingw64` |
| C Compiler | `E:\MSYS2\mingw64\bin\gcc.exe` |
| C++ Compiler | `E:\MSYS2\mingw64\bin\g++.exe` |
| Debugger | `E:\CLion\CLion 2026.1.4\bin\gdb\win\x64\bin\gdb.exe` |
| CMake | `E:\CLion\CLion 2026.1.4\bin\cmake\win\x64\bin\cmake.exe` |
| Build Tool | `E:\CLion\CLion 2026.1.4\bin\ninja\win\x64\ninja.exe` |

随后在 CLion 的 CMake Profile 中选择该工具链，CMake 选择 Bundled 或上表中的明确路径，生成器使用 CLion 自带的 Windows x64 Ninja，构建目录使用 `cmake-build-debug-mingw64` 和 `cmake-build-release-mingw64`。可以使用 CLion 自带的 CMake/Ninja，但不要选择 CLion 自带的 Bundled MinGW，否则会与 `E:\MSYS2\mingw64` 中的 Qt/GoogleTest 产生 ABI 或路径混用。

**已批准偏差（2026-09-04）**：用户批准 CLion Toolchain 仅在 Debugger 一项使用上述 CLion 自带 GDB 16.3。C/C++ 编译器、链接器、头文件及全部 C/C++ 库仍固定来自 MSYS2 MINGW64；这不代表可以使用 CLion Bundled MinGW。详细边界和回退方案见 `docs/adr/0005-clion-bundled-gdb.md`。

### 2.4 架构边界

- `core`、`algorithm`、`task`、`parser` 不包含 Qt GUI、Qt SQL 或平台专用 API。
- `app` 只做用例编排，依赖仓库抽象，不直接写 SQL。
- `io`、`database` 是基础设施实现，可以使用 Qt Core/Qt SQL。
- `gui` 只收集输入、展示状态，通过应用服务提交工作。
- Parser 只生成并检查 AST；脚本执行适配器放在 `app/script_runtime`，避免 `app ↔ parser` 循环依赖。
- 线程之间只传任务消息、不可变对象或移动后的结果，不共享可写 `Signal`。

## 3. 工具链与库版本基线

以下是截至 2026-09-02 建议锁定的开发基线，不要求业务代码追随每一个最新版本。

| 类别 | 推荐版本 | 用途与约束 |
|---|---:|---|
| C++ | C++20 | `CMAKE_CXX_STANDARD 20`，关闭 GNU 扩展 |
| CLion | 2026.1.4 | 安装于 `E:\CLion\CLion 2026.1.4`，用于代码编写、CMake Profile、运行与 GDB 调试 |
| MSYS2 | 现有 MINGW64 环境 | 安装于 `E:\MSYS2`；外部库与工具统一由 `pacman` 管理 |
| MinGW-w64 GCC | 16.1.0 | MINGW64 x86_64 主编译器；本机实测版本，以环境快照为准 |
| GDB | 16.3 | CLion 自带版本，固定使用 `E:\CLion\CLion 2026.1.4\bin\gdb\win\x64\bin\gdb.exe` |
| Qt | 6.11.1-1 | 阶段 0 实际冻结的 `qt6-base` 包，提供 Core、Widgets、Sql、QSQLITE 和部署工具 |
| Qt Charts | 6.11.1-1 | 阶段 0 实际冻结版本；QWidget 图表中不封装业务逻辑 |
| SQLite | 3.53.2-1 | 阶段 0 实际冻结的 MSYS2 包，由 Qt/QSQLITE 使用；应用不再手工链接第二份 SQLite |
| CMake | 4.2.2 | CLion 自带版本；“CMake 4.22”按 4.2.2 记录，项目最低要求仍为 3.28 |
| CTest | 随 CMake | 统一发现并运行单元、集成测试 |
| GoogleTest | 1.17.0-1 | 使用已安装的 MSYS2 MINGW64 `gtest` 包，仅测试目标依赖 |
| Ninja | 1.13.2 | 使用 CLion 自带的 Windows x64 Ninja，不使用 Cygwin 版本 |
| Git | 2.54.0.1-1 | 实际安装的 `mingw-w64-x86_64-git` 包；用于功能分支、里程碑标签、代码量和 AI 使用记录 |

### 3.1 本机环境预检（2026-09-02）

| 检查项 | 本机结果 | 与计划的关系 |
|---|---|---|
| `E:\MSYS2\mingw64\bin\g++.exe --version` | GCC 16.1.0 | 可作为本项目编译器基线 |
| `E:\MSYS2\mingw64\bin\gdb.exe --version` | GDB 17.2 | 已安装但不作为本项目所选调试器 |
| CLion 自带 `gdb.exe --version` | GDB 16.3 | 2026-09-04 经用户批准，作为本项目所选调试器 |
| CLion 自带 `cmake.exe --version` | CMake 4.2.2 | 满足计划，可直接使用 |
| CLion 自带 Windows x64 `ninja.exe --version` | Ninja 1.13.2 | 满足计划，可直接使用；不要选同目录下的 Cygwin 版本 |
| MINGW64 Qt 6、Qt Charts | 6.11.1-1 | 已安装，作为阶段 0 实际冻结版本 |
| MINGW64 GoogleTest | 1.17.0-1 | 已安装，满足计划 |
| MINGW64 SQLite | 包版本 3.53.2-1 | 已安装；项目通过 Qt/QSQLITE 使用，命令行版本差异详见环境快照 |
| MINGW64 Git | 2.54.0.1-1 | 已安装，满足版本备份与里程碑要求 |

CLion 自带的 CMake 4.2.2、Ninja 1.13.2 和 GDB 16.3 已满足当前计划。Windows `PATH` 中还能找到 STM32 工具附带的 CMake 3.28.1，因此构建和测试命令必须使用本文给出的 CLion 绝对路径，不能依赖系统 `PATH`。MSYS2 GDB 17.2 保留为环境现状但不被选用；不得为了匹配旧基线盲目降级。用户批准的决定、验证要求和回退方式见 ADR 0005。

依赖策略：

- 所有 C/C++ 外部库通过 MSYS2 MINGW64 仓库安装；CMake/Ninja 使用 CLion 自带版本。不使用 Qt 官方安装器、CLion Bundled MinGW、vcpkg、Conan 或 CMake `FetchContent` 混装依赖。
- 第 1 周完成所需包安装和版本核对后冻结环境 8 周。开发期间不临时升级工具链；确需升级时必须先备份包清单，执行 MSYS2 完整升级，清空构建目录并运行全量测试，不能做局部升级。
- 安装本项目依赖：

```bash
pacman -S --needed \
  mingw-w64-x86_64-qt6-base \
  mingw-w64-x86_64-qt6-charts \
  mingw-w64-x86_64-gtest \
  mingw-w64-x86_64-sqlite3 \
  mingw-w64-x86_64-git
```

- CMake 使用 `find_package(Qt6 6.11 REQUIRED COMPONENTS Core Widgets Charts Sql)` 和 `find_package(GTest CONFIG REQUIRED)`；测试目标链接 `GTest::gtest_main`，并通过 `include(GoogleTest)`、`gtest_discover_tests(signallab_tests)` 注册到 CTest。CLion 使用指定工具链后从 `E:\MSYS2\mingw64`/`/mingw64` 前缀查找依赖。
- 第 1 周及每个里程碑分别执行 CLion 自带 `cmake.exe --version`、Windows x64 `ninja.exe --version`、所选 CLion `gdb.exe --version`、MINGW64 `g++.exe --version` 和 `pacman -Q`，把结果保存到 `docs/msys2-package-snapshot.txt`。快照同时记录未选用的 MSYS2 GDB，避免隐藏环境差异。所有可执行文件都使用本节列出的明确路径，8 周内基线以该快照为准。
- CSV、PCM16 WAV、FFT、FIR、峰值、线程池和 Parser 首版自行实现，不引入替代课程核心的第三方库。
- JSON、SHA-256 和数据库访问放在基础设施边界，分别使用 Qt Core 的 JSON、`QCryptographicHash` 和 Qt SQL。
- 首版不引入 FFTW、QtConcurrent、Boost、spdlog、nlohmann/json、ANTLR/Bison 或机器学习框架。

注意：Qt Charts 的开源许可包含 GPLv3；课程源码交付通常可以满足其要求，但发布方式仍应在 `THIRD_PARTY_NOTICES.md` 中记录。若项目必须闭源分发，应在编码前重新确认 Qt 商业许可或替换绘图库，不能在答辩前临时更换。

## 4. 建议文件结构

目录先按模块建立，源文件可随阶段逐步加入，不要第一天创建所有空文件。

```text
SignalLab/
├─ CMakeLists.txt
├─ CMakePresets.json
├─ CMakeUserPresets.json.example
├─ README.md
├─ LICENSE
├─ THIRD_PARTY_NOTICES.md
│
├─ cmake/
│  ├─ Dependencies.cmake
│  ├─ CompilerWarnings.cmake
│  └─ Sanitizers.cmake
│
├─ include/signallab/
│  ├─ core/
│  │  ├─ error.hpp
│  │  ├─ result.hpp
│  │  ├─ signal.hpp
│  │  ├─ spectrum.hpp
│  │  └─ feature_set.hpp
│  ├─ algorithm/
│  │  ├─ algorithm_registry.hpp
│  │  ├─ fft.hpp
│  │  ├─ window.hpp
│  │  ├─ fir.hpp
│  │  ├─ peak_detector.hpp
│  │  └─ feature_extractor.hpp
│  ├─ task/
│  │  ├─ cancellation_token.hpp
│  │  ├─ task.hpp
│  │  ├─ task_queue.hpp
│  │  ├─ thread_pool.hpp
│  │  └─ scheduler.hpp
│  ├─ parser/
│  │  ├─ source_location.hpp
│  │  ├─ token.hpp
│  │  ├─ lexer.hpp
│  │  ├─ ast.hpp
│  │  ├─ parser.hpp
│  │  └─ semantic_checker.hpp
│  ├─ repository/
│  │  ├─ signal_repository.hpp
│  │  ├─ task_repository.hpp
│  │  ├─ script_repository.hpp
│  │  └─ file_repository.hpp
│  └─ app/
│     ├─ import_service.hpp
│     ├─ analysis_service.hpp
│     ├─ project_service.hpp
│     ├─ script_service.hpp
│     └─ dto.hpp
│
├─ src/
│  ├─ core/
│  │  ├─ signal.cpp
│  │  ├─ spectrum.cpp
│  │  └─ feature_set.cpp
│  ├─ algorithm/
│  │  ├─ algorithm_registry.cpp
│  │  ├─ fft.cpp
│  │  ├─ window.cpp
│  │  ├─ fir.cpp
│  │  ├─ peak_detector.cpp
│  │  └─ feature_extractor.cpp
│  ├─ task/
│  │  ├─ task_queue.cpp
│  │  ├─ thread_pool.cpp
│  │  └─ scheduler.cpp
│  ├─ parser/
│  │  ├─ lexer.cpp
│  │  ├─ parser.cpp
│  │  └─ semantic_checker.cpp
│  ├─ app/
│  │  ├─ import_service.cpp
│  │  ├─ analysis_service.cpp
│  │  ├─ project_service.cpp
│  │  ├─ script_service.cpp
│  │  └─ script_runtime/
│  │     ├─ evaluator.hpp
│  │     └─ evaluator.cpp
│  ├─ io/
│  │  ├─ csv_reader.hpp
│  │  ├─ csv_reader.cpp
│  │  ├─ wav_reader.hpp
│  │  ├─ wav_reader.cpp
│  │  ├─ export_service.hpp
│  │  ├─ export_service.cpp
│  │  ├─ hash_service.hpp
│  │  └─ hash_service.cpp
│  ├─ database/
│  │  ├─ database_connection.hpp
│  │  ├─ database_connection.cpp
│  │  ├─ migration_runner.hpp
│  │  ├─ migration_runner.cpp
│  │  ├─ sqlite_signal_repository.cpp
│  │  ├─ sqlite_task_repository.cpp
│  │  └─ sqlite_script_repository.cpp
│  └─ gui/
│     ├─ main.cpp
│     ├─ main_window.hpp
│     ├─ main_window.cpp
│     ├─ models/
│     │  ├─ signal_list_model.hpp
│     │  ├─ task_list_model.hpp
│     │  └─ result_model.hpp
│     ├─ widgets/
│     │  ├─ signal_chart.hpp
│     │  ├─ spectrum_chart.hpp
│     │  ├─ parameter_panel.hpp
│     │  ├─ task_panel.hpp
│     │  └─ script_console.hpp
│     └─ adapters/
│        └─ qt_event_bridge.hpp
│
├─ migrations/
│  ├─ 001_init.sql
│  └─ 002_add_indexes.sql
├─ config/
│  └─ default.json
├─ resources/
│  ├─ signallab.qrc
│  └─ icons/
├─ data/
│  └─ samples/
│     ├─ README.md
│     └─ expected/
├─ tests/
│  ├─ CMakeLists.txt
│  ├─ unit/
│  │  ├─ core/
│  │  ├─ algorithm/
│  │  ├─ task/
│  │  └─ parser/
│  ├─ integration/
│  │  ├─ import_analysis_flow_test.cpp
│  │  ├─ database_transaction_test.cpp
│  │  └─ script_execution_test.cpp
│  └─ fixtures/
├─ benchmarks/
│  ├─ fft_benchmark.cpp
│  └─ import_benchmark.cpp
├─ tools/
│  └─ generate_samples.cpp
├─ packaging/
│  └─ windows/
├─ docs/
│  ├─ IMPLEMENTATION_PLAN.md
│  ├─ msys2-package-snapshot.txt
│  ├─ requirements.md
│  ├─ architecture.md
│  ├─ database.md
│  ├─ test-plan.md
│  ├─ user-guide.md
│  └─ ai-log.md
└─ .idea/                       # CLion 本地配置，加入 .gitignore，不提交
```

### 4.1 CMake 目标建议

| 目标 | 类型 | 允许依赖 |
|---|---|---|
| `signallab_core` | 静态库 | C++ 标准库 |
| `signallab_algorithm` | 静态库 | `signallab_core` |
| `signallab_task` | 静态库 | `signallab_core`、线程库 |
| `signallab_parser` | 静态库 | `signallab_core` |
| `signallab_app` | 静态库 | core、algorithm、task、parser、repository 抽象 |
| `signallab_io` | 静态库 | core、Qt6::Core |
| `signallab_database` | 静态库 | repository 抽象、Qt6::Core、Qt6::Sql |
| `signallab_gui` | 静态库 | app、Qt6::Core、Qt6::Widgets、Qt6::Charts |
| `SignalLab` | 可执行程序 | gui、io、database；在 `main.cpp` 完成依赖装配 |
| `signallab_tests` | 测试集合 | 被测模块、GTest::gtest_main |

构建脚本必须阻止反向依赖。例如 `algorithm` 不能链接 `Qt6::Widgets`，`gui` 不能直接包含 `sqlite_*.hpp`。

**阶段 2 的临时边界**：为保持首个文件导入纵向切片简单、便于初学者跟踪，`signallab_app` 中的 `ImportService` 暂时私有链接 `signallab_io`。GUI 仍只依赖应用服务；阶段 4 引入 repository 抽象时必须移除此例外并恢复上表的最终依赖方向。见 [ADR 0006](adr/0006-stage2-import-service-boundary.md)。

## 5. 分阶段开发路线

每个阶段必须形成“能构建、能运行、能测试”的增量；验收未通过时不进入下一阶段。

8 周总排期如下：

| 周次 | 重点 | 必须完成的里程碑 |
|---:|---|---|
| 第 1 周 | 环境、骨架、Signal、模拟信号、基础图表 | CLion 可构建运行，形成第一个纵向切片 |
| 第 2 周 | CSV/WAV 导入、校验、哈希、信号管理 | 文件导入后可显示波形 |
| 第 3 周 | 窗函数、FFT、FIR、峰值与特征 | 完成同步分析闭环和算法基准 |
| 第 4 周 | SQLite、迁移、仓库和事务 | 分析记录可持久化、重启和追溯 |
| 第 5 周 | 队列、线程池、状态机、取消 | 三任务并发且 GUI 保持响应 |
| 第 6 周 | Lexer、Parser、AST、语义检查、Evaluator | 脚本可完成分析闭环 |
| 第 7 周 | 完整 GUI、交互、下采样和导出 | 功能冻结，演示流程可重复运行 |
| 第 8 周 | 测试、性能、打包、文档和答辩 | 只修缺陷，形成 V1.0 发布包 |

排期约束：第 7 周结束后停止增加功能；实时采集、机器学习、网络和插件全部移出 8 周范围。若某阶段延期，优先减少界面装饰和扩展特征，不能删除 FFT、并发取消、SQLite 追溯或 Parser 这四类课程核心证据。

### 阶段 0：冻结范围并建立工程骨架（第 1 周第 1—2 天）

目标：从干净目录能够构建一个空窗口并运行一个测试。

主要工作：

- 核对 `E:\MSYS2\mingw64` 环境，并用 `pacman` 安装 Qt、GoogleTest、SQLite 等依赖。
- 在 CLion 中配置混合工具链：编译器指向 `E:\MSYS2\mingw64`，GDB/CMake/Ninja 指向 `E:\CLion\CLion 2026.1.4` 内的上述自带工具。
- 建立根目录、CMake targets、Debug/Release presets。
- 通过 `find_package` 接入 MSYS2 中的 Qt 和 GoogleTest，使用 `gtest_discover_tests` 开启 CTest 自动发现。
- 配置 C++20、GCC 警告、UTF-8、输出目录和版本信息。
- 写 `README.md` 的安装、配置、构建、测试四条命令。
- 建立 `docs/ai-log.md`、`docs/msys2-package-snapshot.txt`、`.gitignore` 和第三方许可证记录。

首批实际文件只需要：

```text
CMakeLists.txt
CMakePresets.json
CMakeUserPresets.json.example
cmake/Dependencies.cmake
cmake/CompilerWarnings.cmake
src/gui/main.cpp
src/gui/main_window.{hpp,cpp}
tests/CMakeLists.txt
tests/unit/smoke_test.cpp
README.md
```

验收门槛：

- CLion Toolchain 检测到 `E:\MSYS2\mingw64` 的 GCC 16.1.0、经批准的 CLion 自带 GDB 16.3、CMake 4.2.2 和 Windows x64 Ninja 1.13.2；能够命中断点并查看局部变量和调用栈；未使用 Bundled MinGW 或 Cygwin Ninja。
- `cmake --preset dev-mingw64` 成功。
- `cmake --build --preset dev-mingw64` 成功并能打开空窗口。
- `ctest --preset dev-mingw64` 至少通过 1 个 smoke test。
- 仓库中没有提交 `build/`、Qt DLL、数据库运行文件或 IDE 临时文件。

### 阶段 1：最小纵向切片（第 1 周第 3—7 天）

目标：先打通“模拟正弦 → Signal → 波形显示 → 自动测试”，验证架构而不是堆空接口。

主要工作：

- 实现 `Error`、`Result<T>`、`SignalMetadata` 和不可变 `Signal`。
- 实现正弦/双音模拟数据生成器。
- 主窗口显示一条小规模时域曲线和基本元数据。
- 明确采样率大于 0、样本无 NaN/Inf 等不变量。
- GUI 只拿只读数据，不自己计算持续时间或坐标轴。

验收门槛：

- 固定参数可生成 1 kHz 正弦信号。
- 界面显示波形、采样率、样本数和持续时间。
- 不变量、持续时间和模拟器至少有 5 个单元测试。
- `signallab_core` 能在不链接 Qt Widgets 的情况下单独测试。

### 阶段 2：文件导入与信号管理（第 2 周）

目标：完成 CSV 和 PCM16 WAV 导入，不产生半成品记录。

主要工作：

- 先实现 `ImportPreview`，再实现确认后的完整导入。
- CSV 支持逗号/制表符、表头/无表头、时间列/数值列和行列错误定位。
- WAV 首版支持 PCM 16-bit；立体声提供左、右、平均三种选择。
- 使用 `std::filesystem::path` 和项目相对路径。
- 使用 SHA-256 检测重复文件；写出采用临时文件 + 原子重命名。
- 百万点显示前按像素桶取 min/max，下采样只影响显示。

验收门槛：

- 正常 CSV、异常 CSV、正常 WAV、截断 WAV 均有测试。
- 错误包含文件、行/列或字节位置和可理解说明。
- 导入后能在项目浏览器中选择信号并显示波形。
- 重复导入同一文件得到明确提示，不重复复制原始文件。

### 阶段 3：核心算法闭环（第 3 周）

目标：同步方式先跑通“预处理 → FFT → 特征 → 图表”，暂不急着并发。

实现顺序：

1. 去直流、归一化、截取。
2. Rect/Hann/Hamming 窗及相干增益。
3. 自研 radix-2 FFT、单边幅度谱和功率谱。
4. 均值、方差、RMS、峰峰值和频带能量。
5. 峰值检测、最小间距抑制和三点抛物线插值。
6. FIR 低通/高通、边界策略。
7. 明确定义后的 SNR 和 THD。

验收门槛：

- 48 kHz、65,536 点、1 kHz 正弦的主峰误差不超过 `fs/N`，幅值误差不超过 2%。
- 常量、冲激、双音、白噪声、非 2 次幂和空输入均有测试。
- 每个算法有参数校验、复杂度说明和版本字符串。
- NumPy/MATLAB 结果只作为离线固化参考，不成为运行依赖。
- Release 模式记录 FFT 基准环境和耗时。

### 阶段 4：SQLite 持久化与追溯（第 4 周）

目标：分析结果可重启查询、可复现，失败时事务完整回滚。

主要工作：

- 完成 `001_init.sql`、迁移表和迁移执行器。
- 实现 `signal_source`、`signal_record`、`analysis_task`、`feature_result`、`file_version`、`script_history`、`operation_log`。
- 打开连接后设置 `foreign_keys=ON`、WAL 和 busy timeout。
- 所有 SQL 使用预编译与参数绑定。
- 保存算法名/版本、规范化参数 JSON、输入/输出哈希和 UTC 时间。
- 每条 Qt SQL 连接只在所属线程创建、使用和销毁。

验收门槛：

- 导入注册和分析结果写入各自是原子事务。
- 注入写入失败后无孤立 `feature_result`，任务为 FAILED。
- 重启后能查询历史任务并展示参数、哈希和版本。
- 启动时把遗留 RUNNING 任务标为 INTERRUPTED。

### 阶段 5：多线程任务系统（第 5 周）

目标：把阶段 3 的同步分析封装成可排队、取消、重试的后台任务。

主要工作：

- 实现 `TaskQueue`、固定大小 `ThreadPool`、`Scheduler` 和任务状态机。
- 计算线程数默认 `max(2, hardware_concurrency - 2)`，同时允许配置覆盖。
- FFT 每个 stage、FIR 每批样本、文件每个块检查取消令牌。
- 数据库保持单写线程；I/O 使用 1—2 个工作线程。
- 进度更新最多约每 50 ms 一次，通过 Qt queued signal 回到 UI。
- 实现固定退出顺序和统一锁顺序。

验收门槛：

- 同时提交 10 个任务、取消其中 3 个，最终状态合法且无半成品结果。
- 同时运行至少 3 个大任务时，窗口仍能缩放和切换信号。
- 取消请求目标在 200 ms 内被工作线程感知。
- 运行中关闭程序可取消、唤醒、join、刷盘并正常退出，无死锁。

### 阶段 6：SignalScript（第 6 周）

目标：脚本和 GUI 复用相同应用服务，完成可演示的编译前端。

实现顺序：

1. `SourceLocation`、Token 和 Lexer。
2. 表达式、调用、赋值和管道的 AST。
3. 递归下降 Parser 与优先级爬升。
4. 错误恢复到下一个分号。
5. 符号表、类型检查和参数范围检查。
6. Evaluator 到应用服务的映射。
7. `load`、`lowpass`、`fft`、`features`、`plot`、`save`。

验收门槛：

- 合法脚本生成预期 AST，结果与同参数 GUI 操作一致。
- 未闭合字符串、非法字符、缺括号、未知变量和类型错误都有行列信息。
- 脚本不能执行系统命令、网络访问或任意文件代码。
- 脚本源码、哈希、状态和错误位置写入 `script_history`。

### 阶段 7：GUI 完整集成（第 7 周）

目标：形成设计稿中的主窗口、项目浏览器、图表、参数/结果、任务中心和脚本控制台。

主要工作：

- 使用 Model/View 管理信号、任务和结果，窗口不持有数据库连接。
- 图表支持时域/频域切换、缩放、游标、局部选择和图片导出。
- 参数提交前即时校验；结果卡显示单位、参数和算法版本。
- 失败状态显示错误原因、位置和可执行建议。
- 除颜色外使用图标和文字表达任务状态。

验收门槛：

- 新用户能在 3 分钟内完成导入、FFT、查看主频和导出。
- 百万点原始数据不直接全部提交给 Qt Charts。
- UI 线程中不存在全量文件读取、FFT、FIR 或 SQL 写入。
- MVP 演示流程可连续重复两次。

### 阶段 8：测试、打包与答辩冻结（第 8 周）

目标：从“能运行”提升到“可重复验收”。

主要工作：

- 补齐单元、集成、系统、性能、并发和回归测试。
- 做 30 分钟稳定性运行及故障注入。
- 使用 Release 构建记录 65,536 点 FFT 和 10 MB CSV 指标。
- 用 `E:\MSYS2\mingw64\bin\windeployqt6.exe` 制作 Windows 包，检查 Qt DLL、MinGW 运行库以及 platforms、sqldrivers 和 imageformats 插件。
- 固化样例、空白数据库、配置、期望结果和演示脚本。
- 同步设计、数据库、测试、用户手册、AI 记录和第三方许可证。
- 建立 `v1.0.0-rc1`，冻结功能后只修缺陷。

验收门槛：

- 所有“必须”需求有代码、测试或演示证据。
- 自动化测试不少于 30 个，核心模块覆盖率目标不低于 70%。
- 干净机器无需 IDE 即可运行发布包。
- 数据库、配置、样例和文档与 V1.0 代码一致。
- 准备 2—3 分钟无剪辑录屏作为现场备份。

## 6. 推荐测试分配

不要等到最后一次性补测试。建议至少覆盖：

| 模块 | 最低测试数 | 重点 |
|---|---:|---|
| core | 5 | 不变量、持续时间、不可变语义、错误传播 |
| algorithm | 12 | FFT、窗、FIR、峰值、特征、边界和误差容限 |
| io | 6 | CSV/WAV 正常、损坏、重复和路径异常 |
| database | 4 | 迁移、事务回滚、索引、重启恢复 |
| task | 6 | 状态转移、并发、取消、停止和异常 |
| parser | 10 | Token、优先级、AST、恢复、类型和错误位置 |
| integration | 4 | 导入→分析→存储、脚本→任务、失败回滚、重启查询 |

合计最低 47 个测试，给“至少 30 个”留出删减和平台差异余量。

## 7. 需求到阶段的追踪

| 需求 | 首次完成阶段 | 最终验收阶段 |
|---|---:|---:|
| FR-01 信号导入 | 2 | 8 |
| FR-02 信号管理 | 2/4 | 7 |
| FR-03 时域处理 | 3 | 7 |
| FR-04 频域分析 | 3 | 8 |
| FR-05 特征提取 | 3 | 8 |
| FR-06 任务管理 | 5 | 8 |
| FR-07 数据持久化 | 4 | 8 |
| FR-08 脚本控制 | 6 | 8 |
| FR-09 可视化 | 1/3 | 7 |
| FR-10 导出 | 2/4 | 8 |

## 8. 第一天和第一周的实际行动

### 第一天

1. 安装 CLion 与 MSYS2，打开 **MSYS2 MINGW64** 终端，确认其根目录为 `E:\MSYS2`。
2. 核对 `E:\MSYS2\mingw64\bin` 中的 `g++.exe`，并核对 `E:\CLion\CLion 2026.1.4` 中的 GDB 16.3、CMake 4.2.2 与 Windows x64 Ninja 1.13.2。
3. 在 MINGW64 环境中安装本文列出的 Qt、GoogleTest、SQLite 等依赖；版本满足验收目标后立即保存 `pacman -Q` 快照并冻结环境，不在开发中途执行无计划升级。
4. 在 CLion 中新建混合工具链：GCC 使用 MSYS2 MINGW64，GDB/CMake/Ninja 使用本文列出的 CLion 自带工具；确认没有选择 Bundled MinGW、MSVC、UCRT64 或 Cygwin Ninja。
5. 创建根 `CMakeLists.txt`、`CMakePresets.json` 和一个空窗口。
6. 通过 `find_package` 接入 Qt/GoogleTest，建立第一个 smoke test，并提交 `chore: bootstrap clion msys2 and tests`。

### 第一周结束前

1. 完成阶段 0 的全部验收门槛。
2. 写下 5 份短 ADR：C++20、MSYS2 MINGW64、QSQLITE、Qt Charts，以及经用户批准的 CLion GDB 偏差。
3. 固化一个小型正弦样例及生成参数。
4. 把构建和测试命令放进 README，并在一个全新构建目录复验。
5. 保存 `pacman -Q` 环境快照并创建里程碑 `v0.1`；之后开始实现 `Signal`，不要先写数据库或脚本界面。

## 9. 关键风险检查

- **C++20 标准漂移**：所有目标统一声明 `cxx_std_20`，不得只在 CLion Profile 中追加 `-std=c++20`。
- **`std::span` 生命周期**：返回的视图不能超过底层不可变缓冲区生命周期。
- **工具链混用**：CLion Bundled MinGW、MSVC、MSYS2 MINGW64 和 UCRT64 的对象文件/静态库不能混用。
- **MSYS2 滚动更新**：不做局部更新；里程碑前保存包快照，完整更新后清空构建目录并全量测试。
- **Qt Charts 性能**：从阶段 2 开始就使用显示下采样，不能拖到 GUI 集成末期。
- **SQLite 线程归属**：连接在所属线程内创建和销毁，连接对象不能跨线程传递。
- **SQLite 双重链接**：使用 QSQLITE 时不再链接独立 `sqlite3`，避免版本和符号混乱。
- **Parser/App 循环依赖**：Parser 只产出 AST；Evaluator 作为应用层适配器。
- **任务成功过早**：必须在算法成功且数据库事务提交后才能置为 SUCCEEDED。
- **算法结果不可解释**：SNR、THD、窗幅值校正和边界策略必须保存参数与定义。
- **范围膨胀**：阶段 8 前不接实时采集、ML、网络或插件。

## 10. 完成定义

一个阶段只有同时满足以下条件才算完成：

- Debug 与 Release 均可构建。
- 新增行为有自动化测试且全量测试通过。
- 错误路径不会留下半成品文件或数据库记录。
- 跨线程对象所有权和退出行为已说明。
- 用户可见行为同步到文档。
- 形成一次可回退的小提交或里程碑标签。

整个 MVP 完成的标志是：在离线 Windows 环境中，从 CSV/WAV 导入开始，能够通过 GUI 或 SignalScript 完成分析、并发取消、结果追溯和导出，并以源码位置、测试记录和可重复演示共同证明。

## 11. 版本信息来源

- [CLion Windows 工具链配置](https://www.jetbrains.com/help/clion/quick-tutorial-on-configuring-clion-on-windows.html)
- [CLion 的 Qt/CMake 项目配置](https://www.jetbrains.com/help/clion/qt-tutorial.html)
- [MSYS2 环境及 MINGW64/UCRT64 区别](https://www.msys2.org/docs/environments/)
- [MSYS2 包命名规则](https://www.msys2.org/docs/package-naming/)
- [MSYS2 MINGW64 GCC 包](https://packages.msys2.org/packages/mingw-w64-x86_64-gcc)
- [MSYS2 MINGW64 Qt 6 Base 包](https://packages.msys2.org/packages/mingw-w64-x86_64-qt6-base)
- [MSYS2 MINGW64 Qt Charts 包](https://packages.msys2.org/packages/mingw-w64-x86_64-qt6-charts)
- [MSYS2 MINGW64 GoogleTest 包](https://packages.msys2.org/packages/mingw-w64-x86_64-gtest)
- [MSYS2 MINGW64 SQLite 包](https://packages.msys2.org/packages/mingw-w64-x86_64-sqlite3)
- [Qt SQL 与 QSQLITE 驱动说明](https://doc.qt.io/qt-6/sql-driver.html)
