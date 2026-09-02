# Common Protocol Core

`03_common_core` 保存 sender 与 receiver 共同使用的线协议定义。目前唯一公共头文件是：

```text
include/gwv3_common/protocol.hpp
```

## Responsibility

- 定义媒体包 magic、版本、固定头、流类型和标志位。
- 提供安全的序列化、反序列化和大小检查。
- 保持 sender 与 receiver 对字段宽度、字节序和 payload 布局的一致理解。

这里不放采集、编码、网络线程、录制或业务状态机。组件特有逻辑分别属于 `01_sender_linux` 和 `02_receiver_linux`。

## Compatibility Rules

1. 已发布字段不能静默改变含义、类型、偏移或字节序。
2. 新字段优先追加，并通过版本或 `header_size` 保持旧解析器可判定。
3. 解析输入前必须检查长度、payload 上限和整数溢出。
4. 改动协议时必须同时更新 sender、receiver、[API reference](../04_docs/api-reference.md) 和兼容性测试。
5. 不允许在 sender/receiver 各自复制一份结构体定义。

协议查表见 [api-reference.md](../04_docs/api-reference.md)，数据流语义见 [data-pipeline.md](../04_docs/data-pipeline.md)。
