# SignalScript 0.6 语言与运行时

SignalScript 是 SignalLab 的受限离线分析语言。语言版本仍为 SignalScript 0.6，当前承载它的应用版本为 SignalLab `1.0.0-rc1`。它提供词法分析、递归下降语法分析、带恢复的诊断、静态类型检查、解释执行、受控文件访问和 SQLite 执行历史，只调用 SignalLab 已登记的算法与导入服务，不提供循环、网络、进程、Shell、动态库或任意文件系统接口。

阶段 6 交付了语言、运行时和应用服务边界；阶段 7 已把它们接入基于 Model/View 的桌面主窗口。当前 SignalScript 页提供脚本编辑、后台运行与取消、诊断点击跳转、变量/输出/AST 查看、结构化信号和频谱图，以及执行历史和源码重新载入。

## 快速示例

仓库中的 [`data/samples/example_analysis.sig`](../data/samples/example_analysis.sig) 展示了完整流程：

```signalscript
# 导入、预处理、分析、绘图并导出
load "data/samples/example_signal.csv" as raw;
clean = raw |> normalize() |> lowpass(cutoff=100, order=63);
spec = clean |> fft(window="hann");
result = features(spec);
plot clean as "Filtered waveform";
plot spec as "One-sided amplitude spectrum";
save result to "example_signal_features.json";
```

脚本中的路径始终相对于宿主注入 `ScriptService` 的 `ScriptFileGateway` 所配置的输入根或输出根，而不是相对于脚本文件自身。上例假定输入根是项目根目录；若宿主把输入根配置为 `data/samples/`，相同输入应写成 `load "example_signal.csv" as raw;`。SignalLab `1.0.0-rc1` 的桌面宿主把输出根固定为 `data/script_outputs/`，因此 `save result to "example_signal_features.json";` 会尝试创建 `data/script_outputs/example_signal_features.json`，且不会覆盖同名文件。

## 词法规则

- 关键字为 `let`、`load`、`save`、`plot`、`as`、`to`、`true` 和 `false`。
- 标识符使用 ASCII 形式 `[A-Za-z_][A-Za-z0-9_]*`，区分大小写。
- 数字支持整数、小数、负号和科学计数法；非有限值和超出范围的字面量会被拒绝。
- 字符串使用双引号，支持 `\n`、`\r`、`\t`、`\"` 和 `\\` 转义；字符串不能跨行。
- `#` 开始行注释，直到 CR、LF 或 CRLF 行尾。
- 标点为 `=`、`(`、`)`、`,`、`;` 和管道运算符 `|>`。
- 源文件必须是有效 UTF-8。行、列均从 1 开始；UTF-8 多字节字符按一个字符计列，CRLF 按一个换行计数。

## 语法

SignalScript 0.6 的语法如下：

```ebnf
program      = { statement } ;
statement    = load_stmt | save_stmt | plot_stmt | assignment | expr_stmt ;
load_stmt    = "load" STRING [ "as" IDENT ] ";" ;
save_stmt    = "save" expression "to" STRING ";" ;
plot_stmt    = "plot" expression [ "as" STRING ] ";" ;
assignment   = [ "let" ] IDENT "=" expression ";" ;
expr_stmt    = expression ";" ;
expression   = pipeline ;
pipeline     = primary { "|>" call } ;
primary      = NUMBER | STRING | BOOLEAN | IDENT | call
             | "(" expression ")" ;
call         = IDENT "(" [ arguments ] ")" ;
arguments    = argument { "," argument } ;
argument     = [ IDENT "=" ] expression ;
```

每条语句必须以分号结束。管道会把左侧结果绑定到右侧调用的第一个参数，因此：

```signalscript
clean = raw |> lowpass(cutoff=100, order=63);
```

等价于：

```signalscript
clean = lowpass(raw, cutoff=100, order=63);
```

位置参数必须出现在命名参数之前，同一参数不能重复绑定。`let` 用于显式首次声明；不带 `let` 的赋值也可以创建变量。`let` 不允许重复声明，同一个已有变量也不能在后续赋值中改变类型。`load` 省略 `as` 时仍会执行读取，但不会把结果放入可引用变量，通常应写别名。

## 类型

静态检查器使用下列类型：

| 类型 | 含义 |
|---|---|
| `Number` | 有限双精度数值 |
| `String` | UTF-8 字符串 |
| `Boolean` | `true` 或 `false` |
| `Signal` | 带采样率和元数据的一维信号 |
| `Spectrum` | 完整分析结果中的单边频谱及其来源信号 |
| `FeatureSet` | 可序列化的特征名、值和单位集合 |
| `Unit` | 不产生可复用值的语句结果 |

`plot` 只接受 `Signal` 或 `Spectrum`；`save` 只接受 `Signal`、`Spectrum` 或 `FeatureSet`。未知变量、未知函数、参数名错误、缺参、重复参数和类型不匹配都会在执行前成为语义诊断。

## 内建函数和语句

运行时只有固定白名单，不按字符串查找外部代码。

| 名称 | 参数 | 返回值与规则 |
|---|---|---|
| `normalize` | `signal: Signal`；可选 `target: Number = 1` | 返回峰值归一化后的 `Signal`；`target` 必须为有限正数 |
| `lowpass` | `signal: Signal`、`cutoff: Number`；可选 `order: Number = 63` | 使用现有 FIR 低通算法返回 `Signal`；截止频率必须为有限正数且低于 Nyquist，`order` 必须是 3—65535 的奇整数 |
| `fft` | `signal: Signal`；可选 `window: String` | 返回 `Spectrum`；窗口只接受 `"rectangular"`、`"hann"` 或 `"hamming"`，默认沿用分析服务选项 |
| `features` | `input: Signal | Spectrum`；可选 `fundamental: Number` | 返回 `FeatureSet`；信号产生基本时域特征，频谱还包含 FFT 点数、可用的主峰、SNR 和 THD；显式基频必须为有限正数、位于频谱范围内且仅适用于 `Spectrum`，用于 SNR/THD 重算；基频能量不足或保护区重叠时执行失败 |
| `load` | 相对输入路径，可选别名 | 支持 `.csv`、`.tsv` 和 PCM16 `.wav`，通过脚本专用的 `ImportService::openCsv/openWav` 使用对应默认导入选项 |
| `plot` | `Signal | Spectrum`，可选标题 | 产生结构化绘图请求；SignalLab 1.0.0-rc1 的主窗口将其显示为交互图表 |
| `save` | `Signal | Spectrum | FeatureSet` 和相对输出路径 | 信号与频谱输出 CSV 文本，特征集输出 JSON 文本 |

`fft` 复用 GUI 分析路径的 FFT、频谱、峰值和质量指标实现，而不是维护第二套计算公式。`plot` 只把不可变信号或频谱及标题放入执行结果；主窗口在 GUI 线程消费请求，并按图表当前可见区和像素宽度降采样，保留信号极值或频谱峰值。

`save` 使用经典区域设置和 17 位数字精度生成 UTF-8 文本。`Signal` 的 CSV 表头为 `time_seconds,value`；`Spectrum` 的 CSV 表头为 `frequency_hz,amplitude,power`；`FeatureSet` 使用 `{"features":[{"name": ..., "value": ..., "unit": ...}]}` 结构。JSON 没有 NaN 或无穷大字面量，因此非有限特征值写为 `null`，内存中的计算结果保持不变。

SignalScript 0.6 的 `load` 语句尚不接受解析选项：`.csv` 默认有表头、第一列为秒时间、第二列为数值且使用逗号；`.tsv` 使用相同列规则和制表符；双声道 WAV 默认取左右平均。需要其他列、显式采样率或单独声道时，当前仍应使用已有导入界面。

脚本加载采用可重放语义。`openCsv/openWav` 以文件内容 SHA-256 和规范化解析选项组成逻辑来源键：再次执行同一脚本且输入内容未变时，返回已经持久化的逻辑信号，而不是把“重复导入”当作失败；同一路径的文件内容若已改变，会得到新的内容哈希和新的逻辑信号。该路径只返回执行所需的 `SignalEntry`，不会读取或修改面向 GUI 的 `ImportService::entries()`；主窗口在脚本完成事件回到 GUI 线程后接纳新信号并刷新项目模型。

## 文件系统安全边界

`ScriptFileGateway` 是解释器唯一可见的文件能力。宿主必须分别配置输入根和输出根：

- `load` 只能读取输入根内已存在的普通文件。
- `save` 只能在输出根内创建新文件；不会覆盖已有目标。
- 空路径、绝对路径、根路径、缺少文件名、`..` 遍历、Windows 盘符/备用数据流，以及经过符号链接或 junction 逃逸根目录的路径都会被拒绝。
- 输出先写入私有临时文件，再以“不覆盖”的原子发布步骤生成目标；取消或失败会清理临时文件。
- 输出文本必须是有效 UTF-8，长写入在安全点响应 `std::stop_token`。

解释器没有原始 `std::filesystem`、网络、Shell、子进程或动态加载入口。扩大输入根或输出根是宿主应用的显式配置决定，不是脚本能够自行完成的操作。

## 诊断、恢复与取消

前端诊断与运行时问题按阶段使用稳定前缀：

| 前缀 | 阶段 | 示例 |
|---|---|---|
| `LEX` | 词法 | 非法字符、未闭合字符串、非法 UTF-8 或数字 |
| `PAR` | 语法 | 缺少分号/右括号、管道后不是调用 |
| `SEM` | 语义 | 未知变量、类型不匹配、无效参数范围 |
| `RUN` | 独立的运行时问题 | 文件不可用、算法失败、输出冲突或取消 |

每条 `LEX/PAR/SEM` 诊断以及可选的 `RUN` 问题都携带源范围，错误展示使用从 1 开始的行、列。解析器遇到语法错误后跳到下一个分号或文件末尾，再继续解析后续语句，以便一次返回多个独立问题；只要词法、语法或语义阶段存在错误，就不会进入解释执行。历史记录保存按源顺序最早的前端错误，或实际运行时问题的位置和代码/消息。

运行时通过 `std::stop_token` 协作取消。已观察到取消的执行以 `CANCELLED` 结束；已经成功完成并提交的执行不会被晚到的取消请求倒退。

为避免受限脚本耗尽进程栈或尝试不合理分配，表达式递归深度和单条管道阶段数均设有 256 的上限，脚本可用 FIR 阶数也受上述范围约束。解析会在超限语句的下一个分号恢复；意外的标准库/运行时异常会转换为 `RUN027` 失败并完成既有历史记录。基础特征、预处理、质量指标、FFT、FIR、序列化和文件写入均在适当安全点观察停止令牌。

## 执行历史

`ScriptService` 在词法分析之前创建 `script_history` 记录，因而未闭合字符串等有效 UTF-8 输入中的前端错误也能追溯。仓库会在创建记录时先验证 UTF-8；非法 UTF-8 可由直接使用 Lexer 的调用得到 `LEX005`，但通过 `ScriptService::execute()` 时会作为外层输入失败返回且不会创建历史记录。一次脚本执行遵循：

```text
创建记录：RUNNING
    ├─ 全部完成：SUCCEEDED
    ├─ 诊断或运行错误：FAILED
    └─ 观察到停止请求：CANCELLED

下次启动清理遗留 RUNNING：INTERRUPTED
```

记录包含原始脚本文本、原始 UTF-8 字节的 lowercase SHA-256、开始/结束 UTC 时间、状态、可选错误行列和错误消息。终态更新带 `WHERE status = 'RUNNING'` 条件并与操作日志处于同一事务，保证一条记录只完成一次。启动清理把上一进程遗留的 `RUNNING` 记录改为 `INTERRUPTED`，不伪造源位置。

迁移前的旧 `script_history` 行保留原编号和原状态；其空 `source_hash` 用于区分旧记录。读取新的非空哈希记录时，仓库会重新计算并校验脚本文本哈希。

## SignalLab 1.0.0-rc1 桌面接入

- `BackgroundScriptService` 把执行投递到计算队列，把历史查询投递到 I/O 队列；工作线程只产生事件，窗口模型和控件只在 Qt GUI 线程更新。
- 主窗口同时只允许一个活动脚本。可以从编辑器启动或取消，也可以在任务中心查看状态、错误建议和重试来源；取消通过 `std::stop_token` 沿既有运行时安全点传播。
- 诊断列表显示稳定代码、消息和从 1 开始的行列；点击条目会把编辑器光标移动到相应 UTF-16 位置，包括前文含多字节 Unicode 字符的情况。
- 成功结果展示变量快照、实际输出路径、稳定 AST 和全部结构化绘图请求。信号图与频谱图支持可见区重采样、缩放、选择、十字光标、复位和 PNG 导出。
- 脚本历史来自 `script_history`，可查看状态、哈希、UTC 时间与错误位置，并把选中记录的源码重新载入编辑器。
- `0.7.3` 的项目信号删除不改变脚本语法或运行时。若脚本完成事件携带的信号在等待 GUI 接纳期间已被删除，`ImportService` 的记录编号 tombstone 会忽略该迟到条目，避免产生数据库中不存在的幽灵信号。
- `0.7.4` 的双信号同轴对比和时间轴滑动条只改变主时域页的显示状态，不改变脚本 `plot`、信号值、采样率或运行时语义。

## 当前交付边界

- 可复用入口是 `ScriptService::execute()`、历史查询和启动中断清理接口；桌面入口通过 `BackgroundScriptService` 包装这些同步能力。
- 解释结果包含稳定 AST 文本、诊断、可选运行时问题、变量快照、绘图请求和实际输出路径。
- 阶段 6 的语言与运行时测试覆盖前端、文件网关、数据库生命周期、资源限制、取消和端到端脚本路径；`0.6.0` 在 Debug 与 Release 下均通过 216/216 项测试，并以 `v0.6`、`stage-6-complete` 和仓库外 Git bundle 固化。
- SignalLab `0.7.0` 的阶段 7 基线在 Debug 与 Release 全量测试均为 233/233；`0.7.1` 删除增量完成后均为 242/242；`0.7.2` 可用性增量完成后均为 255/255；`0.7.3` 项目信号删除增量和 `0.7.4` 双信号显示增量完成后均为 268/268。`1.0.0-rc1` 阶段 8 候选版均为 273/273。这些版本都未改变 SignalScript 0.6 的语言与运行时行为。
- 最终真实 GUI 的视觉检查和连续两轮完整流程截至 2026-10-03 尚未执行；自动化测试结果不用于替代该人工验收。
