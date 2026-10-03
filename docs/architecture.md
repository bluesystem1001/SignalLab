# SignalLab 架构说明

本文描述应用 `1.0.0-rc1` 当前已经接入的结构。阶段 4 的持久化、阶段 5 的后台任务边界和阶段 6 的 SignalScript 语言服务保持不变；阶段 7 在不把 Qt 依赖反向带入 `core`、`algorithm` 或 `parser` 的前提下，完成主窗口的 Model/View 组装、交互式图表、脚本编辑/执行/历史界面，以及第三个后台适配器 `BackgroundScriptService`。`0.7.1` 作为阶段 7 后的补丁增量，增加分析历史与进程内任务快照的受限删除能力；`0.7.2` 增加时域横向缩放、后台 FIR 派生信号和单窗口文件导入；`0.7.3` 增加项目信号的可追溯删除闭环；`0.7.4` 再增加双信号同轴对比与波形时间轴滑动条。阶段 8 不改变运行时分层或数据库结构，新增的是覆盖率、稳定性、性能、固定资产和 Windows 发布包的验收工具与证据。

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

创建成功后，样本只能通过 `std::span<const double>` 读取，没有公开修改函数。底层样本保存在 `std::shared_ptr<const std::vector<double>>` 指向的共享不可变缓冲区中；复制 `Signal` 只共享样本缓冲区，元数据仍按值复制，不会为脚本与 GUI 之间的百万点信号再复制整段样本。`Signal::withMetadata()` 可为已计算出的信号替换派生元数据，同时继续共享不可变样本缓冲区。只要任一持有该缓冲区的 `Signal` 仍然存在，读取视图就有效。

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

`repository` 目录只声明文件、信号、任务、脚本历史和脚本文件能力的抽象接口。`ImportService`、`AnalysisService` 和 `ScriptService` 只依赖这些接口，不知道 SQLite 语句或文件安全检查的具体实现。`main.cpp` 创建 `FilesystemFileRepository`、`FilesystemScriptFileGateway`、`DatabaseExecutor`、`Scheduler`，以及文件、分析、脚本三个后台适配服务，再通过构造函数装配应用与界面；启动时还通过数据库代理清理遗留的分析与脚本执行。`DatabaseExecutor` 同时实现信号、任务和脚本仓库接口，但把具体 SQLite 仓库隐藏在其专属线程内。`FilesystemScriptFileGateway` 实现受限的输入根/输出根访问，解释器永远不会获得原始文件系统对象。[ADR 0006](adr/0006-stage2-import-service-boundary.md) 的退出条件因此已满足。

三个后台适配器共用“调度器任务 + 线程安全事件队列 + Qt queued 调用”的交付方式，但使用与工作负载相符的队列：

- `BackgroundFileService` 把文件预览、导入和项目信号删除提交到 I/O 队列，把 FIR 派生信号计算提交到计算队列。
- `BackgroundAnalysisService` 把分析提交到计算队列。
- `BackgroundScriptService` 把可能执行 FFT/FIR 的脚本提交到计算队列，把脚本历史查询提交到 I/O 队列；执行事件保留本次源码快照，终态事件独占可移动的结构化结果。

当前文件预览和导入的数据流是：

```text
文件选择 -> FileImportDialog（参数、元数据、预览修订）
    -> BackgroundFileService -> Scheduler I/O 队列
    -> 预览任务 -> ImportService -> FileRepository -> 线程安全事件队列
    -> Qt queued 调用 -> 同一对话框显示预览/行内错误并等待确认
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
- `FileImportDialog` 为每组设置分配递增修订号。选项变化会立即使当前预览失效；发起新预览或关闭窗口时会请求取消旧任务，无法及时取消而迟到的旧修订结果也不会覆盖新设置。只有当前修订预览成功后“导入”按钮才可用。

当前项目信号删除的数据流是：

```text
默认“否”的确认 -> BackgroundFileService -> Scheduler I/O 队列
    -> ImportService（与导入/生成注册共用互斥边界）
    -> DatabaseExecutor -> SignalRepository::removeSignal()
       -> 事务删除 signal_record / signal_source
       -> 最后引用时删除 file_version + DELETE_SIGNAL 审计
    -> FileRepository::removeStoredFile() 补偿清理项目存档
    -> 终态事件 -> GUI 线程 forgetSignal() -> 刷新模型与相邻选择
```

- 仓库以结构化 `SignalRemoval` 精确返回“本次是否删除记录”和“是否删掉最后一个文件版本”；共享同一内容的其他逻辑信号仍存在时不返回物理清理候选，因而不会误删共享存档。
- SQLite 提交是逻辑删除的提交边界。其后的存档清理失败只产生“信号已删除、孤儿存档保留”警告，不能把数据库中已不存在的条目留在 GUI；项目外的原始 `sourcePath` 从不参与删除。
- 删除成功会留下记录编号 tombstone。已经完成但尚未被 GUI 接纳的导入、FIR 或脚本结果若携带该编号，会被拒绝而不会形成只存在于内存、重启即消失的幽灵信号。
- 对应信号仍有排队或运行中的分析/FIR 任务时，删除按钮禁用；既有分析历史、特征、脚本历史、任务中心快照和 FIR 派生信号不级联删除。`analysis_task.signal_record_id` 由外键置空，复制的名称、输入哈希和结果继续可追溯。
- 内置 1 kHz 示例只在数据库文件首次创建时播种。已有但为空的数据库被视为用户明确删除后的有效状态，重启不会重新生成示例。

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

归一化、截取和 FIR 低通/高通都是独立公开算法。`0.7.2` 的 GUI 已为 FIR 增加低通/高通、截止频率和奇数抽头数面板；它复用相同的 Hamming 窗系数设计和零填充因果卷积，并保持输出样本数与输入一致。归一化还可由 SignalScript 调用，截取目前只通过公开算法接口使用。所有公式、边界、复杂度和算法版本见 [算法说明](algorithms.md)。

GUI FIR 的数据流是：

```text
所选 SignalEntry + FIR 选项 -> BackgroundFileService -> Scheduler 计算队列
    -> 参数校验 -> FIR 系数设计与可取消卷积
    -> ImportService::persistGeneratedSignal()
    -> DatabaseExecutor -> SignalRepository 事务
    -> 线程安全事件队列 -> Qt queued 调用
    -> GUI 接纳并选中新 GENERATED 派生信号
```

稳定来源键包含 FIR 算法版本、输入记录编号与样本哈希、类型、截止频率、抽头数和边界策略；参数 JSON 保存同一组追溯信息。相同输入与参数的并发或重试在应用服务边界串行查找/注册并复用既有记录。数据库提交后才到达的取消请求不会把已保存结果改写为取消；工作线程也不直接修改 GUI 持有的 `ImportService::entries()`。

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
    -> 分析历史页签
```

- 参数 JSON 使用固定键顺序、无多余空白和与区域无关的数字格式，便于重现同一次分析。
- 任务保存算法名/版本、参数 JSON、输入/输出 SHA-256 和 UTC 时间。输出哈希由稳定排序后的特征数据计算，不受调用者传入顺序影响。
- 特征写入和任务转为 `SUCCEEDED` 必须一起提交。中途任一 SQL 失败时，整组特征回滚，然后用新事务把任务标为 `FAILED`，不留孤立 `feature_result`。
- 启动时先把上次异常终止后遗留的 `PENDING` 或 `RUNNING` 分析任务批量改为 `INTERRUPTED`，再恢复信号并打开界面。
- “分析历史”页签通过 `AnalysisService` 读取仓库，展示状态、算法版本、参数、哈希、UTC 时间、错误与特征值；GUI 不包含 SQL。终态记录可经 `AnalysisService -> TaskRepository` 永久删除，`feature_result` 由外键级联清理，并与 `DELETE_TASK` 审计原子提交；活动记录由仓库拒绝。当前历史读取、特征读取和删除路径都会在 GUI 调用点同步等待 `DatabaseExecutor` 代理返回，SQL 仍只在数据库线程执行，因此不能把这些操作描述成 GUI 异步。
- `QSqlDatabase` 连接、迁移器和具体仓库都在数据库专属线程中创建和销毁。来自 GUI、计算池或 I/O 池的仓库调用通过命令队列串行执行，不会跨线程传递 Qt SQL 连接。
- 正常交互产生的 SQL 写入由文件、分析或脚本后台动作中的应用服务发起，再交给数据库线程执行；GUI 线程不直接执行 SQL 写入。脚本历史查询则由 `BackgroundScriptService` 经 I/O 队列异步交付给 `ScriptHistoryModel`。

调度器中的文件、分析和脚本任务都拥有进程内编号；`AnalysisService` 创建的分析记录持久化到 `analysis_task`，`ScriptService` 创建的脚本记录持久化到 `script_history`。因此“任务中心”的快照与重试来源不会在重启后恢复，而“分析历史”和脚本历史可以跨重启查询。删除任务中心记录只从 `Scheduler` 的公开快照集合移除终态任务，不触碰任一持久化表或文件；删除分析历史只作用于对应 `analysis_task` 及其特征；删除项目信号则作用于信号/来源以及最后引用的项目存档，并保留历史。三条删除路径作用域独立，不能互相替代。

## 阶段 5 的调度、通知与关闭

`signallab_task` 只依赖核心层和 C++20 标准库。`Scheduler` 分别拥有计算池与 I/O 池：计算池默认保留两个硬件线程给界面和系统，且至少使用两个工作线程；I/O 池默认一个线程并限制为一到两个。两个队列相互独立，文件读取不会占住计算任务的队列位置。

- 调度状态为 `PENDING -> RUNNING -> SUCCEEDED/FAILED/CANCELLED`。任务报告的进度不能倒退，默认限制通知频率；第一次达到 100% 可立即通知，紧随其后的重复 100% 报告仍受限频约束。
- 取消是协作式的。排队任务会从队列移除；运行任务收到 `std::stop_token`，CSV/WAV 读取、数据库信号加载与 FFT 等长循环在安全点检查。大 BLOB 的哈希和解码按块检查令牌；数据库代理只在已入队命令真正观察取消并停止后返回，避免任务表面结束而数据库线程仍留有大块读取尾巴。动作成功返回是提交边界，晚到的取消请求不会覆盖已经提交的成功结果。
- 只有终态任务可以重试。重试复制原动作和通知目标，生成新编号并在快照中记录 `retryOf`；它不是原记录的状态回滚。
- 只有终态任务可以从任务中心删除。`Scheduler::remove()` 与重试在同一锁下线性化，删除后查询不再返回该编号；已经发布或正在派发的终态 observer 通知仍保证交付，先进入 `waitForTerminal()` 的等待者也会持有任务记录直到观察到终态，避免界面隐藏记录导致后台结果丢失或等待超时。
- 观察者可能在工作线程或提交线程运行，但不会在任何调度器内部锁下调用。每个任务的通知在锁内按阶段过滤、排队，由单一派发者在锁外串行调用；观察者重入、跨线程委托或并发请求 `shutdown()` 时只发起停止。主退出路径清除通知器后调用 `shutdownAndWait()` 完成 join。后台适配器只把快照和结果放入受互斥量保护的事件队列，终态结果按任务编号提取并释放，避免重试串结果或让大频谱长期滞留在调度器记录中。
- `MainWindow` 的通知器用 `QPointer` 保护窗口寿命，再通过 `QMetaObject::invokeMethod(..., Qt::QueuedConnection)` 请求 GUI 线程排空事件。工作线程不直接访问控件；导入成功后的 `SignalEntry`，以及脚本结果中 `openedSignals` 携带的条目，都只由 GUI 线程显式接纳进项目模型。
- 退出顺序固定为：依次清除脚本、文件和分析适配器的跨线程 UI 通知器，调用 `Scheduler::shutdownAndWait()` 取消并等待工作线程，随后让 `DatabaseExecutor` 停止接收命令、排空已接收命令并在所属线程关闭 SQLite，最后才销毁栈上窗口与服务。重复关闭是安全的。

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

脚本的 `load` 使用 `ImportService::openCsv/openWav`，与 GUI 的“重复导入即提示”用例分开。脚本入口按“文件内容 SHA-256 + 规范化解析选项”查找逻辑来源：相同内容和选项会复用已经持久化的信号，使重复加载具备幂等性；原路径内容改变时会形成新的文件版本和逻辑信号。它不在工作线程修改 `ImportService::entries()`，而是把需要接纳的 `SignalEntry` 放入 `EvaluationOutput::openedSignals`；阶段 7 的终态事件把这些条目移动到 GUI 线程，再由 GUI 显式接纳和刷新项目模型。共享不可变样本缓冲区使这次交接不复制整段样本。这个保证只覆盖 `load`：包含 `save` 的完整脚本再次执行时仍会因禁止覆盖已有输出而失败，先前已提交的导入或输出也不会因后续语句失败而整体回滚。

`ScriptService` 是用例边界：它在任何词法处理前创建 `RUNNING` 历史，组合 `LEX/PAR/SEM` 三类前端诊断，并把独立的 `RUN` 运行时问题与语言错误一起包装为正常的执行结果；只有无法可靠创建或完成历史等基础设施问题才使用外层 `Result` 失败。运行时接受 `std::stop_token`，取消、成功和失败都会尝试完成同一历史记录。

`ScriptFileGateway` 是运行时唯一文件能力。`FilesystemScriptFileGateway` 分别固定输入根和输出根，只接受根内相对路径，拒绝空路径、绝对路径、`..`、Windows 备用数据流以及符号链接/junction 逃逸。输入必须是普通文件；输出以临时文件写完后原子发布，若目标存在则失败且不覆盖。解释器未获得进程、Shell、网络、动态库或任意文件 API。

完整语法、内建参数和文件格式见 [SignalScript 说明](signalscript.md)。阶段 7 已把编辑器、运行/取消、结构化诊断与源码定位、变量与输出摘要、结构化绘图、历史查询及历史源码重新载入接入 `MainWindow`；语言本身仍保持阶段 6 的受限能力边界。

## 阶段 7 的 GUI Model/View 组装

`MainWindow` 以左侧项目信号、中间工作区和右侧信号/参数/结果面板组织界面。中间工作区包含时域、频域、任务中心、分析历史和 SignalScript；界面状态不再依赖控件私有条目的临时文本，而由专用模型提供：

- `SignalListModel`、`TaskListModel`、`AnalysisHistoryModel` 和 `ScriptHistoryModel` 基于 `QAbstractListModel`，提供稳定编号、状态、图标、提示和可访问文本等角色。
- `AnalysisResultModel` 基于 `QAbstractTableModel`，统一展示特征名称、数值与单位；右侧同时展示实际分析参数和算法版本。
- `FileImportDialog` 把 CSV/WAV 设置、元数据、异步预览、行内错误和最终确认集中在一个模态窗口；窗口通过回调请求后台任务，不直接读取文件。
- FIR 参数组在提交前使用所选信号采样率校验 Nyquist 上限和奇数抽头范围；完成事件携带持久化后的 `SignalEntry`，GUI 线程显式接纳、刷新并选中派生信号。
- 主时域页把项目列表当前项作为主信号，并允许从组合框选择一个不同的对比信号。该状态只保存稳定 `recordId`，模型刷新后重新解析；记录消失、与主信号相同或项目删空时立即清除，因而不会跨导入、FIR 完成或删除事件保留行号和 `SignalEntry*`。主信号或对比信号切换时会同时清除旧组合留下的 Shift 选择覆盖层与摘要。比较只是显示状态，不写入 SQLite，也不改变分析、FIR 或脚本的输入选择。
- 项目列表提供异步“删除所选信号”。确认期间只复制稳定记录编号、名称和相邻编号，不保留 `SignalEntry*`；删除进入队列后同时禁止该信号的新分析/FIR 和相关任务重试，删除任务本身也不能绕过确认框从任务中心重试。成功后优先选择下一条、否则上一条，删空时清除波形、分析结果和详情并禁用信号操作。若用户等待期间已改选其他信号，则保留其当前选择。
- 任务中心从 `Scheduler` 快照显示排队、运行、成功、失败和取消状态，并提供终态重试、终态删除、协作取消、错误原因与处置建议。GUI 对一次脚本只维护一个活动任务，重试也遵守同一约束。
- 任务中心删除和分析历史删除均只在合法终态时启用，并以默认“否”的确认对话框说明影响范围；模型重置后按“下一条，否则上一条”恢复选择，删空时清理详情并禁用按钮。确认期间不保留模型内部记录指针，避免嵌套事件循环中的刷新使指针失效。
- SignalScript 运行结果以变量、输出文件、诊断和多个绘图页签呈现；点击诊断会把 UTF-8 源位置转换为 Qt 使用的 UTF-16 光标位置。脚本历史由后台 I/O 任务载入模型，而不是在历史视图中直接访问仓库。

## 显示下采样

原始 `Signal` 和完整 `Spectrum` 都不会因为绘图而改变。小信号/小频谱逐点显示；较大数据只在当前可见的 `[firstIndex, pastLastIndex)` 范围内按图表像素宽度分桶，每桶按原索引顺序保留最小值和最大值。因此一次提交给 Qt Charts 的点数最多约为 `2 × 像素宽度`，缩放后不会继续按全量范围抽样，时域瞬态和频域尖峰仍能保留。双信号模式会按各自采样率把同一可见秒区间转换为各自的索引范围并分别降采样，每条序列都遵守该上限，不会为了比较而复制或重采样完整信号。

`InteractiveChartView` 复用稳定的 chart/series 对象，并允许调用方把滚轮和矩形缩放约束为水平、垂直或双向。主时域图配置为仅水平缩放，并增加显式“横向放大/横向缩小”按钮，因此这些操作不会改写 Y 轴范围；频域与脚本图保持双向缩放。主时域序列以实线显示，可选对比序列以虚线显示并开启文字图例；二者从各自相对 `t=0` 开始挂接同一个秒轴，不插值或相位对齐，home X 范围取两者最大 duration，Y 范围覆盖两者且在后续水平缩放/滑动中保持稳定。不同单位会在轴标题中并列标明。图下 `QScrollBar` 用整数位置映射当前 X 窗口在完整时间域中的起点：home 视图禁用，放大后启用，滑动只平移并钳制 X 范围，不改变窗口跨度或 Y 轴，复位时归零。游标和 `Shift` 局部选择按两条信号各自的采样率分别报告样本信息；主信号或对比信号切换时清除旧选择覆盖层。控件还提供双击/按钮复位和 PNG 导出，并用 100 ms 单次定时器合并连续窗口尺寸变化。主时域 X 轴的连续范围变化使用不会被后续事件重启的 80 ms 单次定时器节流，滚动条松手时停止待处理定时器并立即按最终范围重绘；频域图和动态脚本图仍通过每次范围变化重新启动 80 ms 单次定时器进行防抖。各路径随后按新的可见索引区间重采样，避免交互期间反复提交大点集，同时保证松手终态不等待下一轮节流。

文件预览、导入、项目信号删除、FIR、分析和脚本执行已移出 UI 线程；完整 `Signal`、`Spectrum` 和脚本结构化结果只在任务完成后交给显示路径。分析历史、单条特征结果查询和分析历史删除目前仍由 GUI 同步等待数据库代理，属于已知的后续响应性改进点。

## 当前验证与后续边界

数据库、迁移、后台调度、取消和进程内重试继续沿用阶段 5 已验证的边界；阶段 7 的 GUI、后台脚本适配和范围感知显示已经落入 `0.7.0`—`0.7.4` 代码。阶段 8 候选版的 Debug 与 Release 全量 CTest 分别通过 273/273 项（13.28 秒、10.82 秒）；非 GUI 生产模块行覆盖率为 78.54%，各模块均达到 70%，最低的 database 为 70.02%。固定资产、30 分钟稳定性、性能与发布包的详细命令和结果集中记录在 [阶段 8 验证记录](stage8-validation.md)，避免架构文档复制易过期的发布证据。

真实窗口的两轮人工检查、另一台干净 Windows 机器验证和 2—3 分钟录屏尚未执行；高 DPI/多屏显示、主题观感、鼠标手感和真实交互延迟不能由离屏自动化测试代替，因此这里不把人工验收写成已完成。批处理、插件化任意函数、网络数据源和系统命令仍不在当前 SignalScript 能力内。
