# SignalLab 算法说明

本文记录阶段 3 已实现算法的定义、边界和版本。文中的公式与源码保持一致，目的是让结果可解释、可测试，也方便初学者修改。

## 1. 约定与同步分析流程

统一使用下列符号：

- `x[n]`：第 `n` 个输入样本。
- `N`：补零前的原始样本数。
- `fs`：采样率，单位为 Hz。
- `M`：FFT 长度，即不小于 `N` 的最小 2 次幂。
- `k`：频谱下标，单边谱中 `0 <= k <= M / 2`。
- `T`：FIR 系数个数。

阶段 3 的同步数据流为：

```text
Signal -> 可选预处理/FIR -> 加窗 -> 补零 -> radix-2 FFT
       -> 单边幅值谱和功率谱 -> 基本特征/频带能量/峰值/SNR/THD
```

所有公开算法先校验参数，并以 `Result<T>` 返回成功值或可理解的错误。算法层只依赖 `signallab_core` 和 C++20 标准库，不依赖 Qt。

## 2. 预处理

### 2.1 去直流

`removeDc` 先计算算术平均值，再从每个样本中减去它：

```text
mean = sum(x[n]) / N
y[n] = x[n] - mean
```

输出保留原信号的采样率和元数据。时间复杂度为 `O(N)`，额外空间为 `O(N)`。

### 2.2 峰值归一化

`normalizePeak` 查找最大绝对值，并缩放到 `targetPeak`：

```text
peak = max(abs(x[n]))
y[n] = (x[n] / peak) * targetPeak
```

`targetPeak` 必须有限且大于 0。全零信号没有可用的缩放基准，因此返回错误。时间复杂度为 `O(N)`，额外空间为 `O(N)`。

### 2.3 截取

`sliceSignal(signal, firstIndex, sampleCount)` 复制半开区间：

```text
[firstIndex, firstIndex + sampleCount)
```

截取长度必须大于 0，且整个区间必须位于原信号内。输出保留采样率和元数据。时间与额外空间复杂度均为 `O(sampleCount)`。

## 3. 窗函数与相干增益

`makeWindow` 支持 Rectangular、Hann 和 Hamming 三种对称窗。对 `N > 1`，第 `n` 个系数为：

```text
Rectangular: w[n] = 1
Hann:        w[n] = 0.5 - 0.5 * cos(2*pi*n/(N-1))
Hamming:     w[n] = 0.54 - 0.46 * cos(2*pi*n/(N-1))
```

为避免 `N - 1` 为 0，Hann 和 Hamming 在 `N = 1` 时明确返回 `[1]`。按上述对称公式，`N = 2` 的 Hann 窗是 `[0, 0]`，没有可用能量，因此 `computeSpectrum` 会返回错误；分析两个样本时应明确选择 Rectangular 或 Hamming。空窗是非法输入。

`applyWindow` 逐点计算 `y[n] = x[n] * w[n]`。窗生成和应用的时间、额外空间复杂度均为 `O(N)`。

相干增益是窗系数的算术平均值：

```text
CG = sum(w[n]) / N
```

频谱幅值使用 `CG` 修正窗造成的相干正弦幅值衰减。相干增益计算时间为 `O(N)`，额外空间为 `O(1)`。

## 4. radix-2 FFT 与自动补零

### 4.1 FFT 核心

`radix2Fft` 是自研的迭代 radix-2 Cooley–Tukey 前向 FFT：

1. 复制输入并进行位逆序重排。
2. 从长度 2 开始逐级执行蝶形运算。
3. 前向旋转因子采用负指数，即 `exp(-j*2*pi*k/M)`。

该底层函数只接受非空、长度为 2 次幂、实部和虚部均有限的输入。它不执行 `1/M` 缩放。时间复杂度为 `O(M log M)`，返回数组占用 `O(M)` 额外空间；算法原地执行蝶形，除此之外只使用常量级临时量。

### 4.2 `computeSpectrum` 的补零顺序

高层 `computeSpectrum` 接受任意非空长度：

```text
M = bit_ceil(N)
前 N 项 = x[n] * w[n]
后 M-N 项 = 0
```

窗按原始长度 `N` 生成并应用，然后才补零到 `M`。若 `N` 已是 2 次幂，则 `M = N`。补零只加密频率采样点，不增加原信号包含的实际频率分辨能力。

## 5. 单边幅值谱和功率谱

单边谱包含 `M/2 + 1` 个点，频率为：

```text
frequency[k] = k * fs / M
```

令 `X[k]` 为 FFT 结果，单边系数 `c[k]` 定义为：

```text
c[k] = 1，k 为 DC（0）或 Nyquist（M/2）
c[k] = 2，其他单边频点
```

当 `M = 1` 时只有 DC 点。幅值定义为：

```text
amplitude[k] = c[k] * abs(X[k]) / (N * CG)
```

这里使用原始长度 `N`，而不是补零长度 `M`。因此位于 FFT 频点上的相干正弦在加窗后仍可恢复其峰值幅度。

令窗能量 `WE = sum(w[n]^2)`，每个频点的功率贡献定义为：

```text
power[k] = c[k] * norm(X[k]) / (M * WE)
```

`power[k]` 的单位是输入单位的平方，它表示该频点对均方值的贡献，并不是“每 Hz 功率谱密度”。频带能量、SNR 和 THD 都使用同一组 `power[k]`，因此口径一致。

整个频谱计算由加窗、补零和 FFT 构成，时间复杂度为 `O(M log M)`，额外空间为 `O(M)`。

## 6. 基本特征与频带能量

`calculateBasicFeatures` 使用总体统计量，即方差分母为 `N`：

```text
mean       = sum(x[n]) / N
variance   = sum((x[n] - mean)^2) / N
rms        = sqrt(sum(x[n]^2) / N)
peakToPeak = max(x[n]) - min(x[n])
```

均值、平方和与方差累加使用 `long double` 中间量，最终结果为 `double`。时间复杂度为 `O(N)`，额外空间为 `O(1)`。

`calculateBandEnergy` 对闭区间 `[lowHz, highHz]` 内已有频点的功率直接求和：

```text
bandEnergy = sum(power[k])，其中 lowHz <= frequency[k] <= highHz
```

边界必须有限、顺序正确，并位于当前单边谱范围内。该值没有对落在两个频点之间的边界做插值。时间复杂度为 `O(B)`，额外空间为 `O(1)`，`B = M/2 + 1`。

## 7. 峰值检测与三点插值

`detectPeaks` 按以下步骤工作：

1. 在非端点频点中寻找严格大于左右相邻值、且幅值不小于 `minimumAmplitude` 的局部最大值。
2. 若峰及其左右幅值均大于 0，在对数幅值域做三点抛物线插值。
3. 按幅值从大到小处理候选峰；与已选峰距离小于 `minimumDistanceHz` 的较弱峰被抑制。
4. 最多保留 `maximumPeakCount` 个峰，最后按频率升序返回。

对左、中、右三个对数幅值 `L`、`C`、`R`，频点偏移为：

```text
delta = 0.5 * (L - R) / (L - 2*C + R)
delta 限制到 [-0.5, 0.5]
refinedFrequency = frequency[k] + delta * binWidth
refinedLogAmplitude = C - 0.25 * (L - R) * delta
refinedAmplitude = exp(refinedLogAmplitude)
```

无法安全插值时保留中心频点及其原始幅值。端点不参与三点峰值判断。时间复杂度为 `O(B log B + B*P)`，额外空间为 `O(B)`，其中 `P` 是最大返回峰数。

## 8. FIR 低通、高通和边界

FIR 设计要求：

- `T` 为不小于 3 的奇数。
- `fs` 有限且大于 0。
- 截止频率有限且严格满足 `0 < cutoffHz < fs/2`。

低通使用理想 sinc 冲激响应乘 Hamming 窗。令 `m = n - (T-1)/2`、`fc = cutoffHz/fs`：

```text
hIdeal[n] = 2*fc                                  ，m = 0
hIdeal[n] = sin(2*pi*fc*m) / (pi*m)              ，m != 0
hLow[n]   = hIdeal[n] * hamming[n]
```

随后用系数总和归一化，使低通在 DC 处的增益为 1。高通通过谱反转得到：

```text
hHigh[n] = -hLow[n]
hHigh[(T-1)/2] += 1
```

设计时间与额外空间复杂度均为 `O(T)`。

当前唯一的边界策略是 `ZeroPadding`。滤波采用因果卷积，信号起点之前的样本视为 0：

```text
y[n] = sum(h[k] * x[n-k])，0 <= k < T 且 n-k >= 0
```

输出与输入等长，不补偿线性相位 FIR 的群延迟 `(T-1)/2`。任意有限、非空系数都可以传给 `applyFir`；“奇数且至少 3 项”是设计函数的限制。直接卷积时间复杂度为 `O(N*T)`，输出额外空间为 `O(N)`。

## 9. SNR 和 THD 的项目定义

频点泄漏会让一个正弦的能量分散到相邻频点。因此两项指标都先找到离给定基频最近的非 DC 频点，再使用中心频点左右各 `guardBins` 个点的闭区间。区间会截断到 `[1, M/2]`，始终排除 DC。

### 9.1 SNR

令 `Pfund` 为基频保护区间的功率，`PnonDc` 为所有非 DC 频点的总功率：

```text
Pnoise = PnonDc - Pfund
SNR_dB = 10 * log10(Pfund / Pnoise)
```

本项目把谐波功率包含在 `Pnoise` 中，因此这个操作性定义同时惩罚噪声和失真。它不能与“先排除谐波再计算”的仪器 SNR 直接混用。若 `Pnoise` 恰好为 0，结果定义为正无穷。

时间复杂度为 `O(B)`，额外空间为 `O(1)`。

### 9.2 THD

`harmonicCount` 表示纳入检查的最高谐波阶数，默认检查基频以及第 2 至第 5 次谐波。只累计不超过当前单边谱最高频率的谐波：

```text
THD_percent = 100 * sqrt((P2 + P3 + ... + PK) / Pfund)
```

`Ph` 是第 `h` 次谐波保护区间的功率。保护区间不得相互重叠；THD 不把其余宽带噪声计入分子。时间复杂度为 `O(B + K)`，额外空间为 `O(1)`。

## 10. 参数校验与数值边界

各入口至少检查以下错误：

- 空样本、NaN、无穷大、非法采样率，或小到无法形成严格递增频率轴的采样率。
- 非法窗类型、空窗和非有限窗系数。
- 非 2 次幂 FFT 输入；高层频谱接口则负责安全补零。
- 不一致的 `Spectrum` 元数据、窗类型、数组长度、频率坐标、幅值或功率，以及运算过程中出现的非有限结果。
- 非法频带、峰值阈值、峰间距、FIR 截止频率和系数。
- 不存在有效基频功率、SNR/THD 保护区间越界或相互重叠。

正常的输入错误不抛异常，而是返回带 `ErrorCode` 和说明文字的失败结果。

## 11. 算法版本

版本字符串定义在对应公开头文件中。持久化阶段应保存实际调用的版本，而不是只保存应用版本。

| 模块 | 当前版本字符串 | 公开头文件 |
|---|---|---|
| 预处理 | `0.3.0` | `preprocessing.hpp` |
| 窗函数 | `0.3.0` | `window.hpp` |
| FIR | `0.3.0` | `fir_filter.hpp` |
| radix-2 FFT | `fft-radix2/1.0.0` | `fft.hpp` |
| 单边频谱 | `spectrum-one-sided/1.0.0` | `spectrum.hpp` |
| 基本特征与频带能量 | `features/1.0.0` | `features.hpp` |
| 峰值检测 | `peak-detector-log-parabola/1.0.0` | `peak_detector.hpp` |
| SNR/THD | `quality-metrics-power-bins/1.0.0` | `quality_metrics.hpp` |

## 12. 外部参考与运行时依赖

FFT、窗、FIR、特征和峰值算法均由本项目用 C++20 实现。运行和测试不调用 NumPy、SciPy、MATLAB、Octave、FFTW 或其他数值运行时。

NumPy/MATLAB 只允许在开发机上离线生成参考结果。若以后加入这类对照数据，应把输入、参考数值、生成工具版本和容差固化到测试资料中；正式构建和测试只能读取已固化数据，不能启动这些工具。

## 13. Release FFT 基准记录

此表必须在阶段 3 最终验收前用 Release 构建实际运行后填写。当前只提供记录位置，不用估计值代替测量值。

| 项目 | 记录 |
|---|---|
| 测量日期与时区 | `2026-09-08，Asia/Shanghai` |
| 被测代码提交 | `00326e8e009d7f2bde991c5aadc2c66d5fc4c72d` |
| 构建预设 | `release-mingw64` |
| 编译器 | `GCC 16.1.0（MINGW64）` |
| 构建类型与优化选项 | `Release，-O3 -DNDEBUG` |
| CPU | `Intel(R) Core(TM) i9-14900HX，24 核 / 32 逻辑处理器` |
| 操作系统 | `Microsoft Windows 11 家庭版 中文版，10.0.26200，64-bit` |
| 内存 | `16 GB（系统报告 16,773,996,544 bytes）` |
| 计时方法 | `std::chrono::steady_clock；3 次预热；每轮 20 次；独立运行 5 轮；报告五轮平均值的中位数和全部轮次中的最小单次值` |

| 输入 | 窗与操作 | 实测耗时 |
|---|---|---:|
| 48 kHz、65,536 点、1 kHz 正弦 | `Hann；computeSpectrum（含加窗、FFT、单边谱）` | 平均值中位数 `4.895 ms`；最小 `4.326 ms` |
| 65,536 点复数输入 | `radix2Fft` | 平均值中位数 `1.806 ms`；最小 `1.604 ms` |

计时区间从算法调用前开始，到 `Result` 返回后结束；结果析构不计时。每次计时后读取结果并累计校验和，避免结果成为未使用计算。五轮精度输出一致：插值后主峰为 `1000.011 Hz`、幅值为 `1.018`，频率误差 `0.011 Hz`，幅值误差 `1.776%`。本用例的频点间隔 `fs/N = 0.732421875 Hz`，因此两项均通过“频率误差不超过一个频点、幅值误差不超过 2%”的门槛。表内耗时和精度均按基准程序的三位小数输出记录，结果只代表上述机器与当次负载。
