#!/usr/bin/env bash
# 反序停止全部服务：connd → logic → account → dbproxy → busd → center
set -e

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
RUN="$ROOT/deploy/run"

stop_one() {
    local name="$1"
    if [ -f "$RUN/$name.pid" ]; then
        local pid
        pid="$(cat "$RUN/$name.pid")"
        if kill -0 "$pid" 2>/dev/null; then
            kill "$pid" 2>/dev/null || true
            echo "[stop] $name (pid=$pid)"
        fi
        rm -f "$RUN/$name.pid"
    fi
}

for name in connd logic account dbproxy busd center; do
    stop_one "$name"
done

echo "全部服务已停止"
