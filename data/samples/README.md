# 示例信号与阶段 8 固定资产

`example_signal.csv` 是一个便于手工验证导入流程的小型示例：

- 分隔符：逗号
- 第一行：表头
- 时间列：第 1 列
- 数值列：第 2 列
- 推导采样率：1000 Hz
- 内容：约一个周期的 50 Hz 正弦信号

从主窗口单击“导入 CSV”，按上面的选项填写即可。确认预览后，信号会出现在左侧项目列表中；再次导入同一个文件会显示重复文件提示。

同目录还包含：

- `example_signal_pcm16.wav`：与 CSV 波形对应的 1 kHz 采样率、单声道 PCM16 WAV，可直接验证 WAV 导入。
- `demo_analysis.sig`：可重复运行且不写文件的现场演示脚本。
- `example_analysis.sig`：包含 `save` 的完整语言示例；输出已存在时会按安全策略拒绝覆盖。
- `../templates/blank.sqlite3`：已经应用 `001_init.sql` 至 `003_signal_script.sql`、但所有业务表均为空的数据库模板。它不是活动数据库；若要使用，应在应用未运行时复制为发布目录下的 `data/signallab.sqlite3`。因为该文件已存在，应用不会自动加入内置正弦信号。
- `expected/`：CSV/WAV 导入和默认分析的固定参考结果。
- `SHA256SUMS.txt`：上述交付资产的 SHA-256 清单。

运行 `tools/verify_stage8_assets.ps1` 可同时检查文件哈希、WAV 头、JSON、空白数据库迁移版本/空表/integrity/FK，以及第三方许可证副本。
