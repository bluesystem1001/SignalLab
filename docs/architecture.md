# SignalLab 架构说明

本文只描述当前已经实现的结构。新阶段完成后再补充对应模块，避免文档先于代码膨胀。

## 阶段 1 的最小纵向切片

当前程序的数据流是：

```text
固定模拟参数 -> 信号生成器 -> Signal -> MainWindow -> Qt Charts
```

- `signallab_core` 保存信号模型和模拟信号生成器，只依赖 C++ 标准库。
- `signallab_gui` 读取 `Signal`，把已有的数据和范围交给 Qt Widgets/Qt Charts 显示。
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

## 后续边界

CSV/WAV、算法、数据库、任务系统和脚本模块尚未在阶段 1 中实现。它们会按实施计划逐阶段加入，并继续遵守“核心模块不依赖 GUI”的方向。
