#!/usr/bin/env bash
# MySQL 8.4 管理脚本（本机后端，二进制来自 third/mysql）
#
#   ./tools/mysql_ctl.sh start | stop | restart | status | cli | logs | backup
#
# 可用环境变量：MYSQL_PORT（默认 3306）、DB_PASSWORD（默认 CHANGE_ME_production_db_password）
set -e

TOOLS_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$TOOLS_DIR/.."
MYSQL_HOME="$ROOT_DIR/third/mysql"
CONF="$ROOT_DIR/deploy/run/mysql.cnf"
PORT="${MYSQL_PORT:-3306}"
DB_PASSWORD="${DB_PASSWORD:-CHANGE_ME_production_db_password}"
SOCK=/tmp/mysql8.sock

export LD_LIBRARY_PATH="$MYSQL_HOME/lib/private:$LD_LIBRARY_PATH"

if [ ! -x "$MYSQL_HOME/bin/mysqld" ]; then
    echo "找不到 $MYSQL_HOME/bin/mysqld —— 请先跑 tools/deploy_mysql.sh"
    exit 1
fi
if [ ! -f "$CONF" ]; then
    echo "找不到配置 $CONF —— 请先跑 tools/deploy_mysql.sh"
    exit 1
fi

case "$1" in
  start)
    if ss -lnt 2>/dev/null | grep -qE ":$PORT\s"; then
        echo "端口 $PORT 已被占用："; ss -lntp 2>/dev/null | grep ":$PORT" || true; exit 1
    fi
    "$MYSQL_HOME/bin/mysqld" --defaults-file="$CONF" --daemonize
    echo "MySQL 已启动（socket=$SOCK port=$PORT）"
    ;;
  stop)
    "$MYSQL_HOME/bin/mysqladmin" --socket="$SOCK" -uroot -p"$DB_PASSWORD" shutdown
    echo "MySQL 已停止"
    ;;
  restart)
    "$0" stop || true; sleep 2; "$0" start
    ;;
  status)
    if "$MYSQL_HOME/bin/mysqladmin" --socket="$SOCK" -uroot -p"$DB_PASSWORD" ping 2>/dev/null | grep -q alive; then
        "$MYSQL_HOME/bin/mysql" --socket="$SOCK" -uroot -p"$DB_PASSWORD" -N -e \
          "SELECT CONCAT('MySQL ', @@version, '  uptime=', VARIABLE_VALUE, 's') FROM performance_schema.global_status WHERE VARIABLE_NAME='Uptime'" 2>/dev/null
    else
        echo "MySQL 未运行"; exit 1
    fi
    ;;
  cli)
    shift || true
    "$MYSQL_HOME/bin/mysql" --socket="$SOCK" -uroot -p"$DB_PASSWORD" "$@"
    ;;
  logs)
    tail -n "${2:-50}" "$(grep -E '^log_error' "$CONF" | head -1 | cut -d= -f2-)"
    ;;
  backup)
    mkdir -p "$ROOT_DIR/deploy/backup"
    out="$ROOT_DIR/deploy/backup/tgchat_$(date +%Y%m%d_%H%M%S).sql"
    "$MYSQL_HOME/bin/mysqldump" --socket="$SOCK" -uroot -p"$DB_PASSWORD" --databases tgchat > "$out"
    echo "已备份到 $out"
    ;;
  *)
    echo "用法: $0 start | stop | restart | status | cli | logs [n] | backup"
    exit 1
    ;;
esac
