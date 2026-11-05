# =============================================================================
# tgChatServer 全服务镜像（center / busd / connd / logic / account / dbproxy）
# 构建上下文必须是仓库根目录（deploy/Dockerfile 的上一级）：
#   docker build -f deploy/Dockerfile -t tgchat-server .
# =============================================================================

# ---------------------------- 构建阶段 ---------------------------------------
FROM debian:bookworm AS builder

ENV DEBIAN_FRONTEND=noninteractive

# 第三方库需要联网下载源码编译；protobuf/openssl/curl/zlib/libcopp 均为静态库
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential \
        cmake \
        git \
        curl \
        ca-certificates \
        pkg-config \
        libmariadb-dev \
        libssl-dev \
        zlib1g-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src

# 拷贝整个仓库（源码 + third/kcp 等已 vendored 的依赖）
COPY . .

# 构建第三方静态依赖（输出到 /src/third/）
RUN chmod +x tools/build_thirds.sh && ./tools/build_thirds.sh

# 构建全部服务二进制 + dbmigrate 迁移工具
RUN cmake -B build -S server -DBUILD_TESTS=OFF \
    && cmake --build build -j"$(nproc)"

# ---------------------------- 运行阶段 ---------------------------------------
FROM debian:bookworm-slim AS runtime

ENV DEBIAN_FRONTEND=noninteractive

# 运行时动态库：libmariadb（dbproxy/dbmigrate 依赖）、OpenSSL 运行时、zlib、libstdc++
RUN apt-get update && apt-get install -y --no-install-recommends \
        ca-certificates \
        libmariadb3 \
        libssl3 \
        libcrypto3 \
        zlib1g \
        libstdc++6 \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app

# 拷贝服务二进制
COPY --from=builder /src/server/bin/center/center     /app/bin/center
COPY --from=builder /src/server/bin/busd/busd         /app/bin/busd
COPY --from=builder /src/server/bin/connd/connd       /app/bin/connd
COPY --from=builder /src/server/bin/logic/logic       /app/bin/logic
COPY --from=builder /src/server/bin/account/account   /app/bin/account
COPY --from=builder /src/server/bin/dbproxy/dbproxy   /app/bin/dbproxy
COPY --from=builder /src/server/bin/tools/dbmigrate   /app/bin/dbmigrate

# 拷贝数据库迁移文件（供 db-migrate init 容器使用）
COPY db/migrations /app/db/migrations

ENV PATH="/app/bin:${PATH}"

# 各服务通过 docker-compose 的 command 指定 --config 启动
CMD ["/bin/bash"]
