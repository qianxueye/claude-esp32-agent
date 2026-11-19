# claude-voice（Mac 端中继）

把 ESP32-S3 圆屏设备变成 Claude Code 的语音前端：设备帧（见 `docs/protocol.md`）↔ whisper.cpp 语音识别 ↔ 一个常驻、隔离的 Claude Code 会话 ↔ macOS `say` 语音合成 ↔ 设备扬声器，同时驱动屏幕上的吉祥物状态。

## 运行

```
cd host
uv sync
cp config.example.toml config.toml   # 然后填写 whisper_model
uv run claude-voice [--config config.toml]
```

- 中继自动找到唯一一台 USB 连接的设备（Espressif USB-JTAG，VID 0x303A / PID 0x1001），不看端口名；拔插后自动重连。连了多台时在 `config.toml` 里用 `device_serial` 指定序列号。
- 设备用 USB 连着本机时走 USB；否则等设备通过 Wi-Fi 连进来（需要先配对，见下）。两条都可用时 USB 优先。
- 首次启动 whisper-server 要编译 Metal 内核，约 40 秒，之后保持常驻。
- 日志输出到标准输出和 `host/logs/relay.log`。
- `config.toml` 只放本机设置，不进版本库，键名见 `src/claude_voice/config.py`（设备序列号、模型、effort、工具、系统提示词、音色、语速、whisper 模型路径、VAD 常量、超时、初始音量、Wi-Fi 端口）。示例：`config.example.toml`。
- `config.toml` 里的 `volume` 是初始音量：只在开发板还没有保存音量时发给开发板，之后以开发板上的音量为准。

## Wi-Fi 配对

开发板固件的 Wi-Fi 部分还在开发，现在配对会收到设备「未实现」的回复。下面是完成后的流程。

配对只需做一次，通过 USB 完成：

1. 停止正在运行的 `claude-voice`（配对要独占设备的串口和端口 47801）。
2. 用数据线把设备连到这台 Mac，在自己的终端里运行：

   ```
   uv run claude-voice pair
   ```

3. 按提示输入 Wi-Fi 名称和密码。密码输入时不显示，不会写进日志。设备会重启一次。
4. 程序生成配对密钥，存进 macOS 钥匙串（服务名 `claude-voice`），连同 Wi-Fi 设置和本机的局域网名称、地址一起写入设备，然后等设备通过 Wi-Fi 连上本机，并显示结果。
5. 第一次配对时 macOS 防火墙可能弹窗，询问是否允许 Python 接受传入连接。请点「允许」，否则设备连不上本机。
6. 配对完成后拔掉 USB 线（或改用充电器供电），再运行 `uv run claude-voice`，设备就会通过 Wi-Fi 连上。

- 换了 Wi-Fi 或本机地址变了，重新运行一次 `pair` 即可。
- 清除配对：`uv run claude-voice forget`（设备要用 USB 连着）。它清除设备上的 Wi-Fi 设置，并删除钥匙串里的密钥。
- Mac 睡眠或中继没在运行时，设备屏幕显示「离线」；Mac 唤醒、中继启动后设备会自动重连。
- 设备没有开启闪存加密，拿到设备的人可以读出其中的 Wi-Fi 密码和配对密钥。

## 前提

- whisper 模型文件（如 `ggml-medium.bin`）：在 `config.toml` 里用 `whisper_model` 写明路径。没写或文件不存在时，启动会报错并指出该配置项。
- 已登录的 Claude Code CLI（`claude_cli`，默认取 `PATH` 里的 `claude`）。会话用 `--safe-mode --restricted`，只开 `WebSearch`、`WebFetch`，无 MCP，工作目录 `~/.claude-voice-agent-ws`。
- 不需要 `ANTHROPIC_API_KEY`。

## 操作

- 按一下 BOOT 或 PWR 键：开始听。说完（静音 0.8 秒）或再按一下结束。
- 按住 PWR 键约 6 秒会关机。
- 思考或朗读时按一下 BOOT 或 PWR 键：打断并重新听。
- 触摸屏幕只用来点亮屏幕、调节音量和亮度，不会开始听或打断。
- 10 分钟没有对话会自动开始新会话。
- 调节音量：手指沿屏幕边缘转圈，顺时针调大，逆时针调小。先滑过一小段，音量才开始变化。
- 调节时，屏幕边缘的弧线显示当前音量。音量已经最大或最小时继续转，弧线会顺着手指的方向往外伸，转得越远伸得越长，但最多只伸出一小段，这时音量不变。往回转或松手，伸出的部分就缩回。松手后不到 1 秒，弧线消失。音量保存在开发板上，重启后不变；在朗读或收音时调节的，等结束后才保存。
- 调节亮度：手指上下滑动，起点在屏幕中部或屏幕上下边缘都可以。朝 Clawd 头顶的方向滑变亮，反方向滑变暗。先滑过一小段，亮度才开始变化。从屏幕左右边缘开始的上下滑动调的是音量。
- 调节亮度时，屏幕左侧边缘出现一段弧线，亮度越高，弧线从下往上填得越多。已经最亮或最暗时继续滑，弧线会顺着手指的方向往外伸，滑得越远伸得越长，但最多只伸出一小段，这时亮度不变。往回滑或松手，伸出的部分就缩回。松手后不到 1 秒，弧线消失。亮度保存在开发板上，重启后不变；在朗读或收音时调节的，等结束后才保存。
- 屏幕变暗或熄灭时，第一次触摸只点亮屏幕，不会调节音量或亮度；按 BOOT 或 PWR 键照常生效。

## 测试

```
uv run pytest                                  # 单元测试和假设备测试，只用本机回环地址，不碰真实钥匙串条目
CLAUDE_VOICE_KEYCHAIN_TEST=1 uv run pytest tests/test_pairing.py   # 另外用临时服务名读写一次真实钥匙串
CLAUDE_VOICE_E2E=1 uv run pytest -s tests/test_e2e.py   # 真实 whisper + Claude + say，会消耗 Claude 订阅额度
```

端到端测试不会打开任何串口，也不会用 Mac 扬声器播放；输出 WAV 在 `host/test-output/`。
