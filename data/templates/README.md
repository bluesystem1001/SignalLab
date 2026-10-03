# 空白项目数据库模板

`blank.sqlite3` 是阶段 8 固定的 schema-only 模板：已记录迁移 `001_init.sql`、`002_task_scheduler.sql` 和 `003_signal_script.sql`，所有业务表均为空。

它不会被程序直接打开。需要一个没有内置演示信号的空项目时，请先退出 SignalLab，再把模板复制到发布目录的 `data/signallab.sqlite3`。不要在程序运行期间替换活动数据库，也不要把运行后产生的 `-wal`、`-shm` 文件当成交付资产。

模板由 `tools/freeze_stage8_assets.ps1` 生成，并由 `tools/verify_stage8_assets.ps1` 检查迁移集合、空表、`integrity_check` 和 `foreign_key_check`。其 SHA-256 同时记录在 `data/samples/SHA256SUMS.txt`。

