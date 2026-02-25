#!/usr/bin/env bash
set -e  # 遇到错误立即退出
set -o pipefail

start_time=$(date +%s)

arg=$1

# 打印提示函数
log() {
    echo -e "\033[1;34m[INFO]\033[0m $*"
}

# 构建函数
build_project() {
    log "开始 CMake 构建..."
    mkdir -p build
    cd build
    cmake -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache -DENABLE_ASAN=ON -DCMAKE_BUILD_TYPE=Debug ..
    make -j8
    cd ..
}

case "$arg" in
    table|res)
        log "执行资源构建 (res)"
        cd gameSvr && ./conv res && cd ..
        ;;
    config)
        log "执行配置构建 (nores)"
        cd ../public && ./make.sh && cd ../server
        ;;
    all)
        log "执行完整构建 (res + 编译)"
        build_project
        ;;
    "" )
        log "执行默认构建 (nores + 编译)"
        cd ../public && ./make.sh && cd ../server
        build_project
        ;;
    *)
        echo -e "\033[1;31m[ERROR]\033[0m 无效参数: $arg"
        echo "用法: $0 [table|res|config|all]"
        exit 1
        ;;
esac

end_time=$(date +%s)
elapsed=$((end_time - start_time))

log "构建完成，总耗时: ${elapsed} 秒"


