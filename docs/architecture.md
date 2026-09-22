# SignalLab 架构说明

本文描述 `0.6.0` 当前已经接入的结构。阶段 4 的持久化和阶段 5 的后台任务边界保持不变；阶段 6 在 GUI 之外增加 SignalScript 前端、静态检查、受限解释运行时、脚本文件网关和执行历史。脚本编辑器与控制台仍属于阶段 7，因此当前主窗口只展示既有交互能力。

## 阶段 1 的历史切片

阶段 1 完成时的数据流是：

```text
固定模拟参数 -> 信号生成器 -> Signal -> MainWindow -> Qt Charts
```

- `signallab_core` 保存信号模型和模拟信号生成器，只依赖 C++ 标准库。
- 当时的 `signallab_gui` 直接读取 `Signal`，把已有的数据和范围交给 Qt Widgets/Qt Charts 显示。
- `SignalLab` 负责创建示例信号并启动窗口。
- `signallab_tests` 验证核心规则以及当前基础设施是否可用。

依赖只能沿上面的方向前进。`signallab_core` 不包含 Qt 头文件，也不链接 Qt。

## Signal 的规则

`Signal` 通过工厂函数创建。创建时统一检查：

- 采样率必须是有限数并且大于 0。
- 样本不能为空。
- 每个样本都必须是有限数，不能包含 NaN 或无穷大。

创建成功后，样本只能通过 `std::span<const double>` 读取，没有公开修改函数。`Signal` 自己拥有底层 `std::vector<double>`，因此只要 `Signal` 仍然存在，读取视图就有效。

当前时间定义为：

```text
信号持续时间 = 样本数 / 采样率
第 i 个样本的时间 = i / 采样率
```

GUI 调用这些接口，不重复计算持续时间、样本坐标或样本范围。

## 错误处理

预期内的输入错误使用 `Result<T>` 返回。调用者先判断结果是否成功，再读取值或错误：

```cpp
auto result = makeSomething();

if (!result) {
    showError(result.error());
    return;
}

auto value = std::move(result).value();
```

这种写法让错误路径保持可见，也避免用异常处理普通的参数校验失败。

## 当前模块边界与持久化导入

阶段 4 已把阶段 2 的临时 `app -> io` 依赖移除。当前目标依赖为：

```text
SignalLab（组装根）
├─> signallab_gui -> signallab_app
│                    ├─> signallab_algorithm -> signallab_core
│                    ├─> signallab_parser -> signallab_core
│                    ├─> repository 接口 -> signallab_core
│                    └─> signallab_task -> signallab_core
├─> signallab_io       -> repository 接口 + signallab_core + Qt Core
└─> signallab_database -> repository 接口 + Qt Core + Qt Sql
```

`repository` 目录只声明文件、信号、任务、脚本历史和脚本文件能力的抽象接口。`ImportService`、`AnalysisService` 和 `ScriptService` 只依赖这些接口，不知道 SQLite 语句或文件安全检查的具体实现。`main.cpp` 创建 `FilesystemFileRepository`、`DatabaseExecutor`、`Scheduler` 和两个后台适配服务，再通过构造函数装配现有应用与界面；阶段 6 还在启动时通过数据库代理清理遗留脚本执行。`DatabaseExecutor` 同时实现信号、任务和脚本仓库接口，但把具体 SQLite 仓库隐藏在其专属线程内。`FilesystemScriptFileGateway` 则实现受限的输入根/输出根访问，解释器永远不会获得原始文件系统对象。[ADR 0006](adr/0006-stage2-import-service-boundary.md) 的退出条件因此已满足。

当前文件预览和导入的数据流是：

```text
文件选择与参数 -> BackgroundFileService -> Scheduler I/O 队列
    -> 预览任务 -> ImportService -> FileRepository -> 线程安全事件队列
    -> Qt queued 调用 -> GUI 显示预览并等待确认
    -> 导入任务 -> ImportService -> FileRepository
                                  -> 临时快照 / SHA-256 / 解析
                                  -> data/imports/<sha256>.source
                                  -> DatabaseExecutor -> SignalRepository 事务
    -> 线程安全事件队列 -> Qt queued 调用
    -> GUI 接收 SignalEntry -> 项目列表与波形
```

- `signallab_io` 实现 `FileRepository`，负责 CSV、PCM16 WAV、SHA-256、内容寻址存档和文件格式错误定位。
- `signallab_database` 实现 `SignalRepository`，把 `file_version`、`signal_source` 和 `signal_record` 在一个事务中注册。其中任何一步失败都会回滚。
- 新存档固定使用 `<sha256>.source`，位置使用项目相对路径；旧版本已有的 `<sha256>.csv/.wav` 会保持原名并继续复用，避免数据库中的旧路径失效。`sourcePath` 只记录导入时的来源位置，重启恢复使用项目内的存档。
- 导入先写同目录临时快照，再原子重命名。如果后续数据库注册失败，应用服务会在确认该存档未被其他记录引用后清理新文件。
- 存档提升与数据库注册是带补偿的组合操作，不是跨文件系统事务。`ImportService` 串行执行这段边界，避免两个 I/O 工作者同时导入相同内容时，一个任务误删另一个任务即将注册的存档。
- 文件 SHA-256 与规范化解析选项共同形成逻辑信号键。因此原始内容只存档一份，同一 CSV/WAV 却可用不同列或声道产生多个逻辑信号；只有内容和解析选项都相同时才拒绝重复导入。
- 启动时，`ImportService::restoreSignals()` 通过数据库执行器读取全部信号，并先验证导入存档的相对路径和内容哈希，再重建界面列表。启动恢复在窗口显示前完成，不经过后台调度器。

CSV 支持逗号或制表符、可选表头、指定时间列/数值列，以及没有时间列时显式填写采样率。若使用时间列，其单位必须是秒，且解析器要求时间严格递增、近似等间隔。CSV 错误使用从 1 开始的行号和列号。

WAV 首版只接受 RIFF/WAVE 中的 PCM 16-bit 单声道或双声道。双声道可选择左、右或平均值，样本统一除以 `32768.0` 转换为归一化浮点值。WAV 错误包含字节偏移。

## 算法闭环

`signallab_algorithm` 是只依赖 `signallab_core` 与 C++20 标准库的静态库。它包含预处理、窗函数、自研 radix-2 FFT、单边频谱、基本特征、峰值检测、FIR 和质量指标；不包含 Qt，也不直接负责界面状态。

GUI 只把所选记录编号交给后台分析适配器，当前数据流是：

```text
所选 signalRecordId -> BackgroundAnalysisService -> Scheduler 计算队列
    -> AnalysisService -> DatabaseExecutor -> SignalRepository::findByRecordId()
                       -> 可信 Signal 与 sampleHash
                       -> 原始样本的基本时域特征
                       -> 去直流 -> Hann 窗 -> bit_ceil 补零
                       -> radix-2 FFT -> 单边幅值/功率谱
                       -> 峰值检测 -> SNR/THD
                       -> 持久化结果
    -> 线程安全事件队列 -> Qt queued 调用
    -> GUI 线程下采样显示 -> Qt Charts
```

基本特征使用原始 `Signal`，避免去直流改变用户看到的均值；频谱路径先去直流，减少 DC 对峰值查找的影响。默认峰值阈值是最强非 DC 幅值的 1%，最多返回 10 个峰。若没有可用主峰，或当前信号无法按已定义口径计算质量指标，`dominantPeak`、`snrDb` 或 `thdPercent` 保持为空，GUI 显示“不可计算”。非法分析选项会直接返回失败。

归一化、截取和 FIR 低通/高通已经是独立公开算法，但阶段 3 GUI 没有为它们加入复杂参数面板。这样既保留课程算法实现，也让首个闭环保持直观。所有公式、边界、复杂度和算法版本见 [算法说明](algorithms.md)。

## SQLite 追溯闭环

`DatabaseExecutor` 启动专属线程，在该线程内打开 `data/signallab.sqlite3`，再按顺序执行 `migrations/` 中尚未记录的 SQL 迁移。`001_init.sql` 建立初始业务表；`002_task_scheduler.sql` 在保留既有任务编号、历史和特征的前提下扩展任务状态约束，加入 `CANCELLED`；`003_signal_script.sql` 为脚本历史增加源码哈希、错误行列和阶段 6 终态约束。`schema_migration` 由迁移执行器管理。连接开启外键、WAL 和 5000 ms busy timeout。

当前分析与追溯数据流是：

```text
后台分析工作者传入所选 signalRecordId
    -> AnalysisService 经 DatabaseExecutor 加载可信 Signal + sampleHash
    -> TaskRepository 再核对记录编号/输入哈希并创建 PENDING 记录
    -> 条件更新 PENDING -> RUNNING
    -> AnalysisService 执行可协作取消的算法
       ├─ 成功 -> 特征结果 + 输出哈希 + SUCCEEDED（一个事务）
       ├─ 取消 -> CANCELLED + 原因
       └─ 其他失败 -> FAILED + 错误文本
    -> 历史任务页签
```

- 参数 JSON 使用固定键顺序、无多余空白和与区域无关的数字格式，便于重现同一次分析。
- 任务保存算法名/版本、参数 JSON、输入/输出 SHA-256 和 UTC 时间。输出哈希由稳定排序后的特征数据计算，不受调用者传入顺序影响。
- 特征写入和任务转为 `SUCCEEDED` 必须一起提交。中途任一 SQL 失败时，整组特征回滚，然后用新事务把任务标为 `FAILED`，不留孤立 `feature_result`。
- 启动时先把上次异常终止后遗留的 `PENDING` 或 `RUNNING` 分析任务批量改为 `INTERRUPTED`，再恢复信号并打开界面。
- “历史任务”页签通过 `AnalysisService` 读取仓库，展示状态、算法版本、参数、哈希、UTC 时间、错误与特征值；GUI 不包含 SQL。
- `QSqlDatabase` 连接、迁移器和具体仓库都在数据库专属线程中创建和销毁。来自 GUI、计算池或 I/O 池的仓库调用通过命令队列串行执行，不会跨线程传递 Qt SQL 连接。

调度器中的文件任务和分析任务拥有进程内编号；只有 `AnalysisService` 创建的分析记录会持久化到 `analysis_task`。因此“后台任务”的重试来源和文件任务不会在重启后恢复，“历史任务”则可以跨重启查询。

## 阶段 5 的调度、通知与关闭

`signallab_task` 只依赖核心层和 C++20 标准库。`Scheduler` 分别拥有计算池与 I/O 池：计算池默认保留两个硬件线程给界面和系统，且至少使用两个工作线程；I/O 池默认一个线程并限制为一到两个。两个队列相互独立，文件读取不会占住计算任务的队列位置。

- 调度状态为 `PENDING -> RUNNING -> SUCCEEDED/FAILED/CANCELLED`。任务报告的进度不能倒退，默认限制通知频率；第一次达到 100% 可立即通知，紧随其后的重复 100% 报告仍受限频约束。
- 取消是协作式的。排队任务会从队列移除；运行任务收到 `std::stop_token`，CSV/WAV 读取、数据库信号加载与 FFT 等长循环在安全点检查。大 BLOB 的哈希和解码按块检查令牌；数据库代理只在已入队命令真正观察取消并停止后返回，避免任务表面结束而数据库线程仍留有大块读取尾巴。动作成功返回是提交边界，晚到的取消请求不会覆盖已经提交的成功结果。
- 只有终态任务可以重试。重试复制原动作和通知目标，生成新编号并在快照中记录 `retryOf`；它不是原记录的状态回滚。
- 观察者可能在工作线程或提交线程运行，但不会在任何调度器内部锁下调用。每个任务的通知在锁内按阶段过滤、排队，由单一派发者在锁外串行调用；观察者重入、跨线程委托或并发请求 `shutdown()` 时只发起停止。主退出路径清除通知器后调用 `shutdownAndWait()` 完成 join。后台适配器只把快照和结果放入受互斥量保护的事件队列，终态结果按任务编号提取并释放，避免重试串结果或让大频谱长期滞留在调度器记录中。
- `MainWindow` 的通知器用 `QPointer` 保护窗口寿命，再通过 `QMetaObject::invokeMethod(..., Qt::QueuedConnection)` 请求 GUI 线程排空事件。工作线程不直接访问控件；导入成功后的 `SignalEntry` 也由 GUI 线程接纳进界面列表。
- 退出顺序固定为：清除跨线程 UI 通知器，停止调度并取消/等待工作线程，随后让数据库执行器停止接收命令、排空已接收命令并在所属线程关闭 SQLite。重复关闭是安全的。

## 阶段 6 的 SignalScript 边界

SignalScript 拆成四段，前端和算法都不依赖 GUI：

```text
UTF-8 source
    -> Lexer -> Token + LEX diagnostics
    -> Parser -> AST + PAR diagnostics（分号同步恢复）
    -> SemanticChecker -> 类型/符号表/绑定调用 + SEM diagnostics
    -> ScriptService -> Evaluator -> ImportService / 既有算法
                                  -> ScriptFileGateway
                                  -> 结构化 plot 请求
    -> ScriptRepository -> script_history + operation_log
```

`signallab_parser` 负责源位置、Token、AST、递归下降分析和语义绑定。AST 的每个表达式都有稳定的本次解析节点编号与源范围；语义模型用节点编号保存表达式类型和内建调用的参数绑定。管道没有单独的动态调度规则，而是在检查时把左侧值绑定为右侧白名单函数的首参数。解析器在错误后丢弃到下一个分号或文件末尾，再继续建立后续可恢复语句；任一前端错误都会阻止解释执行。

静态类型为 `Number`、`String`、`Boolean`、`Signal`、`Spectrum`、`FeatureSet`、`Unit` 和内部错误占位 `Invalid`。可执行内建只有 `normalize`、`lowpass`、`fft` 和 `features`；`load`、`plot`、`save` 是专用语句。`fft` 调用与 GUI 相同的 `analyzeSignal()` 算法路径，`features` 复用基本特征和质量指标实现。解释值持有不可变 `Signal`、完整分析结果或特征集合；`plot` 返回数据和标题，不直接引用 Qt Charts。

脚本的 `load` 使用 `ImportService::openCsv/openWav`，与 GUI 的“重复导入即提示”用例分开。脚本入口按“文件内容 SHA-256 + 规范化解析选项”查找逻辑来源：相同内容和选项会复用已经持久化的信号，使重复加载具备幂等性；原路径内容改变时会形成新的文件版本和逻辑信号。它只返回脚本计算所需的 `SignalEntry`，不读取或修改 `ImportService::entries()`，因此后台解释不会和 GUI 项目模型竞态；阶段 7 再由 GUI 线程显式刷新或接纳脚本产生的持久化信号。这个保证只覆盖 `load`：包含 `save` 的完整脚本再次执行时仍会因禁止覆盖已有输出而失败，先前已提交的导入或输出也不会因后续语句失败而整体回滚。

`ScriptService` 是用例边界：它在任何词法处理前创建 `RUNNING` 历史，组合 `LEX/PAR/SEM` 三类前端诊断，并把独立的 `RUN` 运行时问题与语言错误一起包装为正常的执行结果；只有无法可靠创建或完成历史等基础设施问题才使用外层 `Result` 失败。运行时接受 `std::stop_token`，取消、成功和失败都会尝试完成同一历史记录。

`ScriptFileGateway` 是运行时唯一文件能力。`FilesystemScriptFileGateway` 分别固定输入根和输出根，只接受根内相对路径，拒绝空路径、绝对路径、`..`、Windows 备用数据流以及符号链接/junction 逃逸。输入必须是普通文件；输出以临时文件写完后原子发布，若目标存在则失败且不覆盖。解释器未获得进程、Shell、网络、动态库或任意文件 API。

完整语法、内建参数和文件格式见 [SignalScript 说明](signalscript.md)。阶段 6 提供语言服务、执行结果和历史接口；把编辑器、运行操作、诊断导航及结构化绘图请求接入 `MainWindow` 是阶段 7 的职责。

## 显示下采样

原始 `Signal` 和完整 `Spectrum` 都不会因为绘图而改变。小信号/小频谱逐点显示；较大数据按图表像素宽度分桶，每桶按原索引顺序保留最小值和最大值。因此提交给 Qt Charts 的点数最多约为 `2 × 像素宽度`，时域瞬态和频域尖峰仍能保留。

文件预览、导入和分析已移出 UI 线程；完整 `Signal` 和 `Spectrum` 仍只在任务完成后交给显示路径。阶段 7 再补充窗口缩放后的延迟重绘。

## 后续边界

数据库、迁移、后台调度、取消和进程内重试继续沿用阶段 5 已验证的边界。阶段 6 的 SignalScript 前端、解释执行、受限文件网关和历史生命周期已经落入 `0.6.0` 代码；Debug 与 Release 均无警告构建成功并通过 216/216 项测试。阶段完成版本由 `v0.6` 与 `stage-6-complete` 标记，并生成仓库外 Git bundle。

阶段 7 将在不把 Qt 依赖反向带入 parser、algorithm 或 core 的前提下，把脚本编辑、执行状态、诊断定位、历史查询和绘图请求接到主窗口。批处理、插件化任意函数、网络数据源和系统命令不在 `0.6.0` 的语言能力内。
