# SignalLab 阶段 4—7 数据库说明

阶段 4 使用 SQLite 保存信号、分析任务和结果；阶段 5 在不改变仓库抽象边界的前提下增加数据库专属执行线程、排队状态和取消状态；阶段 6 为 SignalScript 增加可校验的源码身份、单次终态转换和错误行列；阶段 7 在 GUI 中接入分析/脚本历史模型，但没有改变数据库结构。程序通过 Qt 的 `QSQLITE` 驱动访问数据库，界面和应用服务都不直接编写 SQL。本文对应应用 `0.7.0` 的结构；Debug 与 Release 全量 233/233 项自动化测试均通过，真实窗口人工检查清单尚未执行。

## 文件位置与启动顺序

运行时数据库位于项目根目录下：

```text
data/signallab.sqlite3
```

导入文件的原始存档仍位于 `data/imports/`，不放进数据库。新存档固定命名为 `<sha256>.source`；旧版本已有的 `<sha256>.csv/.wav` 会保持原名并继续复用。SQLite 采用 WAL 模式，程序运行时还可能出现 `signallab.sqlite3-wal` 和 `signallab.sqlite3-shm` 两个辅助文件。

程序启动时依次执行：

1. 启动 `DatabaseExecutor` 专属线程，并在该线程打开数据库、应用连接设置。
2. 在同一线程执行尚未应用的迁移。
3. 把上次异常退出后遗留的 `PENDING` 或 `RUNNING` 分析任务改为 `INTERRUPTED`。
4. 把阶段 6 遗留且带源码哈希的 `RUNNING` 脚本执行改为 `INTERRUPTED`。
5. 从数据库恢复信号；对于 CSV/WAV，还会检查存档文件及其 SHA-256。
6. 构造主窗口时读取分析历史，供“分析历史”页显示；同时把脚本历史查询提交给 `BackgroundScriptService` 的 I/O 队列，完成后更新脚本历史模型。

如果数据库、迁移、存档或信号完整性检查失败，程序会显示启动错误，而不是带着不完整数据继续运行。正常退出时依次清除脚本、文件和分析适配器的跨线程 UI 通知器，调用 `Scheduler::shutdownAndWait()` 取消并等待工作线程，再让数据库执行器拒绝新命令、排空已接收命令并在所属线程关闭连接，最后才销毁窗口与应用服务。

## 分层方式

依赖方向如下：

```text
GUI -> app 应用服务 -> repository 抽象接口
                               ^
                               |
           DatabaseExecutor / io 具体实现
```

- `include/signallab/repository/` 定义信号、任务、脚本历史、普通导入文件和脚本受限文件网关接口，不依赖 SQLite。
- `src/database/` 实现 SQLite 信号仓库、任务仓库和脚本历史仓库；`DatabaseExecutor` 在专属线程内拥有这些具体对象，并作为三个仓库接口的线程安全串行代理。
- `src/io/` 实现文件预览、解析、存档和校验，并实现固定输入根/输出根的 `FilesystemScriptFileGateway`；脚本运行时不直接访问文件系统。
- 可执行程序在 `main.cpp` 中创建文件仓库、脚本受限文件网关、数据库执行器和调度器，再装配 `BackgroundFileService`、`BackgroundAnalysisService` 与 `BackgroundScriptService` 三个后台适配器。

因此，`app` 只知道仓库接口，GUI 不知道 SQL，`core` 和 `algorithm` 也不依赖 Qt SQL。

正常交互中的持久化写入由文件导入、分析或脚本执行的后台任务调用应用服务，再经 `DatabaseExecutor` 在数据库线程完成；GUI 线程不执行 SQL 写入。读取路径并非全部异步：脚本历史通过 `BackgroundScriptService` 的 I/O 队列查询并以 queued 事件更新 `ScriptHistoryModel`，但分析历史与所选任务的特征结果目前由 GUI 调用 `AnalysisService`，同步等待 `DatabaseExecutor` 代理返回。后者的 SQL 仍只在数据库线程执行，但等待者是 GUI 线程。

## 连接设置

数据库专属线程打开 `DatabaseConnection` 时会设置：

| 设置 | 值 | 作用 |
|---|---:|---|
| `PRAGMA foreign_keys` | `ON` | 启用外键和级联规则 |
| `PRAGMA journal_mode` | `WAL` | 采用预写日志，提高读写并行能力 |
| `PRAGMA busy_timeout` | `5000` | 数据库繁忙时最多等待 5000 毫秒 |

业务查询和写入都使用预编译语句与参数绑定，避免把用户数据直接拼进 SQL。

## 迁移

迁移文件放在 `migrations/`。`MigrationRunner` 按文件名的字典序执行所有 `.sql` 文件。当前包括：

- `001_init.sql`：建立阶段 4 的初始业务表、约束和索引。
- `002_task_scheduler.sql`：重建任务及特征表以扩展状态约束，加入 `CANCELLED`；迁移复制既有编号、任务历史和特征结果后再替换旧表。
- `003_signal_script.sql`：在不重建 `script_history` 的前提下增加 `source_hash`、`error_line` 和 `error_column`，并对阶段 6 的非空哈希记录约束状态、完成时间和错误字段组合；旧行保留原编号、原状态与空哈希。

阶段 7 只增加 GUI 模型和后台适配边界，没有新增字段、表或约束，因此 `0.7.0` 仍只应用 `001`—`003`，不存在阶段 7 的 `004` 迁移。

运行器自己创建 `schema_migration` 表：

| 字段 | 含义 |
|---|---|
| `version` | 已执行的迁移文件名，也是主键 |
| `applied_utc` | 迁移成功提交的 UTC 时间 |

一个迁移文件只执行一次。文件中的全部语句和迁移记录放在同一事务中；任一语句失败时整体回滚，不会留下“执行了一半”的结构。

已经发布的迁移文件不应修改。需要改变结构时，应使用下一个未占用编号新增迁移文件，例如当前应从 `004_add_xxx.sql` 开始。

## 表和关系

除 `schema_migration` 外，当前有 7 张业务表：

| 表 | 保存内容 | 主要关系与约束 |
|---|---|---|
| `file_version` | 导入文件的内容哈希、项目内存档路径、原文件名和字节数 | `content_hash` 与 `stored_relative_path` 唯一；一个文件版本可被多个逻辑信号来源复用 |
| `signal_source` | `GENERATED`、`CSV` 或 `WAV` 来源、稳定来源键、原路径和导入参数 JSON | 可选关联一个 `file_version`；`source_key` 唯一 |
| `signal_record` | 采样率、样本数、样本 BLOB、样本哈希和信号元数据 | 每个 `signal_source` 恰好对应一个记录；删除来源时级联删除记录 |
| `analysis_task` | `PENDING/RUNNING/SUCCEEDED/FAILED/INTERRUPTED/CANCELLED` 状态、信号名、算法名/版本、参数、输入/输出哈希、时间和错误 | 可选关联一个 `signal_record`；删除信号记录时任务保留，外键改为 `NULL` |
| `feature_result` | 一次任务产生的特征名、数值、文本值和单位 | 多条结果属于一个任务；任务删除时级联删除；同一任务内特征名唯一 |
| `script_history` | 原始脚本文本、源码 SHA-256、`RUNNING/SUCCEEDED/FAILED/CANCELLED/INTERRUPTED` 状态、开始/结束时间及可选错误行列和消息 | 与信号/分析表独立；阶段 6 服务负责生命周期，阶段 7 的脚本历史模型已经接入查询与源码重新载入 |
| `operation_log` | 保存信号、创建/完成/失败任务，以及脚本历史创建、完成和批量中断等写操作日志 | 用 `entity_type` 和可选 `entity_id` 指向业务对象，不设置外键 |

主要关系可以简化为：

```text
file_version 1 ── 0..N signal_source 1 ── 1 signal_record
                                              |
                                              └── 0..N analysis_task 1 ── 0..N feature_result

script_history        operation_log          （两者独立）
```

文件内容哈希相同但导入选项不同，例如 CSV 选择不同列或 WAV 选择不同声道时，可以复用同一个 `file_version`，同时建立不同的 `signal_source`。逻辑来源键由文件 SHA-256 和规范化导入选项共同组成。

## 事务与失败语义

### 保存和删除信号

保存一个导入信号时，`file_version`、`signal_source`、`signal_record` 和 `operation_log` 在同一个数据库事务内写入。任何一步失败都会回滚，因此不会只留下文件版本或来源记录。删除信号时，相关来源、没有其他引用的文件版本和操作日志也在同一事务中处理。

原始文件存档属于文件系统，不能参加 SQLite 事务。导入服务先完成文件存档，再提交数据库；若数据库写入失败，它会检查该存档是否已被其他记录引用，并在安全时删除本次新建的存档。这是一种补偿清理，不是跨文件系统和数据库的真正事务。阶段 5 将“存档提升—数据库注册—必要时补偿删除”串行化，避免多个 I/O 工作者对同一内容并发操作时误删另一个任务即将使用的存档。

### 保存分析任务和结果

GUI 只把所选 `signalRecordId` 交给后台分析服务。计算工作者中的 `AnalysisService` 先通过 `SignalRepository::findByRecordId()` 重新加载可信的 `Signal` 和 `sample_hash`，不会采用 GUI 传入的样本或哈希；任务仓库创建任务时还会再次核对记录编号与输入哈希。分析记录先以 `PENDING` 写入，再用 `WHERE status = 'PENDING'` 的条件更新取得启动权并转为 `RUNNING`。分析成功后，全部 `feature_result`、任务的 `SUCCEEDED` 状态、输出哈希和操作日志在一个事务中提交。

如果任一特征写入或完成更新失败，完成事务会先整体回滚，所以不会留下部分特征。随后仓库用一个新事务把任务标记为 `FAILED`，写入完成时间和错误信息，并保持空输出哈希。空特征名和 NaN 也会使任务失败；正负无穷以 SQL `NULL` 数值配合文本 `inf` 或 `-inf` 保存。

取消请求使用 `std::stop_token` 传入信号加载和算法。`DatabaseExecutor` 的可取消等待会在令牌停止后及时把控制权还给计算工作者；具体信号仓库也会在 BLOB 校验、解码边界检查令牌。在提交特征前观察到取消时，仓库把 `PENDING` 或 `RUNNING` 记录改为 `CANCELLED`，删除任何未完成特征并记录原因。若成功事务已经提交，晚到的取消请求不会把 `SUCCEEDED` 倒退为 `CANCELLED`。调度器中尚未开始便被移出队列的分析动作不会创建数据库记录，因为 `AnalysisService` 尚未执行。

程序正常启动时会把所有遗留的 `PENDING` 和 `RUNNING` 任务统一改为 `INTERRUPTED`，写入完成时间和原因。这样异常退出不会让任务永久显示为排队或运行中。

### SignalScript 执行历史

`ScriptService` 在词法分析之前调用 `createScriptHistory()`。仓库验证源码是 UTF-8，按原始字节（包括空白和换行差异）计算 lowercase SHA-256，并在一个事务中插入 `RUNNING` 记录和 `CREATE_SCRIPT_HISTORY` 操作日志。此时 `finished_utc` 与错误消息为空，错误行列为 `NULL`。

一次执行只允许从 `RUNNING` 转为一个终态。`finishScriptHistory()` 使用 `WHERE id = ? AND status = 'RUNNING' AND source_hash <> ''` 条件更新，并在同一事务写入 `FINISH_SCRIPT_HISTORY` 操作日志：

- `SUCCEEDED` 必须有完成时间，且不得保存错误位置或消息。
- `FAILED`、`CANCELLED` 必须保存非空错误消息；若有源位置，行和列必须同时存在且从 1 开始。
- `INTERRUPTED` 用于下次启动清理上一进程遗留的 `RUNNING`，保存原因但不伪造源位置。

若另一个完成动作已经赢得竞态，条件更新不会改写终态。脚本历史创建、单条完成和批量中断都与各自操作日志一起提交或一起回滚。阶段 6 记录还会在读取时重新计算源码 SHA-256；哈希不一致、未知的新状态或不完整错误位置会作为数据库完整性错误返回。

`003_signal_script.sql` 特意让迁移前记录保持空 `source_hash`。这些旧行保留原始状态文本；仓库映射已知状态，其他旧拼写归为 `Legacy`。它们不参加新的终态更新和启动中断，避免把历史数据误认成正在执行的阶段 6 脚本。

阶段 7 没有为 GUI 新增脚本历史写入语义。脚本执行仍在 `BackgroundScriptService` 的计算队列中调用 `ScriptService` 创建并完成记录；历史按钮和执行后的刷新则在 I/O 队列调用 `ScriptService::history()`，将记录移动进 `ScriptHistoryModel`。GUI 只显示记录或把所选历史源码重新放回编辑器，不直接更新 `script_history`。

## 可复现记录

### 规范参数 JSON

导入服务和分析服务自行生成参数 JSON，规则是：

- 对象键按字母顺序固定排列。
- 不加入无意义空格。
- 字符串统一转义引号、反斜杠和控制字符。
- 浮点数使用经典 `C` 区域设置和 17 位有效精度；正负零统一写成 `0`，非有限值写成 `null`。

数据库同时保存算法名、组合后的算法版本、参数 JSON、输入哈希和输出哈希。相同数据、相同选项和相同算法版本因而能得到可比较的记录。仓库只检查 JSON 非空，规范化责任在应用服务；不要绕过服务直接传入随意格式的 JSON。

### 信号输入哈希

`signal_record.sample_hash` 是 SHA-256，输入字节严格按以下顺序组成：

1. 把采样率的 IEEE-754 `double` 位模式写成 8 字节小端序。
2. 按样本顺序，把每个样本的 IEEE-754 `double` 位模式写成 8 字节小端序。

信号元数据不参与该哈希。分析任务的 `input_hash` 使用这个值。恢复信号时会重新计算并比较哈希，再把 BLOB 解码为样本；不一致会作为数据库完整性错误返回。

### 特征输出哈希

任务完成前，特征先按名称稳定升序排列。每个特征转换为一行规范字节：

```text
名称字节数:名称|单位字节数:单位|规范数值\n
```

有限数值使用经典区域设置和 `double` 的 `max_digits10` 精度，正负无穷分别写为 `inf`、`-inf`，NaN 不允许保存。所有行拼接后计算 SHA-256，结果写入 `analysis_task.output_hash`。因此调用者提供特征的先后顺序不会改变输出哈希。

### UTC 时间

所有 `*_utc` 字段都由 `QDateTime::currentDateTimeUtc()` 生成，并以带毫秒的 ISO 8601 文本保存。数据库不保存本地时区；显示层需要本地时间时再转换。

## Qt SQL 的线程归属

`DatabaseConnection` 在构造时记住当前线程。打开、查询、提交和关闭前都会检查线程；跨线程访问会返回错误。连接销毁时先释放 `QSqlDatabase` 句柄，再调用 Qt 的 `removeDatabase()`。

阶段 5 的 `DatabaseExecutor` 在一个固定线程内创建、使用并销毁 `DatabaseConnection`、迁移器和三个 SQLite 仓库。GUI、计算工作者和 I/O 工作者调用的是代理；代理把命令放入队列并同步等待所属线程返回 `Result`，不会把连接或具体仓库跨线程传递。调用方可能是后台工作者，也可能是当前分析历史/特征结果读取的 GUI 线程；线程归属安全不等同于调用方一定异步。可取消的信号读取把停止令牌传到数据库线程；BLOB 哈希和解码按块检查令牌，代理在已入队命令真正停止后才向调用方返回取消，因此后续关闭不会再等待隐藏的读取尾巴。数据库连接不会被跨线程强行中断。`start()` 和 `shutdown()` 可重复调用；关闭会先拒绝新命令、排空已接收命令，再在数据库线程销毁 Qt SQL 对象。

## 当前限制

- 数据库面向本机单项目使用，不提供网络数据库或多用户权限管理。
- 迁移历史只记录文件名和应用时间，不校验迁移文件内容，也没有自动降级迁移。
- 信号样本整体存为一个 BLOB，恢复时也会整体读入内存；当前未实现超大信号的分块存储或流式读取。
- 参数 JSON 的规范性由应用服务保证，SQLite 只检查它非空。
- 文件存档位于数据库外，只能使用补偿清理；备份正在运行的项目时，应同时考虑数据库、WAL 辅助文件和 `data/imports/`。
- 所有 SQLite 操作由一个执行线程串行处理，保证线程归属和简单事务语义，但长查询仍可能让其他数据库调用排队；当前没有连接池。
- 分析历史列表和单条特征结果当前会让 GUI 同步等待数据库代理；SQL 不在 GUI 线程执行，也没有 UI 线程 SQL 写入，但很长的读取仍可能影响界面响应。脚本历史已经通过 I/O 队列异步读取。
