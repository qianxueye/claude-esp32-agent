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

配对只需做一次，通过 USB 完成：

1. 停止正在运行的 `claude-voice`（配对要独占设备的串口和端口 47801）。
2. 用数据线把设备连到这台 Mac，在自己的终端里运行：

   ```
   uv run claude-voice pair
   ```

3. 按提示输入 Wi-Fi 名称和密码，之后可以再输入最多 3 个其他 Wi-Fi（例如手机热点），直接按回车结束。密码输入时不显示，不会写进日志。
4. 程序生成配对密钥，存进 macOS 钥匙串（服务名 `claude-voice`），连同 Wi-Fi 设置和本机的局域网名称、地址一起写入设备，然后等设备通过 Wi-Fi 连上本机，并显示结果。
5. 第一次配对时 macOS 防火墙可能弹窗，询问是否允许 Python 接受传入连接。请点「允许」，否则设备连不上本机。
6. 配对完成后拔掉 USB 线（或改用充电器供电），再运行 `uv run claude-voice`，设备就会通过 Wi-Fi 连上。

- 换了 Wi-Fi 或本机地址变了，重新运行一次 `pair` 即可。
- 清除配对：`uv run claude-voice forget`（设备要用 USB 连着）。它清除设备上的 Wi-Fi 设置，并删除钥匙串里的密钥。
- 设备丢失时：运行 `uv run claude-voice revoke`，不需要连接设备。它删除钥匙串里这台设备的配对密钥，正在运行的中继会在 10 秒内断开这台设备。拿到设备的人仍能读出其中保存的 Wi-Fi 密码，请修改这些 Wi-Fi 的密码。钥匙串里有多台设备的密钥时，先在 `config.toml` 里用 `device_serial` 指定要撤销的设备。
- 配对结果无法确认时，钥匙串会保留这次的配对密钥，之前的也不删除，直到下一次配对成功，或运行 forget 或 revoke。
- Mac 睡眠或中继没在运行时，设备屏幕显示「离线」；Mac 唤醒、中继启动后设备会自动重连。
- 设备没有开启闪存加密，拿到设备的人可以读出其中的 Wi-Fi 密码和配对密钥。

## 远程连接端点

中继在本机 `127.0.0.1` 的 47802 端口（`tunnel_port`）上提供 WebSocket 端点 `/clawd/v1`，用来接收隧道程序（如 cloudflared）从互联网转来的开发板连接。

- 只有这台 Mac 上的程序能连到这个端口，局域网上的其他设备连不到。
- 端点里传的是与 Wi-Fi 相同的加密连接，没有配对密钥的连接会被断开。
- 在 `config.toml` 里设置 `cloudflared_bin`（cloudflared 程序的路径）后，中继会启动 cloudflared 的临时隧道，得到一个 `trycloudflare.com` 公网地址，写进日志并发给开发板。cloudflared 退出后中继会自动重启它，每次得到的地址都不同。没有设置时，中继不启动隧道程序。
- 地址变了以后，开发板要先通过 USB 或局域网连上中继，才能收到新地址。
- cloudflared 启动失败不影响 USB 和 Wi-Fi 连接。它的日志写在日志目录的 `cloudflared.log`。
- 中继通过一个看护进程启动 cloudflared。中继退出时，包括被强制结束（SIGKILL），看护进程都会停止 cloudflared。只有看护进程自己被强制结束时，cloudflared 才会留在后台，需要手动结束它。
- 和 Wi-Fi 端口一样，只在 Wi-Fi 开启、这台 Mac 已和开发板配对时启动。不需要时，在 `config.toml` 里设 `tunnel_enabled = false`。

## 前提

- whisper 模型文件（如 `ggml-medium.bin`）：在 `config.toml` 里用 `whisper_model` 写明路径。没写或文件不存在时，启动会报错并指出该配置项。
- 已登录的 Claude Code CLI（`claude_cli`，默认取 `PATH` 里的 `claude`）。会话用 `--safe-mode --restricted`，只开 `WebSearch`、`WebFetch`，无 MCP，工作目录 `~/.claude-voice-agent-ws`。
- 不需要 `ANTHROPIC_API_KEY`。

## 语音合成

默认用 macOS `say`。把 `tts_engine` 设为 `"qwen3"`，改用 mlx-audio 语音服务上的 Qwen3-TTS（默认模型 0.6B，声音 `ono_anna`）：

- `qwen_server_cmd`：服务的启动命令（参数列表）。设置后，中继在启动时选一个本机空闲端口启动服务，用一句短话预热，退出时停止服务。同时设置了 `qwen_ssh_host` 时，服务改在另一台 Mac 上运行，见下文。
- `qwen_url`：没有设置 `qwen_server_cmd` 时，连接这个地址的服务，例如经 SSH 端口转发的另一台 Mac。
- `qwen_lang_code`（`chinese` 或 `japanese`）和 `qwen_voice` 由配置决定，不自动识别语言；`qwen_voices` 可按语言指定不同的声音。
- 服务返回 24 kHz 音频，中继用 `afconvert` 转成 16 kHz。
- 服务启动失败或没有就绪时，中继在日志里记一条错误，并继续使用 `say`。某一句合成失败时，这一句改用 `say`，日志记一条警告，之后 `qwen_skip_s` 秒内的句子直接用 `say`，不再等待超时。
- 用 `qwen_url` 连接的服务失败后，中继在 `qwen_skip_s` 秒后再次尝试连接；中继在本机启动的服务一旦失败，不会重启，中继保持使用 `say`，直到重新启动中继。
- 服务日志写在日志目录的 `qwen-tts-server.log`。

在另一台 Mac 上合成：把 `qwen_ssh_host` 设为那台 Mac 在 `~/.ssh/config` 里的别名（例如 `speech-mac`），`qwen_server_cmd` 里写那台 Mac 上的路径。中继通过 SSH 在那台 Mac 上启动服务，并把服务端口转发到本机。

- 那台 Mac 需要事先准备好：
  - 已打开「远程登录」。别名里已配置密钥，可以不输密码登录；那台 Mac 的主机密钥已在本机的 `known_hosts` 里。中继不会询问密码，也不会确认新的主机密钥。
  - 允许 SSH 端口转发（`AllowTcpForwarding`，默认允许）。别名里不要写 `LocalForward`。
  - 有 `/usr/bin/python3`（随命令行工具安装）；没有时用 `qwen_remote_python` 指定那台 Mac 上 Python 的绝对路径。
  - 服务的虚拟环境已经装好。
- 中继不在那台 Mac 上安装或写入文件。服务在那台 Mac 上使用端口 `qwen_remote_port`（默认 47803）。
- 中继启动时不等服务就绪。服务就绪之前，每句话都用 `say`。
- 连接断开或服务退出后，下一句起改用 `say`，日志记一条警告。中继随后自动重连：间隔从 `qwen_restart_min_s` 秒开始，每次加倍，最长 `qwen_restart_max_s` 秒。重连成功后恢复使用 Qwen3。
- 中继退出、被强制结束或网络中断后，那台 Mac 最迟在 `qwen_link_timeout_s` 秒（默认 10 秒）后开始停止服务；服务 5 秒内没有退出时被强制结束。
- 例外：中继在那台 Mac 上用一个小的 Python 进程看管服务。如果这个进程被单独结束，服务会继续运行并占用端口，之后的每次重连都会在日志里报告端口被占用，需要在那台 Mac 上手动结束服务。
- SSH 和那台 Mac 上服务的输出也写在 `qwen-tts-server.log`。

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
