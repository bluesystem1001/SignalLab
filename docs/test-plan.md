# SignalLab 阶段 8 测试计划

## 1. 目标与适用范围

阶段 8 的测试不再只证明单个函数正确，而要证明同一候选版能被重复构建、持续运行、打包并在没有 IDE 的目录中验收。范围包括核心/算法/I/O/数据库/任务/脚本/GUI、固定资产、发布 DLL 闭包和关机生命周期。

候选版的发布判定依赖顺序为：静态资产检查 → Debug 全量 → Release 全量 → 覆盖率 → 故障注入 → 30 分钟稳定性 → Release 基准 → Windows 包验证 → 真实桌面人工验收。为节约冻结时间，彼此独立的证据可以提前采集，但任一上游门禁失败时，不得越级宣布候选版通过。

## 2. 自动化测试矩阵

| 类别 | 主要证据 | 最低要求 |
|---|---|---|
| 单元测试 | CTest 中 core、algorithm、I/O、task、parser、database、GUI 单元目标 | 全部通过；总数不少于 30 |
| 集成测试 | 导入→持久化→分析、脚本执行、后台服务、GUI runtime | 全部通过，无孤儿文件或半成品数据库记录 |
| 回归测试 | 阶段 2–7 的导入、追溯、取消、删除、FIR、双信号和滑动条用例 | Debug/Release 都通过 |
| 并发测试 | 调度器关闭、观察者重入、取消/提交边界、数据库线程归属 | 重复执行无死锁、崩溃或状态倒退 |
| 固定资产 | `tools/verify_stage8_assets.ps1` | 样例/期望/配置/数据库/许可证全部通过 |
| 发布包 | `tools/verify-windows-package.ps1` | DLL/插件闭包完整，无活动数据库，可隔离启动并正常退出 |

Debug 与 Release 使用仓库 CMake preset，且执行前把 `E:\MSYS2\mingw64\bin` 放在当前进程 `PATH` 首位，避免加载其他 MinGW 运行库。

覆盖率使用 `stage8-coverage-mingw64` preset 的 GCC `--coverage -O0` 配置。该 test preset 明确排除会被插桩改变实时阈值的 `Stage5GuiRuntimeTest.ThreeLargeAnalysesKeepTheQtEventLoopResponsive`；它仍必须在未插桩的 Debug/Release 全量门禁通过。完成测试后运行：

```powershell
python .\tools\collect_stage8_coverage.py --build-dir .\cmake-build-stage8-coverage-mingw64 --gcov E:\MSYS2\mingw64\bin\gcov.exe --threshold 70
```

汇总器只统计 `src/` 下非 GUI 生产模块，要求 algorithm、app、core、database、io、parser、task 每个模块都达到 70%，不能用总体平均值掩盖单模块不足。

## 3. 故障注入门禁

30 分钟稳定性运行前先执行明确的故障用例，至少覆盖：

- CSV/WAV 不存在、截断、非法头、非法列、非有限数值和不等间隔时间；
- 数据库存储/迁移失败、事务中途失败、异常遗留 PENDING/RUNNING、损坏 BLOB/hash；
- 脚本词法/语法/语义错误、路径越界、符号链接逃逸、输出冲突和取消；
- 排队取消、运行中取消、完成提交后的迟到取消、关闭与回调竞态；
- FIR 非法截止频率/抽头数，FFT/峰值/质量指标的空值和边界；
- 分析历史、任务快照和项目信号删除的否/是、运行中禁用、事务回滚、共享存档与清理警告；
- GUI 空列表、失效选择、对比信号删除、极端 X 轴范围和复位后状态清理。

`tools/run_stage8_stability.ps1` 的启动门禁应选择约 30 项代表性测试；完整 CTest 仍在前置门禁执行，不能用这 30 项替代全量结果。

## 4. 30 分钟稳定性运行

默认持续时间为 30 分钟，使用 Release 构建。脚本提供三种运行模式：

1. **Full**：持续重复完整 CTest，发现跨模块状态污染；
2. **Critical**：持续重复数据库、后台服务、脚本和 GUI 主链路；
3. **Mixed**（默认）：第 1 轮执行完整 CTest，随后从首轮起每隔 5 轮再执行一次完整 CTest（第 1、6、11……轮），其余轮次执行关键组；`FullSuiteInterval` 改变时按同一规则调整间隔。

每轮必须记录开始/结束 UTC、类型、退出码和耗时；输出至少包括 `ctest.log`、`iterations.csv`、`summary.json` 和 `summary.txt`。任何非零退出、崩溃、超时或脚本自身异常都使该轮及整个稳定性门禁失败。运行中不得静默重试失败轮次后只保留成功结果。

若受现场时间限制执行短时预检，记录必须明确写“预检”，不能冒充 30 分钟验收。

## 5. Release 性能基准

在固定机器、Release、空闲负载下记录：

- 65,536 点正弦的 `computeSpectrum`（含 Hann、补零、FFT 和单边谱）；
- 65,536 点复数输入的 radix-2 FFT；
- 10 MiB CSV 的读取/解析（同时记录实际字节数、样本数、采样率和校验和）。

FFT 基准按既有口径预热 3 次，每轮 20 次并执行 5 轮，记录五轮平均值的中位数和全局最小单次值；CSV 基准预热 1 次并正式读取 5 次，记录平均值、最小值和吞吐量。基准不是硬实时承诺；若与既有记录差异超过 20%，先检查 CPU、电源策略、杀毒扫描、构建类型和工具链，再判断是否回归。

## 6. Windows 发布包验收

发布包必须从 Release 可执行文件通过 `windeployqt6.exe` 生成，并由验证脚本检查：

- `SignalLab.exe`、Qt DLL、MinGW 运行库和所有非系统传递 DLL；
- `platforms/qwindows.dll`、`sqldrivers/qsqlite.dll` 与所需 `imageformats` 插件；
- `migrations/`、`data/samples/`、`data/templates/blank.sqlite3`、`config/default.json`、用户文档和许可证；
- 不包含 `data/signallab.sqlite3`、`*.sqlite3-wal`、`*.sqlite3-shm`、导入存档、脚本输出、测试可执行文件或编译中间文件；
- 在清理 Qt/MSYS2 PATH 的隔离子进程中可以启动，创建自己的活动数据库，并在自动关闭后返回 0。

真正的“干净机器无需 IDE”仍需在另一台 Windows 10/11 x64 或干净虚拟机上手工复核；同机清理 PATH 的 smoke test 是必要但不充分的证据。

## 7. 真实 GUI 人工验收

按 README 中的 `1.0.0-rc1` 最终 GUI 清单连续执行两轮，并额外确认：

- 从发布包启动而不是构建目录；
- 2–3 分钟答辩脚本能连续完成并正常退出；
- 125%/150% DPI、键盘焦点、图例、异单位轴标题和确认框默认按钮可辨；
- 导出 PNG 可打开，空白数据库模板复制后不会自动生成演示信号；
- 第二轮重启后导入、FIR、历史和删除结果符合持久化约定。

人工结果必须记录操作者、日期、系统/DPI、包 SHA-256、每项通过/失败和截图/录像位置。自动化 GUI runtime 不等同于人工点击验收。

## 8. 证据记录模板

| 门禁 | 命令/环境 | 结果 | 日志或产物 |
|---|---|---|---|
| 资产校验 | `tools/verify_stage8_assets.ps1` | 通过，419 项 | 固定样例、期望、空白库、配置与许可摘要 |
| Debug 全量 | 绝对路径 CLion 4.2.2 `ctest --preset dev-mingw64 --output-on-failure` | 通过，273/273，13.28 秒 | CTest 输出 |
| Release 全量 | 绝对路径 CLion 4.2.2 `ctest --preset release-mingw64 --output-on-failure` | 通过，273/273，10.82 秒 | CTest 输出 |
| 故障注入 | stability 启动门禁 | 通过，30/30 | `cmake-build-release-mingw64/stage8-stability-final/ctest.log` |
| 30 分钟稳定性 | `tools/run_stage8_stability.ps1` | 通过，1,805.262 秒；231 轮/24,732 个测试实例，0 失败 | `stage8-stability-final/iterations.csv`、`summary.*`、`ctest.log` |
| 覆盖率 | 绝对路径 CLion 4.2.2 + `tools/collect_stage8_coverage.py` | 272/272 通过，17.43 秒；总体 78.54%，各模块均不低于 70% | 冻结证据：`cmake-build-stage8-coverage-clion-rc1/stage8-coverage/`；preset 默认复现路径为无 `-clion-rc1` 目录 |
| Release 基准 | FFT + 10 MiB CSV | 通过，FFT 中位 3.251 ms；CSV 中位 161.136 ms | [阶段 8 验证记录](stage8-validation.md) |
| 发布包校验 | package + verify scripts | 通过；155 文件/38 DLL，隔离 smoke 和全新解压复验均通过 | 包内清单、包外 `.zip.sha256`、验证器控制台记录 |
| 真实 GUI 两轮 | README 清单 | 待执行 | 人工记录、录像 |

只有实际执行后才能把“待执行”替换为通过；版本号、测试总数、耗时和包哈希必须从日志抄录，不凭估计填写。
