# SignalLab

SignalLab 是一个面向课程学习的一维信号分析程序，支持模拟信号、CSV/WAV 导入、时域处理、FFT、特征提取、SQLite 追溯、后台任务和 SignalScript；当前源码还提供 Windows 麦克风实时采集、流式 FIR 滤波、WAV 保存及 MariaDB 项目备份。

“阶段 4：SQLite 持久化与追溯”和“阶段 5：后台任务与取消”的既有能力已经过回归复核；当前候选版本为 `1.0.0-rc1`。阶段 6 加入了 SignalScript 0.6 的语言前端、静态检查、受限解释运行时、文件安全网关和可追溯执行历史；阶段 7 已把这些能力接入基于 Model/View 的主窗口，并完成交互图表、任务中心、分析历史和 SignalScript 控制台。`0.7.1`—`0.7.4` 又补齐历史/任务删除、横向缩放、FIR、单窗口导入、项目信号删除、双信号同轴对比和时间轴滑动条。阶段 8 不再扩展业务范围，集中完成测试、覆盖率、性能、固定资产、文档和 Windows 候选包冻结。

阶段 8 的入口文档为 [用户手册](docs/user-guide.md)、[测试计划](docs/test-plan.md)、[发布资产说明](docs/release-assets.md)、[演示脚本](docs/demo-script.md) 和 [验证记录](docs/stage8-validation.md)。

2026-10-11 实时音频扩展已在 Debug/Release 构建并各通过 308/308 项测试（新增 35 项）。下文阶段 8 的 273 项测试和发布 ZIP 信息属于原冻结版本；新功能需要运行当前源码构建的程序。真实麦克风录音、权限与拔插仍需按用户手册验证。

同日 MariaDB 备份扩展完成后，Debug/Release 全量测试各为 353 项通过、1 项因 Windows 符号链接权限跳过、0 失败；包含真实本机 MariaDB 集成测试。另已完成现有项目的首次备份及独立副本恢复，成功读取全部 6 个信号。连接密码仅保存在仓库外的 Windows 用户加密配置中。

2026-10-13 云端连接改为通过 `mysql.h` 使用原生 MariaDB Connector/C，直接链接 `libmariadb.dll`；本地 SQLite 持久化与一致性快照继续使用 Qt SQL / QSQLITE。当前源码及新打包脚本不依赖或分发 Qt `QMYSQL` 插件。上述 2026-10-11 测试和备份结果保留为当时实现的验证记录。

原生连接改动验证：Debug/Release 均构建成功，各 358 项测试通过、1 项因 Windows 符号链接权限跳过、0 失败；13 项 MariaDB 专项测试全部通过。在 QMYSQL 不可用环境中成功读取旧备份并恢复全部 6 个信号；独立 Release 包通过原生依赖、无 QMYSQL 插件、许可证、哈希和隔离启动检查。

## 当前可以看到什么

运行程序后，主窗口会显示：

- 全新项目数据库首次创建时会加入 48 kHz 采样率、0.02 秒、幅值 1.0 的 1 kHz 正弦信号；用户删除后不会在重启时自动重建。
- 基于 Qt Model/View 的左侧项目列表，以及所选信号的名称、采样率、样本数和持续时间。
- 项目信号的持久化删除入口；确认框默认为“否”，存在关联的活动分析或 FIR 任务时不允许删除，删除排队后也会冻结该信号的新分析/FIR 与相关重试；成功后选中相邻信号，删空时清空详情、图表和分析结果。
- CSV 和 WAV 导入按钮；选定文件后，格式参数、信号元数据、后台预览、行内错误与最终确认都在同一个导入窗口完成，并保留重复逻辑信号提示。
- “实时采集”页签：选择麦克风、采样率和直通/低通/高通，开始后实时显示原始与滤波波形、时长和电平；停止后保存原始或滤波后的 PCM16 WAV，也可保存并导入项目继续分析。使用方法及麦克风权限排查见[用户手册](docs/user-guide.md#17-麦克风实时采集)。
- “云端备份”页签：连接本机 MariaDB，手动备份项目数据库及导入源文件、查看历史备份、下载校验后的恢复副本；支持后台进度、取消和 Windows 用户加密保存密码。默认目标为 `127.0.0.1:3306/signallab_backup`，操作方法见[备份说明](docs/user-guide.md#18-mariadb-数据库备份)。
- 由 Qt Charts 绘制的“时域”和“频域”交互图表。时域图可在主信号之外选择一个第二信号：两条波形以各自相对 `t=0` 在同一秒轴显示，不做重采样或相位对齐，完整时间域取两者中较长的 duration；主信号为实线、对比信号为虚线并显示图例，单位不同时轴标题会明确标注。时域图的滚轮、矩形和显式按钮只缩放横轴，放大后可用图下滑动条平移固定宽度的时间窗口，完整视图下滑动条禁用。图表还支持分别报告两信号的游标/选择信息、复位和 PNG 导出；切换主信号或对比信号会清理旧组合的 Shift 选择覆盖层。大信号会按每条序列的当前可见区独立保留桶内极值，每条提交点数不超过约 `2 × viewport width`。
- “后台分析所选信号”按钮，以及均值、总体方差、RMS、峰峰值、主频、SNR 和 THD；结果同时显示实际参数、算法版本和单位。
- FIR 低通/高通侧栏，可设置截止频率和奇数抽头数；滤波在后台计算通道运行，完成后生成、持久化并选中一个带输入来源和算法参数的派生信号。
- “分析历史”页签，可查看任务状态、算法版本、规范化参数 JSON、输入/输出哈希、UTC 时间和特征结果；终态记录可在确认后连同其特征结果从 SQLite 永久删除。
- “任务中心”页签，可查看本次运行中导入、FIR、分析和脚本任务的排队、运行、成功、失败或取消状态，以及进度、错误建议和重试来源；终态快照可从本次进程的列表移除，不影响持久化历史或文件。
- “SignalScript”页签，包含脚本编辑、运行/取消、诊断点击跳转、变量/输出/AST、结构化信号与频谱图，以及可重新载入源码的执行历史。
- CSV/WAV 预览、确认后的导入、FIR 滤波以及信号分析均在后台执行；工作线程只投递事件，实际界面更新回到 Qt GUI 线程。
- 重启后从数据库恢复未删除的内置、导入和 FIR 派生信号，不再只保存在本次运行的内存中。

核心层会拒绝无效采样率、空样本和 NaN/无穷大样本。算法层还会校验窗、FFT 长度、频带、峰值、FIR 和质量指标参数，并拒绝运算后产生的非有限结果。新导入的原始文件按 SHA-256 内容哈希保存为项目相对路径 `data/imports/<sha256>.source`；旧版本已存在的 `<sha256>.csv/.wav` 存档会保持原名并继续复用。SQLite 数据库位于 `data/signallab.sqlite3`，SignalScript 的 `save` 输出根为 `data/script_outputs/`。这些都是运行数据，不会提交到 Git。

阶段 5 已加入任务队列/线程池、取消与重试、进度通知、后台适配器、数据库专属线程和真实 FFT/大 BLOB 读取中途取消等自动化测试。阶段 5 的 `0.5.0` 基线在 Debug 与 Release 下均无警告构建成功并通过 164/164 项测试；观察者重入、跨线程委托关闭及并发关闭用例重复 100 轮，批量取消和 GUI 运行时用例重复 10 轮，大 BLOB 取消后排空用例也重复 10 轮，均全部通过。阶段 6 完成后，`0.6.0` 在 Debug 与 Release 下均无警告构建成功并通过 216/216 项测试；资源边界、取消、重复加载、非有限 JSON、安全路径和异常历史收尾等关键用例又在 Release 下重复验证。

阶段 7 的 `0.7.0` 在 Debug 与 Release 下均通过 233/233 项测试；`0.7.1`、`0.7.2` 和 `0.7.3`—`0.7.4` 增量依次提升到 242、255 和 268 项。当前 `1.0.0-rc1` 的 Debug 与 Release 全量 CTest 分别通过 273/273 项（13.28 秒、10.82 秒）；非 GUI 生产模块行覆盖率为 78.54%，algorithm、app、core、database、io、parser、task 七个模块全部达到 70% 门槛。65,536 点 FFT 五轮平均值中位数为 3.251 ms，10 MiB CSV 解析五轮平均值中位数为 161.136 ms。完整的稳定性、资产与人工门禁状态以 [阶段 8 验证记录](docs/stage8-validation.md) 为准；最终 ZIP 的 SHA-256 仅以归档旁同名 `.zip.sha256` 文件为准。

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

完整语法、参数规则、输出格式、安全边界和当前 GUI 行为见 [SignalScript 说明](docs/signalscript.md)。

## 后台分析与任务管理

“后台分析所选信号”的算法顺序保持不变，由后台计算任务执行：

```text
Signal -> 基本时域特征
       -> 去直流 -> Hann 窗 -> 自动补零 -> 自研 radix-2 FFT
       -> 单边幅值/功率谱 -> 峰值 -> SNR/THD -> 频谱图
```

非 2 次幂长度会补零到下一个 2 次幂。幅值按窗的相干增益校正；主峰再用三点对数抛物线插值细化。SNR 把基频保护频点之外的全部非 DC 功率（包括谐波）视为噪声，THD 则只累计第 2 次到指定最高次数的谐波功率。准确公式、FIR 边界策略、复杂度、版本字符串和 Release 基准见 [算法说明](docs/algorithms.md)。

调度器把 FIR、分析与 SignalScript 执行送入计算线程池，把 CSV/WAV 预览和导入送入小型 I/O 线程池。内存任务按 `PENDING -> RUNNING -> SUCCEEDED/FAILED/CANCELLED` 变化；运行中的长循环通过 `std::stop_token` 协作取消。排队任务可直接移出队列，已结束任务可用新编号重试，并记录来源任务编号；也可在确认后从任务中心移除，但已发布的终态通知仍会交付，后台服务不会因此丢失完成结果。成功返回是提交边界，因此提交完成后才到达的取消请求不会把已经成功的任务改写成“已取消”。“任务中心”只记录本次运行的调度状态；可跨重启查询的分析记录和脚本记录分别位于“分析历史”和 SignalScript 历史中。

持久化分析先在 SQLite 创建 `PENDING` 记录，再以条件更新转为 `RUNNING`。成功时，全部特征和 `SUCCEEDED` 状态在一个事务中提交；取消时记为 `CANCELLED`，其他失败记为 `FAILED`。任何特征写入失败都会整体回滚。数据库连接和具体仓库只在数据库专属线程内创建、使用与销毁，GUI、计算线程和 I/O 线程通过同一个串行代理访问它。GUI 的 FIR 低通/高通复用既有算法接口，以零填充边界策略产生同长度信号，并用稳定来源键幂等保存为 `GENERATED` 记录；归一化仍可由 SignalScript 调用，截取目前只作为独立算法接口提供。

项目信号删除由 `BackgroundFileService` 放入 I/O 通道，再由 `ImportService` 协调 SQLite 与导入存档。SQLite 事务删除选中的 `signal_record` 与 `signal_source`；既有分析历史、特征、任务快照、脚本历史和 FIR 派生信号均保留，其中分析历史的 `signal_record_id` 由外键置为 `NULL`。多个逻辑信号共享的项目存档在最后一个引用删除前不会清理；最后一个引用删除时，同一事务也会移除已无引用的 `file_version`。用户选取的外部原文件始终不会被删除。数据库提交是逻辑删除的边界；之后如果物理存档清理失败，界面报告清理警告而不会把已提交的删除伪装成失败。删除墓碑还会拒绝同一记录已在途中的延迟结果，防止信号回到项目列表。删除任务不能从任务中心直接重试，必须返回项目列表重新确认，以保持影响范围和相邻选择一致。

### 1.0.0-rc1 最终真实 GUI 人工验收（尚未执行）

以下项目截至 2026-10-03 均未执行；自动化测试通过不能代替这份视觉与连续运行检查。应从最终 Windows 候选包在真实桌面环境中连续完成两轮，每轮都从独立启动的进程开始，并在结束后正常关闭：

1. 确认窗口标题为 `SignalLab 1.0.0-rc1`，三栏布局、文字、图标、焦点和高 DPI 缩放无截断或重叠；用键盘切换项目列表、对比信号选择、任务中心、分析历史和 SignalScript 列表，状态不能只靠颜色表达。
2. 导入示例 CSV，确认参数、元数据、预览、错误和确认始终位于同一个导入窗口；修改参数后旧预览应失效，当前预览成功前“导入”不可用。提交后运行后台分析，确认任务状态与进度不倒退，分析历史显示实际参数、算法版本和带单位的结果。
3. 选择两个采样率或时长不同的信号进行对比，确认它们从各自相对 `t=0` 开始共用同一秒轴，主实线、对比虚线和文字图例可辨；异单位时轴标题必须明确标注，游标和 Shift 选择摘要应分别报告两信号。先拖出一段 Shift 选择，再切换主信号和对比信号，确认旧选择覆盖层及摘要被清理。对大信号反复调整窗口大小，检查滚轮、矩形及“横向放大/横向缩小”只改变 X 轴且 Y 轴保持稳定；完整视图下时间轴滑动条应禁用，放大后把滑动条移到左端、中间和右端，确认时间窗口宽度不变、不会越过完整时间域且两条波形均按可见区更新，松开滑块时最终位置立即重绘。最后检查复位会恢复完整时间域并禁用滑动条，切换或删除主/对比信号不会留下错误图例或悬空波形，再检查频域图的双轴缩放、十字光标、双击复位和 PNG 导出。
4. 分别提交 FIR 低通和高通，检查无效截止频率或偶数抽头会立即阻止提交；成功任务应生成并自动选中派生信号，重启后仍可恢复。用相同输入和参数重试时不应产生第二条数据库记录。
5. 在 SignalScript 页运行不带 `save` 的示例两次，确认编辑器、变量、输出、AST、结构化信号/频谱图和历史同步更新；再制造一条诊断并点击跳到正确源码位置。
6. 启动一个足够长的脚本并取消，确认任务中心、脚本状态和历史一致，GUI 在取消期间仍可交互；从任务中心重试脚本时应继续保持“单个活动脚本”约束。
7. 分别对终态任务和终态分析历史验证“否/是”确认：任务中心删除不影响分析/脚本历史和文件；分析历史删除会跨重启生效但不删除原始信号。删除后应选中相邻项，删空后按钮应禁用；运行中记录不可删除。
8. 选中项目信号验证删除确认的“否/是”路径，初始焦点必须在“否”；关联分析或 FIR 任务处于排队/运行时删除按钮应禁用，删除进入队列后分析与 FIR 按钮也应禁用，任务中心不能直接重试删除任务。删除后应选中相邻项，删空后详情、图表和结果应清空；分析历史/特征、任务中心、脚本历史及既有 FIR 派生信号仍在。对同一存档的两个逻辑信号依次删除，确认存档只在最后一个引用消失后清理，外部原文件不受影响。
9. 每轮关闭程序前都检查无模态错误残留、无崩溃或后台线程悬挂；第二轮启动后确认已删除信号不会重生，删空后内置 1 kHz 信号也不会自动重建；未删除的导入/FIR 派生信号、分析历史和脚本历史仍可读取。

### 阶段 5 人工验收（保留作回归检查）

1. 按本文 Debug 命令启动当前程序，确认窗口标题为 `SignalLab 1.0.0-rc1`；全新数据库的左侧能看到内置 1 kHz 信号，并能打开“任务中心”页签。
2. 导入 `data/samples/example_signal.csv`：在同一个导入窗口内选择逗号、有表头、数值列 2、时间列 1，并更新预览。确认预览成功前不能提交，确认后才出现导入任务，成功后信号才加入左侧列表。
3. 单击“后台分析所选信号”，确认任务经历排队/运行/成功，运行时能显示非倒退的进度；完成后频谱和特征更新，“分析历史”中出现持久化的成功记录。
4. 使用足够大的输入启动耗时任务，在“任务中心”页选中它并取消。确认界面仍能操作，任务最终显示“已取消”；过小任务可能在取消前已经跨过成功提交边界，不适合这一项验收。
5. 选中一个已结束任务并重试，确认生成新的任务编号，并显示原任务编号；非终态任务不能重试。
6. 在有排队或运行任务时关闭窗口，确认程序停止通知、协作取消并等待工作线程后再关闭数据库，进程能正常结束且不崩溃。
7. 重新启动程序，确认已导入信号和分析历史仍可查询；“任务中心”是本次进程内列表，不要求跨重启恢复。

排队取消竞态、观察者重入或跨线程委托关闭、工作线程触发关闭、重复 100% 进度限频、事务中途失败和异常退出遗留 `PENDING/RUNNING` 等边界不适合依赖人工稳定复现，代码中已有对应自动化测试，并已包含在 Debug/Release 的 164/164 通过结果中。

### 阶段 4 回归检查

1. 打开“分析历史”并选中最新分析，确认算法名/版本、参数 JSON、输入/输出哈希、UTC 时间和特征结果均可读取。
2. 对同一个 CSV 改用“不选时间列、显式填写 1000 Hz”再次导入，确认它能成为第二个逻辑信号，但 `data/imports/` 不会多出第二份相同内容的存档。
3. 用完全相同的文件和选项再导入一次，确认出现重复提示，且信号列表和存档数量不变。

### 阶段 3 人工验收

1. 启动程序，在左侧选择“内置 1 kHz 正弦信号”，单击“后台分析所选信号”。
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
| MariaDB Connector/C | MSYS2 MINGW64 `libmariadbclient 3.4.8-3`，云端备份通过 `mysql.h` 原生接口使用 |
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
& 'E:\MSYS2\usr\bin\pacman.exe' -Q mingw-w64-x86_64-qt6-base mingw-w64-x86_64-qt6-charts mingw-w64-x86_64-gtest mingw-w64-x86_64-sqlite3 mingw-w64-x86_64-git mingw-w64-x86_64-libmariadbclient
```

如果任一命令提示文件或包缺失，请先停止配置和构建，补全环境后再重新检查。

需要在一台新机器上安装这些包时，可在 PowerShell 中执行：

```powershell
& 'E:\MSYS2\usr\bin\pacman.exe' -S --needed mingw-w64-x86_64-qt6-base mingw-w64-x86_64-qt6-charts mingw-w64-x86_64-gtest mingw-w64-x86_64-sqlite3 mingw-w64-x86_64-git mingw-w64-x86_64-libmariadbclient
```

不要为了本项目执行局部升级，也不要通过 Qt 官方安装器、vcpkg、Conan 或 CMake `FetchContent` 再安装第二套依赖。

云端备份的构建需要同一 MINGW64 环境中的 MariaDB Connector/C 头文件 `include/mariadb/mysql.h`、导入库和 `bin/libmariadb.dll`。本地 SQLite 仍需要 Qt SQL 及 `sqldrivers/qsqlite.dll`；`sqldrivers/qsqlmysql.dll` 不参与当前实现。

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

## Windows `1.0.0-rc1` 候选包

阶段 8 使用独立的 `BUILD_TESTING=OFF` Release 构建，并由 `windeployqt6`、递归 DLL 闭包检查和包内哈希清单生成可携带目录与 ZIP：

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\tools\package-windows.ps1 -Force
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\tools\verify-windows-package.ps1 -PackageDirectory .\dist\SignalLab-1.0.0-rc1-windows-x64
```

成功后产物位于 `dist/SignalLab-1.0.0-rc1-windows-x64/`、同名 `.zip` 和 `.zip.sha256`。外部 `.zip.sha256` 是最终 ZIP 哈希的唯一权威记录，不放入 ZIP，从而避免归档自引用。`dist` 下的目录仍处在源码树内，只作为打包暂存和校验对象；功能验收必须把 ZIP 解压到仓库之外的独立可写目录，避免开发树的 `CMakeLists.txt` 让程序误选源码数据根。验证脚本会检查 Qt/MinGW DLL 与插件闭包、固定资产、空白数据库、许可文件和逐文件 SHA-256，并把包复制到系统临时独立目录，在移除 Qt/MSYS2 `PATH` 的子进程中启动 `SignalLab.exe --smoke-test`。同机隔离启动不能替代另一台干净 Windows 机器上的人工验收。

当前打包脚本保留本地 SQLite 的 `qsqlite.dll`，明确排除 `qsqlmysql.dll`，要求程序直接导入 `libmariadb.dll`，并收集、校验 MariaDB 客户端的完整 DLL 依赖和许可证。验证当前源码时应使用独立的 `-DistRoot`，保留原 `dist` 阶段 8 冻结资产。

当前 Qt Charts 开源包为 GPL-3.0-only；在项目权利人选择 GPL 兼容发布方式或提供商业 Qt 许可前，这个候选包仅用于本地验收，不公开分发。详见 [第三方声明](THIRD_PARTY_NOTICES.md) 与 [阶段 8 验证记录](docs/stage8-validation.md)。

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
