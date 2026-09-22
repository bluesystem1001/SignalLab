# ADR 0006：阶段 2 导入服务边界

- 状态：已执行并关闭（阶段 4 退出条件已满足）
- 日期：2026-09-08
- 关闭日期：2026-09-11
- 最近复核：2026-09-22（阶段 5 后台导入与阶段 6 SignalScript 仍保持该边界）

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

## 关闭记录与后续复核

### 阶段 4：退出临时边界

- `signallab_repository` 已提供 `FileRepository`、`SignalRepository`、`TaskRepository` 和 `ScriptRepository` 抽象。
- `ImportService` 只接收 `FileRepository` 与 `SignalRepository`，`AnalysisService` 只接收 `TaskRepository` 与 `SignalRepository`；`signallab_app` 不再链接 `signallab_io` 或 `signallab_database`。
- `signallab_io` 提供文件系统实现，`signallab_database` 提供 SQLite 实现，`main.cpp` 在启动时创建并注入这些对象。
- 信号列表现在可从 SQLite 恢复。文件内容哈希与解析选项共同标识逻辑信号，因此同一原始文件可以用不同列或声道选项产生多个信号，而原始内容仍只存档一份。

### 阶段 5：后台导入复核

- `BackgroundFileService` 只组合 `ImportService` 与 `Scheduler`，把预览和导入送入 I/O 队列；它不直接依赖 CSV/WAV 读取器、文件系统仓库或 SQLite 实现。
- 慢速工作通过 `ImportService::loadCsv/loadWav` 返回 `SignalEntry`，不在工作线程修改 GUI 使用的 `entries()`；`MainWindow` 在 GUI 线程消费完成事件并调用 `acceptImportedSignal()`。
- `ImportService` 串行保护“内容寻址存档—数据库注册—失败补偿”边界，避免同一实例中的并发导入互相删除或重复注册存档。

### 阶段 6：脚本复用复核

- SignalScript 通过 `ImportService::openCsv/openWav` 复用同一导入用例。相同内容与规范化解析选项会返回既有持久化信号；同一路径内容改变时才建立新的文件版本和逻辑信号。
- `openCsv/openWav` 与后台加载入口一样不修改 `entries()`，所以脚本工作线程不会越过应用服务去操作 GUI 项目模型。
- 脚本文件权限由 repository 层的 `ScriptFileGateway` 抽象表达，文件系统实现仍位于 `signallab_io`；解释器没有获得原始文件系统、数据库或 Qt GUI 对象。
- 当前 `signallab_app` 的 CMake 依赖为 `core`、`algorithm`、`repository`、`parser` 和 `task`，没有重新链接 `signallab_io` 或 `signallab_database`。

对应自动化测试覆盖内容寻址存档复用、不同解析选项、恢复与失败补偿、后台工作线程到 GUI 线程的交接，以及脚本重复加载和输入内容变化。2026-09-22 的最终复核中，Debug 与 Release 均无警告构建成功并通过 216/216 项测试。上述代码边界持续满足退出条件，因此本 ADR 的临时例外保持关闭；后续新增基础设施能力也必须先通过 repository 抽象进入 `app`，不能以本 ADR 为依据恢复直接依赖。
