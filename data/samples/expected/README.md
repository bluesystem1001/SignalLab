# 固化期望结果

本目录保存阶段 8 的只读参考结果。它们用于答辩复核和资产校验，不是运行时配置。

- `example_signal_reference.json` 固化 CSV 导入参数、内容哈希、样本哈希和默认分析结果。
- `example_signal_wav_reference.json` 固化 PCM16 WAV 样例的容器与导入元数据。

数值来自 Release 构建调用当前 `CsvReader` 与 `analyzeSignal` 的结果。修改样例、CSV 解析、信号哈希编码或算法定义时，必须同时说明原因、重新生成参考结果、执行全量测试，并更新 `data/samples/SHA256SUMS.txt`；不得只为让校验通过而手工放宽容差。

