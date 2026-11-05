#!/usr/bin/env bash
set -e

# 用法:
#   ./build_thirds.sh                 # 增量编译：只编译尚未完成的依赖
#   ./build_thirds.sh openssl curl    # 只编译指定依赖
#   ./build_thirds.sh -f              # 强制重新编译全部依赖
#   ./build_thirds.sh -f mariadb      # 强制重新编译指定依赖
#   ./build_thirds.sh -l              # 列出可编译的依赖

TOOLS_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$TOOLS_DIR/.."
THIRD_DIR="$ROOT_DIR/third"
PKG_DIR="$TOOLS_DIR/packages"
STAMP_DIR="$THIRD_DIR/.stamp"
NPROC=$(nproc 2>/dev/null || sysctl -n hw.ncpu)

ALL_DEPS=(gtest zlib openssl protobuf curl libcopp libmariadb kcp)
FORCE=0
TARGETS=()

mkdir -p "$THIRD_DIR" "$PKG_DIR" "$STAMP_DIR"

usage() {
  sed -n '3,10p' "${BASH_SOURCE[0]}"
  exit 0
}

while [ $# -gt 0 ]; do
  case "$1" in
    -f|--force) FORCE=1 ;;
    -l|--list)  echo "${ALL_DEPS[*]}"; exit 0 ;;
    -h|--help)  usage ;;
    -*)         echo "unknown option: $1"; exit 1 ;;
    *)          TARGETS+=("$1") ;;
  esac
  shift
done

if [ ${#TARGETS[@]} -eq 0 ]; then
  TARGETS=("${ALL_DEPS[@]}")
fi

# 下载或解压函数
fetch_and_extract() {
  local url=$1
  local pkg=$2
  local dir=$3

  cd "$PKG_DIR"
  if [ ! -f "$pkg" ]; then
    echo ">>> downloading $pkg ..."
    echo "curl -L $url -o $pkg"
    curl -L "$url" -o "$pkg"
  else
    echo ">>> using local $pkg"
  fi

  echo ">>> extracting $pkg ..."
  tar xf "$pkg"
  cd "$ROOT_DIR"
  rm -fr "$THIRD_DIR/$dir" 
  mv "$PKG_DIR/$dir" "$THIRD_DIR/"
}

# 克隆github仓库函数
git_clone() {
  local url=$1
  local tag=$2
  local dir=$3

  cd "$PKG_DIR"
  if [ ! -f "$dir.tar.gz" ]; then
    echo ">>> git clone $dir ..."
    echo "git clone --branch $tag --recurse-submodules $url $dir"
    git clone --branch "$tag" --recurse-submodules "$url" "$dir"
    tar -zcvf "$dir.tar.gz" "$dir"
  else
    echo ">>> using local $dir.tar.gz"
    echo ">>> extracting $dir.tar.gz ..."
    tar xf "$dir.tar.gz"
  fi

  cd "$ROOT_DIR"
  rm -fr "$THIRD_DIR/$dir" 
  mv "$PKG_DIR/$dir" "$THIRD_DIR/"
}

# ============ 增量编译支持 ============
# 用法: begin_build <名称> <版本> <产物1> [产物2 ...]
# 返回 0 表示需要编译，返回 1 表示已完成可跳过（调用方用 `|| return 0` 跳过）。
# 判定依据: 存在该版本的完成戳记，或全部产物文件都已存在（兼容本脚本改造前已编译好的依赖）。
BUILD_STAMP=""
begin_build() {
  local name=$1
  local ver=$2
  shift 2

  BUILD_STAMP="$STAMP_DIR/$name-$ver.done"
  if [ "$FORCE" -eq 0 ]; then
    local all_exist=1
    local f
    for f in "$@"; do
      [ -e "$f" ] || all_exist=0
    done
    if [ -f "$BUILD_STAMP" ] || [ "$all_exist" -eq 1 ]; then
      touch "$BUILD_STAMP"
      echo ">>> $name-$ver already built, skip"
      return 1
    fi
  fi

  rm -f "$BUILD_STAMP"
  echo ">>> building $name-$ver ..."
  return 0
}

end_build() {
  mkdir -p "$STAMP_DIR"
  touch "$BUILD_STAMP"
}

# ============ build zlib ============
build_zlib() {
  local ver="1.3.1"
  local pkg="zlib-$ver.tar.gz"
  local url="https://zlib.net/$pkg"
  begin_build zlib "$ver" "$THIRD_DIR/zlib/lib/libz.a" "$THIRD_DIR/zlib/include/zlib.h" || return 0
  fetch_and_extract "$url" "$pkg" "zlib-$ver"

  cd "$THIRD_DIR/zlib-$ver"
  CFLAGS="-fPIC" ./configure --prefix="$THIRD_DIR/zlib"
  make -j$NPROC && make install
  cd "$ROOT_DIR"
  rm -rf "$THIRD_DIR/zlib-$ver"
  end_build
}

# ============ build openssl ============
build_openssl() {
  local ver="3.3.1"
  local pkg="openssl-$ver.tar.gz"
  local url="https://www.openssl.org/source/$pkg"
  begin_build openssl "$ver" "$THIRD_DIR/openssl/lib/libcrypto.a" "$THIRD_DIR/openssl/include/openssl/ssl.h" || return 0
  fetch_and_extract "$url" "$pkg" "openssl-$ver"

  cd "$THIRD_DIR/openssl-$ver"
  ./Configure --prefix="$THIRD_DIR/openssl" no-shared
  make -j$NPROC && make install_sw
  rm -f "$THIRD_DIR/openssl/lib"
  ln -sf "$THIRD_DIR/openssl/lib64" "$THIRD_DIR/openssl/lib"
  cd "$ROOT_DIR"
  rm -rf "$THIRD_DIR/openssl-$ver"
  end_build
}

# ============ build protobuf ============
build_protobuf() {
  local ver="3.21.12"
  local pkg="v$ver.tar.gz"
  local url="https://github.com/protocolbuffers/protobuf/archive/refs/tags/v$ver.tar.gz"
  begin_build protobuf "$ver" "$THIRD_DIR/protobuf/lib/libprotobuf.a" "$THIRD_DIR/protobuf/include/google/protobuf" || return 0
  fetch_and_extract "$url" "$pkg" "protobuf-$ver"

  cd "$THIRD_DIR/protobuf-$ver"
  cmake -B build -DCMAKE_BUILD_TYPE=Release \
        -Dprotobuf_BUILD_TESTS=OFF \
        -Dprotobuf_BUILD_SHARED_LIBS=OFF \
        -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
        -DCMAKE_INSTALL_PREFIX="$THIRD_DIR/protobuf"
  cmake --build build -j$NPROC
  cmake --install build
  cd "$ROOT_DIR"
  rm -rf "$THIRD_DIR/protobuf-$ver"
  end_build
}

# ============ build curl ============
build_curl() {
  local ver="8.7.1"
  local pkg="curl-$ver.tar.gz"
  local url="https://curl.se/download/$pkg"
  begin_build curl "$ver" "$THIRD_DIR/curl/lib/libcurl.a" || return 0
  fetch_and_extract "$url" "$pkg" "curl-$ver"

  cd "$THIRD_DIR/curl-$ver"
  CFLAGS="-fPIC" CXXFLAGS="-fPIC" ./configure \
    --prefix="$THIRD_DIR/curl" \
    --with-openssl="$THIRD_DIR/openssl" \
    --with-zlib="$THIRD_DIR/zlib" \
    --disable-shared \
    --enable-static \
    --disable-ldap \
    --disable-ldaps \
    --disable-rtsp \
    --without-librtmp \
    --disable-dict \
    --disable-telnet \
    --disable-tftp \
    --disable-pop3 \
    --disable-imap \
    --disable-smtp \
    --disable-gopher \
    --disable-smb \
    --disable-mqtt \
    --without-brotli \
    --without-zstd \
    --without-libidn2 \
    --without-libpsl \
    --without-libssh2 \
    --without-nghttp2

  make -j$NPROC && make install
  cd "$ROOT_DIR"
  rm -rf "$THIRD_DIR/curl-$ver"
  end_build
}

# ============ build libcopp ============
build_libcopp() {
  local ver="2.3.1"
  local tag="v$ver"
  local url="https://github.com/owent/libcopp"
  begin_build libcopp "$ver" "$THIRD_DIR/libcopp/lib/libcopp.a" "$THIRD_DIR/libcopp/include/libcopp" || return 0
  git_clone "$url" "$tag" "libcopp-$ver"

  cd "$THIRD_DIR/libcopp-$ver"
  cmake -B build -S . -DCMAKE_INSTALL_PREFIX=$THIRD_DIR/libcopp -DLIBCOPP_ENABLE_SEGMENTED_STACKS=YES -DCMAKE_CXX_STANDARD=17 -DCMAKE_CXX_STANDARD_REQUIRED=ON -DCMAKE_CXX_EXTENSIONS=OFF
  cd build
  make -j$NPROC && make install
  cd "$ROOT_DIR"
  rm -rf "$THIRD_DIR/libcopp-$ver"
  end_build
}

# ============ build gtest ============
build_gtest() {
  local ver="1.15.2"
  local dir="googletest-$ver"
  local pkg="$dir.tar.gz"
  local url="https://github.com/google/googletest/archive/refs/tags/v$ver.tar.gz"
  begin_build gtest "$ver" "$THIRD_DIR/gtest/googletest/include/gtest/gtest.h" || return 0
  fetch_and_extract "$url" "$pkg" "$dir"

  cd "$ROOT_DIR"
  rm -rf "$THIRD_DIR/gtest"
  mv "$THIRD_DIR/$dir" "$THIRD_DIR/gtest"
  end_build
}

# ============ install kcp (单个 .c + .h，直接落到 third/kcp 供 connd 编译) ============
build_kcp() {
  local ver="1.7"
  local dir="kcp-$ver"
  local url="https://github.com/skywind3000/kcp"
  begin_build kcp "$ver" "$THIRD_DIR/kcp/ikcp.c" "$THIRD_DIR/kcp/ikcp.h" || return 0
  git_clone "$url" "$ver" "$dir"

  mkdir -p "$THIRD_DIR/kcp"
  cp "$THIRD_DIR/$dir/ikcp.c" "$THIRD_DIR/$dir/ikcp.h" "$THIRD_DIR/kcp/"
  rm -rf "$THIRD_DIR/$dir"
  end_build
}

# ============ build libmariadb (MariaDB Connector/C，固定在 third/libmariadb) ============
# 注意：非阻塞 API（*_start / *_cont）由本库实现，dbproxy 的异步执行路径依赖它，
#       所以必须固定版本自行构建，不用系统库。
# 版本要求 >= 3.4.10：更早的版本（如 3.4.5）在异步等待掩码里不带 MYSQL_WAIT_TIMEOUT，
#       mysql_get_timeout_value_ms() 恒为 0，协作式超时（查询超时）无法生效。
build_libmariadb() {
  local ver="3.4.11"
  local dir="mariadb-connector-c-$ver"
  local pkg="$dir.tar.gz"
  local url="https://github.com/mariadb-corporation/mariadb-connector-c/archive/refs/tags/v$ver.tar.gz"
  begin_build libmariadb "$ver" "$THIRD_DIR/libmariadb/include/mariadb/mysql.h" "$THIRD_DIR/libmariadb/lib/mariadb/libmariadb.a" || return 0
  fetch_and_extract "$url" "$pkg" "$dir"

  cd "$THIRD_DIR/$dir"
  # openssl 是静态库（no-shared），libcrypto.a 依赖 pthread / dl，
  # 而 connector 的插件（sha256_password / parsec / caching_sha2_password 等）是 MODULE 目标，
  # 不会自动带上 pthread，必须显式给全量链接参数，否则报 pthread_rwlock_* 未定义符号。
  cmake -B build \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX="$THIRD_DIR/libmariadb" \
        -DWITH_SSL=ON \
        -DOPENSSL_ROOT_DIR="$THIRD_DIR/openssl" \
        -DOPENSSL_USE_STATIC_LIBS=ON \
        -DWITH_UNITTEST=OFF \
        -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
        -DCMAKE_C_FLAGS="-pthread" \
        -DCMAKE_C_STANDARD_LIBRARIES="-pthread -ldl"
  cmake --build build -j$NPROC
  cmake --install build
  cd "$ROOT_DIR"
  rm -rf "$THIRD_DIR/$dir"
  end_build
}

# ============ header-only deps ============
install_headers() {
  if [ "$FORCE" -eq 0 ] && [ -s "$THIRD_DIR/httplib/httplib.h" ] && [ -s "$THIRD_DIR/nlohmann/json.hpp" ]; then
    echo ">>> header-only deps already installed, skip"
    return 0
  fi

  # 先下载到临时文件再改名，避免中断/并发时留下半个头文件（会被后续增量检查误判为已安装）
  # httplib
  mkdir -p "$THIRD_DIR/httplib"
  curl -fsSL https://raw.githubusercontent.com/yhirose/cpp-httplib/master/httplib.h \
       -o "$THIRD_DIR/httplib/httplib.h.tmp"
  mv -f "$THIRD_DIR/httplib/httplib.h.tmp" "$THIRD_DIR/httplib/httplib.h"

  # nlohmann-json
  mkdir -p "$THIRD_DIR/nlohmann"
  curl -fsSL https://github.com/nlohmann/json/releases/download/v3.11.3/json.hpp \
       -o "$THIRD_DIR/nlohmann/json.hpp.tmp"
  mv -f "$THIRD_DIR/nlohmann/json.hpp.tmp" "$THIRD_DIR/nlohmann/json.hpp"
}

# 主流程
for dep in "${TARGETS[@]}"; do
  case "$dep" in
    gtest)      build_gtest ;;
    zlib)       build_zlib ;;
    openssl)    build_openssl ;;
    protobuf)   build_protobuf ;;
    curl)       build_curl ;;
    libcopp)    build_libcopp ;;
    libmariadb|mariadb) build_libmariadb ;;
    kcp)       build_kcp ;;
    headers)    install_headers ;;
    *) echo ">>> unknown dependency: $dep (可选: ${ALL_DEPS[*]} headers)"; exit 1 ;;
  esac
done

if [ ${#TARGETS[@]} -eq ${#ALL_DEPS[@]} ]; then
  install_headers
fi

echo ">>> All dependencies are built successfully into $THIRD_DIR"
