# 阶段 8 固定资产说明

本文定义 `1.0.0-rc1` 候选版随源代码和 Windows 包一起复核的固定资产。固定指“输入、参数、结果、哈希和生成方法都可追溯”，并不表示以后永远不能修改；任何修改都必须同步参考结果和测试证据。

## 1. 资产清单

| 资产 | 路径 | 用途 |
|---|---|---|
| CSV 样例 | `data/samples/example_signal.csv` | 21 点、1 kHz 采样、约 50 Hz 正弦，覆盖时间列推导采样率 |
| WAV 样例 | `data/samples/example_signal_pcm16.wav` | 21 帧、1 kHz、单声道 PCM16，覆盖 WAV 导入 |
| 完整 SignalScript 示例 | `data/samples/example_analysis.sig` | 展示 load、管道、FFT、features、plot 和 save；输出不覆盖 |
| 可重复演示脚本 | `data/samples/demo_analysis.sig` | 不执行 save，可在答辩中连续运行 |
| 期望结果 | `data/samples/expected/*.json` | 固化导入元数据、默认分析值和数值容差 |
| 空白数据库 | `data/templates/blank.sqlite3` | 已迁移到 003、无业务数据的 schema-only 模板 |
| 默认配置快照 | `config/default.json` | 记录候选版硬编码默认值与安全边界 |
| 文件哈希 | `data/samples/SHA256SUMS.txt` | 固化样例、模板和期望结果字节 |
| 第三方许可 | `THIRD_PARTY_NOTICES.md`、`licenses/` | 组件清单、原始许可文本及哈希 |
| 用户手册 | `docs/user-guide.md` | 安装、操作、数据与故障恢复 |
| 测试计划 | `docs/test-plan.md` | 自动化、稳定性、故障注入、基准和干净机验收 |
| 答辩脚本 | `docs/demo-script.md` | 2–3 分钟无剪辑录屏流程 |
| 验证记录 | `docs/stage8-validation.md` | 候选版实测结果与仍待人工完成的门禁 |
| 技术参考 | `docs/algorithms.md`、`docs/signalscript.md`、`docs/adr/0005-clion-bundled-gdb.md` | 保证发布包内 README 的技术链接完整 |
| 工具链快照 | `docs/msys2-package-snapshot.txt` | 记录冻结的 MSYS2/CLion 工具与依赖复核结果 |

证据分三层保存：

- Windows 包内携带可离线阅读的摘要文档、逐文件 `SHA256SUMS.txt`、固定资产和许可文本；
- 源码树保留 `tools/` 下的生成/校验脚本以及 `cmake-build-*` 下的原始测试、覆盖率与稳定性日志，这些开发证据不复制进运行包；
- `dist/` 中与 ZIP 同名的 `.zip.sha256` 作为归档外部 sidecar，是最终 ZIP 哈希的唯一权威记录。

## 2. 默认配置的含义

`config/default.json` 是候选版的机器可读配置快照，当前桌面程序**不会在运行时读取或覆盖它**。实际值仍由 C++ 默认参数和宿主路径绑定决定。文件中的 `status` 必须保持为 `frozen-reference-not-runtime-loaded`，避免用户误以为编辑 JSON 会改变程序行为。

若后续真的加入配置加载，应先定义 schema、错误回退和迁移策略，再删除该状态标记并增加对应测试；不能悄悄把参考文件变成控制文件。

## 3. 空白数据库模板

`data/templates/blank.sqlite3` 由仓库迁移顺序生成，包含：

- `schema_migration` 中恰好三条记录：`001_init.sql`、`002_task_scheduler.sql`、`003_signal_script.sql`；
- `file_version`、`signal_source`、`signal_record`、`analysis_task`、`feature_result`、`script_history` 和 `operation_log` 七张空业务表；
- 通过 `PRAGMA integrity_check` 和 `PRAGMA foreign_key_check` 的结构。

它不能命名或放置为交付包中的活动 `data/signallab.sqlite3`。若用户明确需要空项目，应先退出程序，再手工复制模板到该活动路径。由于文件在启动前已经存在，程序不会写入内置 1 kHz 演示信号。

Windows 包本身不得携带运行过的活动数据库、`-wal`、`-shm`、导入存档或脚本输出。

## 4. 生成与校验

资产生成会覆盖固定二进制、哈希清单和第三方许可副本，因此仅在样例、迁移、工具链或依赖经过评审后执行：

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass `
  -File .\tools\freeze_stage8_assets.ps1 -Force
```

普通构建和打包前只运行只读校验：

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass `
  -File .\tools\verify_stage8_assets.ps1
```

校验项包括 JSON 可解析性、CSV/WAV 固定属性、全部 SHA-256、数据库迁移集合/空表/integrity/FK，以及第三方许可副本哈希。失败时不得自动“接受新哈希”；应先确认差异来源。

## 5. 许可证冻结边界

许可证副本来自本机已安装的 MSYS2 包，不从网页转抄。Qt Charts 的开源包提供 GPL-3.0-only；当前仓库没有由权利人选择项目级兼容授权。因此 `1.0.0-rc1` 可以用于本地打包和验收，但在权利人作出许可决定或提供商业 Qt 许可之前，公共分发仍是明确的发布阻塞项。详见根目录 `LICENSE` 与 `THIRD_PARTY_NOTICES.md`。

## 6. 变更规则

修改任一固定资产时，至少完成：

1. 说明修改原因和兼容性影响；
2. 重新生成受影响的二进制和 SHA-256；
3. 更新期望结果与用户可见文档；
4. 运行资产校验、Debug/Release 全量测试和相关专项测试；
5. 若依赖或 DLL 闭包变化，重新收集许可证并复核发布边界。
