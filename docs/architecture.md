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

## 阶段 2 的文件导入

加入阶段 3 算法层后的当前目标依赖为：

```text
SignalLab -> signallab_gui -> signallab_app -> signallab_algorithm -> core
                                      |
                                      +-> signallab_io -> core + Qt Core
```

GUI 只调用应用服务。为让阶段 2 的完整导入链保持直观，`ImportService` 暂时直接调用 `signallab_io`；该依赖是 [ADR 0006](adr/0006-stage2-import-service-boundary.md) 记录的阶段性例外，将在阶段 4 引入仓库抽象时移除。

当前文件导入的数据流是：

```text
文件选择与参数 -> ImportPreview -> 用户确认 -> ImportService
                                             -> 临时快照
                                             -> SHA-256 / CSV 或 WAV 解析
                                             -> data/imports/<hash>.<扩展名>
                                             -> SignalEntry -> 项目列表与波形
```

- `signallab_io` 负责 CSV、PCM16 WAV、SHA-256 和文件格式错误定位。
- `signallab_app` 的 `ImportService` 编排预览、去重、解析和原始文件存档；GUI 不直接解析文件。
- 项目中的原始文件使用内容哈希命名，存档位置使用项目相对路径；`sourcePath` 只记录本次会话选择的来源位置，不用于重新定位项目存档。
- 导入先写同目录临时快照，再原子重命名。只有解析和存档都成功后才把信号加入内存列表；失败路径会清理临时文件。
- 当前阶段按文件内容去重。重复文件会返回明确错误，也不会产生第二份原始文件。

CSV 支持逗号或制表符、可选表头、指定时间列/数值列，以及没有时间列时显式填写采样率。若使用时间列，其单位必须是秒，且解析器要求时间严格递增、近似等间隔。CSV 错误使用从 1 开始的行号和列号。

WAV 首版只接受 RIFF/WAVE 中的 PCM 16-bit 单声道或双声道。双声道可选择左、右或平均值，样本统一除以 `32768.0` 转换为归一化浮点值。WAV 错误包含字节偏移。

## 阶段 3 的同步算法闭环

`signallab_algorithm` 是只依赖 `signallab_core` 与 C++20 标准库的静态库。它包含预处理、窗函数、自研 radix-2 FFT、单边频谱、基本特征、峰值检测、FIR 和质量指标；不包含 Qt，也不直接负责界面状态。

GUI 只调用 `AnalysisService`，当前同步数据流是：

```text
所选 Signal -> AnalysisService -> 原始样本的基本时域特征
                             -> 去直流 -> Hann 窗 -> bit_ceil 补零
                             -> radix-2 FFT -> 单边幅值/功率谱
                             -> 峰值检测 -> SNR/THD
                             -> 频谱显示下采样 -> Qt Charts
```

基本特征使用原始 `Signal`，避免去直流改变用户看到的均值；频谱路径先去直流，减少 DC 对峰值查找的影响。默认峰值阈值是最强非 DC 幅值的 1%，最多返回 10 个峰。若没有可用主峰，或当前信号无法按已定义口径计算质量指标，`dominantPeak`、`snrDb` 或 `thdPercent` 保持为空，GUI 显示“不可计算”。非法分析选项会直接返回失败。

归一化、截取和 FIR 低通/高通已经是独立公开算法，但阶段 3 GUI 没有为它们加入复杂参数面板。这样既保留课程算法实现，也让首个闭环保持直观。所有公式、边界、复杂度和算法版本见 [算法说明](algorithms.md)。

## 显示下采样

原始 `Signal` 和完整 `Spectrum` 都不会因为绘图而改变。小信号/小频谱逐点显示；较大数据按图表像素宽度分桶，每桶按原索引顺序保留最小值和最大值。因此提交给 Qt Charts 的点数最多约为 `2 × 像素宽度`，时域瞬态和频域尖峰仍能保留。

文件预览和导入在阶段 2 仍是同步调用。阶段 5 引入任务系统后，大文件 I/O 会移出 UI 线程；阶段 7 再补充窗口缩放后的延迟重绘。

阶段 2 的 `SignalEntry` 列表只保存在内存中，程序重启后不会从 `data/imports/` 自动恢复；该能力属于阶段 4。当前按整个原始文件哈希判重，所以同一 CSV 不能再次选择另一数值列，同一 WAV 也不能再次选择另一声道。阶段 4 会把原始文件记录和逻辑信号记录分开。

## 后续边界

数据库、任务系统和脚本模块尚未实现。阶段 3 分析有意保持同步；阶段 5 会把同一个应用服务调用移入可排队、取消和重试的后台任务。后续模块会继续遵守“核心与算法模块不依赖 GUI”的方向。
