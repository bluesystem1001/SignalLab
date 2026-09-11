# SignalLab 架构说明

本文只描述当前已经实现的结构。新阶段完成后再补充对应模块，避免文档先于代码膨胀。

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

## 阶段 4 的仓库边界与持久化导入

阶段 4 已把阶段 2 的临时 `app -> io` 依赖移除。当前目标依赖为：

```text
SignalLab（组装根）
├─> signallab_gui -> signallab_app -> signallab_algorithm -> signallab_core
│                              └─> repository 接口 -> signallab_core
├─> signallab_io       -> repository 接口 + signallab_core + Qt Core
└─> signallab_database -> repository 接口 + Qt Core + Qt Sql
```

`repository` 目录只声明文件、信号、任务和脚本历史的抽象接口。`ImportService` 和 `AnalysisService` 只依赖这些接口，不知道 SQLite 或文件系统细节。`main.cpp` 创建 `FilesystemFileRepository` 和 SQLite 仓库，再通过构造函数传给应用服务。[ADR 0006](adr/0006-stage2-import-service-boundary.md) 的退出条件因此已满足。

当前文件导入的数据流是：

```text
文件选择与参数 -> ImportService -> FileRepository
                                      -> 临时快照 / SHA-256 / 解析
                                      -> data/imports/<sha256>.source
                                      -> SignalRepository（SQLite 事务）
                                      -> SignalEntry -> 项目列表与波形
```

- `signallab_io` 实现 `FileRepository`，负责 CSV、PCM16 WAV、SHA-256、内容寻址存档和文件格式错误定位。
- `signallab_database` 实现 `SignalRepository`，把 `file_version`、`signal_source` 和 `signal_record` 在一个事务中注册。其中任何一步失败都会回滚。
- 新存档固定使用 `<sha256>.source`，位置使用项目相对路径；旧版本已有的 `<sha256>.csv/.wav` 会保持原名并继续复用，避免数据库中的旧路径失效。`sourcePath` 只记录导入时的来源位置，重启恢复使用项目内的存档。
- 导入先写同目录临时快照，再原子重命名。如果后续数据库注册失败，应用服务会在确认该存档未被其他记录引用后清理新文件。
- 文件 SHA-256 与规范化解析选项共同形成逻辑信号键。因此原始内容只存档一份，同一 CSV/WAV 却可用不同列或声道产生多个逻辑信号；只有内容和解析选项都相同时才拒绝重复导入。
- 启动时，`ImportService::restoreSignals()` 从 SQLite 读取全部信号，并先验证导入存档的相对路径和内容哈希，再重建界面列表。

CSV 支持逗号或制表符、可选表头、指定时间列/数值列，以及没有时间列时显式填写采样率。若使用时间列，其单位必须是秒，且解析器要求时间严格递增、近似等间隔。CSV 错误使用从 1 开始的行号和列号。

WAV 首版只接受 RIFF/WAVE 中的 PCM 16-bit 单声道或双声道。双声道可选择左、右或平均值，样本统一除以 `32768.0` 转换为归一化浮点值。WAV 错误包含字节偏移。

## 阶段 3 的同步算法闭环

`signallab_algorithm` 是只依赖 `signallab_core` 与 C++20 标准库的静态库。它包含预处理、窗函数、自研 radix-2 FFT、单边频谱、基本特征、峰值检测、FIR 和质量指标；不包含 Qt，也不直接负责界面状态。

GUI 只把所选记录编号交给 `AnalysisService`，当前同步数据流是：

```text
所选 signalRecordId -> AnalysisService -> SignalRepository::findByRecordId()
                                      -> 可信 Signal 与 sampleHash
                                      -> 原始样本的基本时域特征
                                      -> 去直流 -> Hann 窗 -> bit_ceil 补零
                                      -> radix-2 FFT -> 单边幅值/功率谱
                                      -> 峰值检测 -> SNR/THD
                                      -> 频谱显示下采样 -> Qt Charts
```

基本特征使用原始 `Signal`，避免去直流改变用户看到的均值；频谱路径先去直流，减少 DC 对峰值查找的影响。默认峰值阈值是最强非 DC 幅值的 1%，最多返回 10 个峰。若没有可用主峰，或当前信号无法按已定义口径计算质量指标，`dominantPeak`、`snrDb` 或 `thdPercent` 保持为空，GUI 显示“不可计算”。非法分析选项会直接返回失败。

归一化、截取和 FIR 低通/高通已经是独立公开算法，但阶段 3 GUI 没有为它们加入复杂参数面板。这样既保留课程算法实现，也让首个闭环保持直观。所有公式、边界、复杂度和算法版本见 [算法说明](algorithms.md)。

## 阶段 4 的 SQLite 追溯闭环

程序启动时打开 `data/signallab.sqlite3`，再按顺序执行 `migrations/` 中尚未记录的 SQL 迁移。`001_init.sql` 建立 `signal_source`、`signal_record`、`analysis_task`、`feature_result`、`file_version`、`script_history` 和 `operation_log`；`schema_migration` 由迁移执行器管理。每次连接都开启外键、WAL 和 5000 ms busy timeout。

当前分析与追溯数据流是：

```text
GUI 传入所选 signalRecordId
    -> AnalysisService 从 SignalRepository 加载可信 Signal + sampleHash
    -> TaskRepository 再核对记录编号/输入哈希并创建 RUNNING 记录
    -> AnalysisService 同步执行算法
       ├─ 成功 -> 特征结果 + 输出哈希 + SUCCEEDED（一个事务）
       └─ 失败 -> FAILED + 错误文本
    -> 历史任务页签
```

- 参数 JSON 使用固定键顺序、无多余空白和与区域无关的数字格式，便于重现同一次分析。
- 任务保存算法名/版本、参数 JSON、输入/输出 SHA-256 和 UTC 时间。输出哈希由稳定排序后的特征数据计算，不受调用者传入顺序影响。
- 特征写入和任务转为 `SUCCEEDED` 必须一起提交。中途任一 SQL 失败时，整组特征回滚，然后用新事务把任务标为 `FAILED`，不留孤立 `feature_result`。
- 启动时先把上次异常终止后遗留的 `RUNNING` 任务批量改为 `INTERRUPTED`，再恢复信号并打开界面。
- “历史任务”页签通过 `AnalysisService` 读取仓库，展示状态、算法版本、参数、哈希、UTC 时间、错误与特征值；GUI 不包含 SQL。
- `QSqlDatabase` 连接记录创建它的 Qt 线程。打开、查询和关闭都会检查线程归属，不允许把连接跨线程传递。

## 显示下采样

原始 `Signal` 和完整 `Spectrum` 都不会因为绘图而改变。小信号/小频谱逐点显示；较大数据按图表像素宽度分桶，每桶按原索引顺序保留最小值和最大值。因此提交给 Qt Charts 的点数最多约为 `2 × 像素宽度`，时域瞬态和频域尖峰仍能保留。

文件预览、导入和分析在阶段 4 仍是同步调用。阶段 5 引入任务系统后，大文件 I/O 和计算会移出 UI 线程；阶段 7 再补充窗口缩放后的延迟重绘。

## 后续边界

数据库、迁移和同步任务追溯已实现，但阶段 5 的后台调度/取消/重试与阶段 6 的 SignalScript 解释器仍未实现。`script_history` 及其仓库接口在阶段 4 只提供持久化基础，不代表脚本语法已经可用。后续模块会继续遵守“核心与算法模块不依赖 GUI”的方向。
