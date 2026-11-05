# tgChatServer 部署说明

本项目为 C++20 微服务聊天服务器，采用「注册中心 center + 消息总线 bus（共享内存 + TCP）」架构。
本目录提供可上线部署所需的配置、镜像与编排资产，**不修改任何现有源码**。

## 服务清单

| 服务 | 二进制路径 | service_id | svr_name |
|---|---|---|---|
| center（注册中心，TCP 9090） | `server/bin/center/center` | 1 | CenterService |
| busd（总线守护，本地 3099） | `server/bin/busd/busd` | 2 | BusdService |
| connd（连接网关 TCP/WS/KCP） | `server/bin/connd/connd` | 3 | ConndService |
| logic（业务逻辑） | `server/bin/logic/logic` | 4 | LogicService |
| account（JWT 签发） | `server/bin/account/account` | 5 | AccountService |
| dbproxy（数据库代理） | `server/bin/dbproxy/dbproxy` | 6 | DBProxyService |
| dbmigrate（迁移工具） | `server/bin/tools/dbmigrate` | - | - |
| client（测试客户端） | `server/client/client` | - | - |

> 注意：`service_id` 与 `svr_name` 必须与上表一致，服务发现路由依赖此对应关系。

## 1. 构建依赖

```bash
# Debian/Ubuntu
sudo apt-get update && sudo apt-get install -y \
    build-essential cmake git curl ca-certificates pkg-config \
    libmariadb-dev libssl-dev zlib1g-dev
```

## 2. 构建

```bash
cd /root/tgChatServer

# 2.1 构建第三方静态依赖（输出到 third/，需联网下载源码）
bash tools/build_thirds.sh

# 2.2 构建全部服务二进制 + dbmigrate
cmake -B build -S server -DBUILD_TESTS=OFF
cmake --build build -j

# 2.3（可选）单独构建测试客户端
cmake -B build-client -S server/client
cmake --build build-client -j
```

## 2.5 本机 MySQL 部署（非 docker 场景）

MySQL 8.4.5 二进制固定在 `tools/packages`，部署与管理脚本：

```bash
tools/deploy_mysql.sh          # 解压到 third/mysql + 初始化数据目录 + 建库建账号
tools/mysql_ctl.sh start       # 启动（stop / restart / status / cli / logs / backup）
```

配置生成在 `deploy/run/mysql.cnf`，日志在 `deploy/logs/mysql-error.log`，
数据目录默认 `deploy/data/mysql`（可用 MYSQL_DATA_DIR 覆盖）。
root 使用 MySQL 8.4 默认的 `caching_sha2_password`（MariaDB Connector/C 3.4.11 起支持）。

## 3. 数据库迁移

表结构的唯一数据源是 `public/db_proto/*.proto`，由 `public/make.sh` 生成期望结构并编译进
`dbmigrate` 二进制。先确保 MySQL（或 MariaDB）已启动并创建 `tgchat` 库，然后：

    # 1) 先看差异（只打印不执行；无差异退出码 0，有差异为 2）
    server/bin/tools/dbmigrate diff -H 127.0.0.1 -P 3306 -u root -p <密码> -d tgchat

    # 2) 确认无误后执行（破坏性变更需显式加 --allow-destructive）
    server/bin/tools/dbmigrate migrate -H 127.0.0.1 -P 3306 -u root -p <密码> -d tgchat

新增/改表：改 `.proto` → 跑 `public/make.sh` → 重新编译 → `diff` 看计划 → `migrate`。
`migrate` 幂等，可重复执行，每次执行记录到 `db_schema_version`。

> 历史上用过 `db/migrations/*.xml`（增量迁移文件），现已废弃删除。
> 库里 `db_schema_version` 的旧记录保留为历史，不影响新的声明式流程。

## 4. 启动

### 方式 A：docker-compose（推荐，一键启动 mysql + 6 服务 + 迁移）

```bash
cd /root/tgChatServer

# 构建镜像并后台启动（首次会自动构建 tgchat-server 镜像）
docker compose -f deploy/docker-compose.yml up --build -d

# 查看日志
docker compose -f deploy/docker-compose.yml logs -f

# 停止
docker compose -f deploy/docker-compose.yml down
```

编排要点：

- `mysql`（8.0）自动初始化数据库 `tgchat`，带健康检查。
- `db-migrate` 为一次性容器，在 mysql 健康后执行全部迁移。
- 启动顺序：center 先启动（健康检查 9090），其余服务依赖 center 健康；dbproxy 额外依赖 mysql。
- 业务服务使用 `ipc: host` 共享 `/dev/shm`（总线依赖 POSIX 共享内存环形缓冲）。
- 配置从 `deploy/config/` 只读挂载；`center_ip=center`、`db_host=mysql` 为 compose 服务名。

### 方式 B：本地脚本（非 docker）

```bash
cd /root/tgChatServer

bash deploy/scripts/start.sh   # center→busd→dbproxy→account→logic→connd 后台启动
bash deploy/scripts/stop.sh    # 反序停止
```

- 日志输出到 `deploy/logs/<服务名>.log`，PID 记录在 `deploy/run/<服务名>.pid`。
- 配置里 `center`/`mysql` 为 docker 服务名，本地运行时脚本会（root 下）自动向
  `/etc/hosts` 追加 `127.0.0.1 center mysql`；非 root 需手动添加。

## 5. 验证

用测试客户端走完整链路（connect → 申请 JWT → 鉴权 → 心跳×3）：

```bash
# 自动测试
server/client/client

# 交互式 REPL（默认 127.0.0.1:7000，即 connd 的 tcp_port）
server/client/client -i
```

## 6. 生产上线注意

1. **JWT 密钥**：`deploy/config/connd.json` 与 `account.json` 中的
   `auth.jwt.secret` 当前为占位值 `CHANGE_ME_production_jwt_secret_replace_this`，
   上线前必须替换，且 **connd 与 account 两处必须完全一致**。
2. **数据库密码**：`deploy/config/dbproxy.json` 与 `docker-compose.yml` 中
   `MYSQL_ROOT_PASSWORD` 的 `CHANGE_ME_production_db_password` 必须替换并保持一致。
3. **共享内存**：总线使用 POSIX 共享内存，生产环境请确保 `/dev/shm` 容量足够
   （`local_ring_size` 默认 1MB，可调大）。
4. **编译类型**：当前 `server/CMakeLists.txt` 硬编码 `CMAKE_BUILD_TYPE=Debug`，
   生产可考虑改为 Release 并关闭调试日志（需调整源码，不在本目录范围内）。
