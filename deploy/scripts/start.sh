#!/usr/bin/env bash
# 本地（非 docker）一键启动全部服务
# 启动顺序：center → busd → dbproxy → account → logic → connd
set -e

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BIN="$ROOT/server/bin"
CFG="$ROOT/deploy/config"
LOG="$ROOT/deploy/logs"
RUN="$ROOT/deploy/run"
mkdir -p "$LOG" "$RUN"

# 本地运行时，配置里的 center/mysql 主机名需要解析到本机
add_host() {
    local ip="$1" name="$2"
    if ! grep -qE "(^|[[:space:]])${name}([[:space:]]|$)" /etc/hosts 2>/dev/null; then
        echo "$ip $name" >> /etc/hosts
        echo "[hosts] add $ip $name"
    fi
}
if [ "$(id -u)" = "0" ]; then
    add_host 127.0.0.1 center
    add_host 127.0.0.1 mysql
else
    echo "[warn] 非 root 用户，请手动确保 /etc/hosts 含 '127.0.0.1 center mysql'"
fi

start_one() {
    local name="$1"
    local bin="$BIN/$name/$name"
    local cfg="$CFG/$name.json"
    if [ ! -x "$bin" ]; then
        echo "[skip] 未找到二进制 $bin，请先构建"
        return
    fi
    if [ -f "$RUN/$name.pid" ] && kill -0 "$(cat "$RUN/$name.pid")" 2>/dev/null; then
        echo "[skip] $name 已在运行"
        return
    fi
    nohup "$bin" --config "$cfg" >> "$LOG/$name.log" 2>&1 &
    echo $! > "$RUN/$name.pid"
    echo "[start] $name (pid=$(cat "$RUN/$name.pid"))"
    sleep 1
}

start_one center
start_one busd
start_one dbproxy
start_one account
start_one logic
start_one connd

echo "全部服务已启动，日志见 $LOG/"
