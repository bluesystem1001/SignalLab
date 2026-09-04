# SignalLab AI 使用记录

本文件只记录可复核的工作内容、用户决定和验证结果，不记录隐藏推理过程。

## 2026-09-04：阶段 0 启动前检查

- 用户目标：按照 `IMPLEMENTATION_PLAN.md` 分阶段构建程序，语法尽量简单，便于初学者修改；工具链不完整时不得开始。
- AI 协助：只读检查计划、工具路径、版本和 MSYS2 包。
- 检查结果：GCC、CMake、Ninja、Qt、Qt Charts、GoogleTest、SQLite 和 Git 包已找到；MSYS2 GDB 为 17.2，而计划要求 GDB 16.3。
- 用户决定：批准使用 CLion 2026.1.4 自带的 GDB 16.3。
- 决定边界：只替换调试器；GCC、Qt、GoogleTest、SQLite 及其他 C/C++ 文件继续统一来自 MSYS2 MINGW64。
- 决策记录：`docs/adr/0005-clion-bundled-gdb.md`。

## 2026-09-04：阶段 0 工程骨架

- AI 协助：建立 CMake presets、Qt 空窗口、GoogleTest/CTest 冒烟测试、README、第三方组件记录、环境快照和五份短 ADR，并对实施计划做最小同步。
- 初学者约束：README 使用可逐条复制的 PowerShell 命令；CMake 和 CTest 使用绝对路径；工具链例外用单独 ADR 解释。
- 构建验证：Debug 与 Release 配置、构建均成功，编译器为 MSYS2 GCC 16.1.0，生成器为 CLion Windows Ninja 1.13.2。
- 测试验证：Debug 与 Release 均通过 2 个测试，包括基础冒烟测试和 QSQLITE 驱动加载测试。
- 运行验证：Debug 与 Release 窗口标题均为 `SignalLab`，进程保持响应并能正常关闭，退出码为 0。
- 调试验证：CLion 自带 GDB 16.3 在 `main.cpp` 命中断点，并正确读取源码与 DWARF 5 调试信息。

## 2026-09-04：阶段 1 最小纵向切片

- AI 协助：实现 `Error`、`Result<T>`、不可变 `Signal`、单音/双音模拟器和基础时域图表。
- 核心规则：采样率必须为正有限数，样本非空且全部有限；持续时间统一定义为 `N / fs`。
- 界面边界：GUI 只读取 `Signal` 提供的采样时刻、持续时间和样本范围，不重复计算这些值。
- 构建验证：Debug 与 Release 均无警告构建成功。
- 测试验证：两种配置均通过 13/13 个测试，其中 11 个覆盖 Signal 与模拟器，超过阶段要求的至少 5 个。
- 解耦验证：核心测试可执行文件没有 Qt DLL 导入，核心源码没有 Qt 引用。
- 运行验证：Debug 与 Release 都创建 `SignalLab 0.1.0` 窗口，保持响应并以退出码 0 正常关闭。
- 环境复验：完整 `pacman -Q` 仍为 167 项，与阶段 0 快照完全一致。

## 后续记录格式

每次重要 AI 协助追加一小节，并记录：

1. 日期和阶段。
2. 用户目标或批准的决定。
3. AI 修改的文件或提供的分析。
4. 实际执行的验证及结果。
5. 尚未完成或需要人工确认的内容。
