#!/data/data/com.termux/files/usr/bin/bash
# HEN-K 安卓版一键编译+运行（Termux 内执行）
set -e
cd "$(dirname "$0")"
command -v clang >/dev/null || pkg install clang -y
clang -O2 -pthread -o gnyx_daemon gnyx_daemon.c
echo "[+] 编译完成: $(pwd)/gnyx_daemon"
echo "[+] root 运行（自动找高能英雄进程）:"
echo "    su -c \"$(pwd)/gnyx_daemon\""
echo "[+] 日志: /data/local/tmp/gnyx_boot.log"
echo "[+] 快照: /data/local/tmp/esp_state.json"
