# Models

模型二进制不提交到 Git。运行默认 Vosk listener 前，将中文模型放到：

```text
models/vosk-model-small-cn-0.22/
```

当前基线使用 `vosk-model-small-cn-0.22`。更换模型时应记录来源和校验值，并重新验证唤醒误触发率、不同说话人识别率、CPU/内存和首次加载时间；不要用同一目录名覆盖成未知版本。
