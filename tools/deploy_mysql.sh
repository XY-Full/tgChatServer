#!/usr/bin/env bash
# MySQL 8.4 部署脚本（本机 / 单机环境）
#
#   ./tools/deploy_mysql.sh
#
# 1) 从 tools/packages 解压 MySQL 8.4.5 到 third/mysql
# 2) 生成配置到 deploy/run/mysql.cnf
# 3) 初始化数据目录（已存在则跳过，不覆盖现有数据）
# 4) 启动并创建 tgchat 库、root 账号（caching_sha2_password）
# 5) 提示用 dbmigrate migrate 建表
#
# 可用环境变量：MYSQL_DATA_DIR（默认 /root/mysql-data）、MYSQL_PORT、DB_PASSWORD
set -e

TOOLS_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$TOOLS_DIR/.."
PKG_DIR="$TOOLS_DIR/packages"
THIRD_DIR="$ROOT_DIR/third"

VER="8.4.5"
TARBALL="mysql-$VER-linux-glibc2.28-x86_64-minimal.tar.xz"
SRC_DIR="mysql-$VER-linux-glibc2.28-x86_64-minimal"
DATA_DIR="${MYSQL_DATA_DIR:-/root/mysql-data}"
PORT="${MYSQL_PORT:-3306}"
DB_PASSWORD="${DB_PASSWORD:-CHANGE_ME_production_db_password}"
MYSQL_HOME="$THIRD_DIR/mysql"
CONF="$ROOT_DIR/deploy/run/mysql.cnf"

echo ">>> 1) 准备 MySQL $VER ..."
if [ ! -x "$MYSQL_HOME/bin/mysqld" ]; then
    cd "$PKG_DIR"
    if [ ! -f "$TARBALL" ]; then
        echo "缺少 $PKG_DIR/$TARBALL，请下载："
        echo "  curl -sSL -o $TARBALL https://dev.mysql.com/get/Downloads/MySQL-8.4/$TARBALL"
        exit 1
    fi
    tar xJf "$TARBALL"
    rm -rf "$MYSQL_HOME"
    mv "$SRC_DIR" "$MYSQL_HOME"
fi

# Debian 13 只有 libaio.so.1t64，MySQL 需要 libaio.so.1
if [ ! -e "$MYSQL_HOME/lib/private/libaio.so.1" ]; then
    ln -sf /lib/x86_64-linux-gnu/libaio.so.1t64 "$MYSQL_HOME/lib/private/libaio.so.1"
fi

echo ">>> 2) 生成配置 $CONF ..."
mkdir -p "$ROOT_DIR/deploy/run" "$ROOT_DIR/deploy/logs"
cat > "$CONF" <<EOF
[mysqld]
datadir=$DATA_DIR
socket=/tmp/mysql8.sock
port=$PORT
user=root
innodb_use_native_aio=0
log_error=$ROOT_DIR/deploy/logs/mysql-error.log
EOF

export LD_LIBRARY_PATH="$MYSQL_HOME/lib/private:$LD_LIBRARY_PATH"

echo ">>> 3) 初始化数据目录 $DATA_DIR ..."
if [ ! -d "$DATA_DIR/mysql" ]; then
    mkdir -p "$DATA_DIR"
    "$MYSQL_HOME/bin/mysqld" --defaults-file="$CONF" --initialize-insecure
else
    echo "    已存在，跳过（不覆盖现有数据）"
fi

echo ">>> 4) 启动 ..."
"$TOOLS_DIR/mysql_ctl.sh" start
sleep 6

echo ">>> 5) 创建库与账号（caching_sha2_password）..."
cat > /tmp/mysql_init.sql <<SQL
CREATE DATABASE IF NOT EXISTS tgchat DEFAULT CHARACTER SET utf8mb4;
ALTER USER 'root'@'localhost' IDENTIFIED WITH caching_sha2_password BY '$DB_PASSWORD';
CREATE USER IF NOT EXISTS 'root'@'127.0.0.1' IDENTIFIED WITH caching_sha2_password BY '$DB_PASSWORD';
GRANT ALL PRIVILEGES ON *.* TO 'root'@'127.0.0.1' WITH GRANT OPTION;
CREATE USER IF NOT EXISTS 'root'@'%' IDENTIFIED WITH caching_sha2_password BY '$DB_PASSWORD';
GRANT ALL PRIVILEGES ON *.* TO 'root'@'%' WITH GRANT OPTION;
FLUSH PRIVILEGES;
SQL
"$MYSQL_HOME/bin/mysql" --socket=/tmp/mysql8.sock -uroot < /tmp/mysql_init.sql
rm -f /tmp/mysql_init.sql

echo
echo ">>> 部署完成。建表：  $ROOT_DIR/server/bin/tools/dbmigrate migrate -H 127.0.0.1 -u root -p <密码> -d tgchat"
echo ">>> 日常管理：      $TOOLS_DIR/mysql_ctl.sh start|stop|restart|status|cli|logs|backup"
