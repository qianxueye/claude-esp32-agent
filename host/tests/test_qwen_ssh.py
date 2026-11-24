import pytest

from claude_voice.config import Config, load

SSH_ON = 'qwen_ssh_host = "speech-mac"\nqwen_server_cmd = ["/opt/venv/bin/python", "-m", "mlx_audio.server"]\n'


def write_config(tmp_path, text):
    p = tmp_path / "c.toml"
    p.write_text(text, encoding="utf-8")
    return p


def test_config_defaults_leave_ssh_off(tmp_path):
    cfg = load(write_config(tmp_path, SSH_ON))
    assert cfg.qwen_ssh_host == "speech-mac" and cfg.qwen_remote_port == 47803
    assert Config().qwen_ssh_host == ""


@pytest.mark.parametrize(
    ("text", "key"),
    [
        ('qwen_ssh_host = "-oProxyCommand=x"\nqwen_server_cmd = ["s"]\n', "qwen_ssh_host"),
        ('qwen_ssh_host = "speech mac"\nqwen_server_cmd = ["s"]\n', "qwen_ssh_host"),
        ('qwen_ssh_host = "speech-mac\\n"\nqwen_server_cmd = ["s"]\n', "qwen_ssh_host"),
        ('qwen_ssh_host = "speech-mac"\n', "qwen_server_cmd"),
        (SSH_ON + 'qwen_remote_python = "python3"\n', "qwen_remote_python"),
        (SSH_ON + 'qwen_remote_python = "/usr/bin/python3;rm"\n', "qwen_remote_python"),
        (SSH_ON + 'qwen_remote_python = "/my venv/python"\n', "qwen_remote_python"),
        (SSH_ON + "qwen_remote_port = 0\n", "qwen_remote_port"),
        (SSH_ON + "qwen_remote_port = 65536\n", "qwen_remote_port"),
        (SSH_ON + "qwen_link_timeout_s = 0.0\n", "qwen_link_timeout_s"),
        (SSH_ON + "qwen_ssh_connect_timeout_s = -1.0\n", "qwen_ssh_connect_timeout_s"),
        (SSH_ON + "qwen_restart_min_s = 0.0\n", "qwen_restart_min_s"),
        (SSH_ON + "qwen_restart_min_s = 10.0\nqwen_restart_max_s = 5.0\n", "qwen_restart_max_s"),
    ],
)
def test_config_ssh_validation(tmp_path, text, key):
    with pytest.raises(ValueError, match=key) as e:
        load(write_config(tmp_path, text))
    assert "unknown config keys" not in str(e.value)
