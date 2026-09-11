# ADR 0006：阶段 2 导入服务边界

- 状态：已执行并关闭（阶段 4 退出条件已满足）
- 日期：2026-09-08
- 关闭日期：2026-09-11

## 背景

最终架构要求 `app` 依赖仓库抽象，由 `io` 和 `database` 提供基础设施实现。阶段 2 需要先用尽量直接的代码跑通“预览、确认、哈希、解析、原子存档、列表显示”，而仓库接口和 SQLite 生命周期要到阶段 4 才能完整定义。

## 决定

阶段 2 允许 `signallab_app` 的 `ImportService` 私有链接 `signallab_io`，直接编排 CSV/WAV 读取和 SHA-256。`signallab_gui` 只调用 `ImportService`，不直接解析或复制文件；`core` 继续只依赖 C++ 标准库。

## 原因

- 当前用例的执行顺序可以在一个文件中从上到下阅读，便于初学者修改。
- 不为尚未实现的 SQLite 和任务系统提前建立空接口。
- GUI、解析规则和原子文件操作仍保持分离，后续替换调用边界不需要改动核心算法。

## 影响与退出条件

- 这是阶段 2—3 的临时依赖，不是最终架构的新方向。
- 在临时边界存续期间，信号列表不负责重启恢复，同一原始文件也只能采用一组导入选项。
- 退出条件是：阶段 4 创建 repository 抽象后，让 `app` 只依赖抽象，并由 `SignalLab` 可执行程序装配 `io`/`database` 实现。

## 阶段 4 关闭记录

- `signallab_repository` 已提供 `FileRepository`、`SignalRepository`、`TaskRepository` 和 `ScriptRepository` 抽象。
- `ImportService` 只接收 `FileRepository` 与 `SignalRepository`，`AnalysisService` 只接收 `TaskRepository` 与 `SignalRepository`；`signallab_app` 不再链接 `signallab_io` 或 `signallab_database`。
- `signallab_io` 提供文件系统实现，`signallab_database` 提供 SQLite 实现，`main.cpp` 在启动时创建并注入这些对象。
- 信号列表现在可从 SQLite 恢复。文件内容哈希与解析选项共同标识逻辑信号，因此同一原始文件可以用不同列或声道选项产生多个信号，而原始内容仍只存档一份。

上述代码边界已完成，所以本 ADR 的临时例外不再适用。阶段 4 的 Debug/Release、GUI 启动重启和备份验证也已完成，项目现停在用户验收点。
