# claude-esp32-agent

## 这是什么

Claude 语音助手，运行在 Waveshare ESP32-S3-Touch-AMOLED-1.75（466×466 圆形 AMOLED）上。屏幕显示像素风格的 Clawd，每种状态有各自的动画，上方有一个状态气泡。开发板转动时，画面按重力自动转正，目前每 90° 一档。

开发板本身不做识别和对话，而是与 Mac 上的中继程序 `claude-voice` 通信：中继用 whisper.cpp 把语音转成文字，交给一个无界面运行的 Claude Code 会话，再用 macOS 的 `say` 朗读回答，并把音频送回开发板的扬声器。设备与中继之间的帧格式见 `docs/protocol.md`，吉祥物的画法和各状态的动作见 `docs/mascot.md`。

## 硬件与软件要求

- 硬件：Waveshare ESP32-S3-Touch-AMOLED-1.75 开发板，USB 数据线。
- 运行中继的 Mac：
  - 已登录的 Claude Code CLI；
  - whisper.cpp（`brew install whisper-cpp`）和一个多语言 ggml 模型文件（例如 `ggml-medium.bin`）；
  - uv 和 Python 3.13。
- 编译固件：ESP-IDF 5.5.4。

## 快速开始

编译并烧录固件（先按 ESP-IDF 的说明加载环境）：

```
cd firmware
idf.py build
idf.py -p <串口> flash
```

目标芯片 `esp32s3` 已写在 `firmware/sdkconfig.defaults` 中，不必再执行 `set-target`。

在 `host/` 下新建 `config.toml`，至少填写 whisper 模型路径。其余键都可以省略，键名见 `host/config.example.toml`：

```
whisper_model = "/path/to/ggml-medium.bin"
```

启动中继：

```
cd host
uv sync
uv run claude-voice
```

首次启动时 whisper-server 要编译 Metal 内核，需要约 40 秒。中继会自动找到用 USB 连接的开发板；同时连了多块时，在 `config.toml` 里用 `device_serial` 指定序列号。

操作方式：

- 点按屏幕、按 BOOT 键或 PWR 键：开始听。说完后静音 0.8 秒，或再点一次，结束收音。
- 按住 PWR 键约 6 秒会关机。
- 思考或朗读时点按：打断，并重新开始听。
- 长按屏幕中部：清空对话，开始新会话。从屏幕边缘开始按住，不会清空对话。10 分钟没有对话也会自动开始新会话。
- 调节音量：手指沿屏幕边缘转圈，顺时针调大，逆时针调小。先滑过一小段，音量才开始变化。
- 调节时，屏幕边缘的弧线显示当前音量，松手后不到 1 秒消失。音量保存在开发板上，重启后不变；在朗读或收音时调节的，等结束后才保存。
- 调节亮度：手指在屏幕中部上下滑动，朝 Clawd 头顶的方向滑变亮，反方向滑变暗。先滑过一小段，亮度才开始变化。从屏幕边缘开始的滑动只调音量。
- 亮度保存在开发板上，重启后不变；在朗读或收音时调节的，等结束后才保存。已经最亮或最暗时继续滑，屏幕会轻闪一下。
- 屏幕变暗或熄灭时，第一次触摸只点亮屏幕，不会开始听、打断、清空对话、调节音量或亮度；按 BOOT 键或 PWR 键照常生效。

屏幕亮度：

- 拿起开发板、触摸屏幕、按 BOOT 键或 PWR 键时，屏幕恢复到设定的亮度。
- 亮度最低可调到 20 %，仍比自动变暗时亮。
- 最后一次操作 20 秒后变暗；从手里放回桌上，3 秒后就变暗。
- 变暗 30 分钟后熄屏，以减少 AMOLED 烧屏。
- 对话进行时屏幕保持亮着；同一状态超过 2 分钟会变暗，但不熄屏。
- 在桌上轻推、打字或敲桌子不会点亮屏幕。

测试：`cd host && uv run pytest -q`。端到端测试需要设置 `CLAUDE_VOICE_E2E=1`，会使用真实的 whisper、Claude 和 `say`，并消耗 Claude 订阅额度，默认跳过。

## Wi-Fi

Wi-Fi 还不能用，目前只能通过 USB 使用。Mac 端已经提供配对命令 `claude-voice pair` 和 `claude-voice forget`（见 `host/README.md`），开发板固件的 Wi-Fi 部分还在开发，现在配对会收到设备「未实现」的回复。

设计见 `docs/protocol.md`（协议版本 3）：经 USB 配对一次，之后开发板通过 TLS 1.2 PSK 连接 Mac 上的中继。

## 隐私与安全

- 麦克风音频只在「在听」状态下发送，且只发给中继。
- Claude 会话以 `--safe-mode --restricted` 启动，只开放网页搜索和网页抓取两个工具（`WebSearch`、`WebFetch`），不加载 MCP。
- Wi-Fi 配对完成后，Wi-Fi 密码和配对密钥会明文保存在开发板的闪存中，未启用闪存加密。能接触开发板的人可以读出它们。
- Mac 上的配对密钥存放在 macOS 钥匙串中。以你的用户身份运行的程序，可以不经提示读出它。
- 中继在局域网上监听 TCP 47801，只处理持有配对密钥的连接。局域网里的其他设备可以占住连接，推迟开发板的 Wi-Fi 重连，但不影响 USB 和中继本身。

## 说明

本项目不是 Anthropic 的官方项目。Clawd 吉祥物和 Claude Code 图标属于 Anthropic，图标来源为 thesvg.org。屏幕文字使用 Fusion Pixel 字体（SIL OFL 1.1），许可证在 `firmware/main/font/`。

本项目的代码以 MIT 许可证发布，见 `LICENSE`。MIT 许可证不涵盖 Clawd 形象、Claude Code 图标和 Fusion Pixel 字体，它们各自适用其所有者的条款。
