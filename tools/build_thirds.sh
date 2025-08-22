#!/usr/bin/env bash
set -e

TOOLS_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$TOOLS_DIR/.."
THIRD_DIR="$ROOT_DIR/third"
PKG_DIR="$TOOLS_DIR/packages"
NPROC=$(nproc 2>/dev/null || sysctl -n hw.ncpu)

mkdir -p "$THIRD_DIR" "$PKG_DIR"

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

# ============ build zlib ============
build_zlib() {
  local ver="1.3.1"
  local pkg="zlib-$ver.tar.gz"
  local url="https://zlib.net/$pkg"
  fetch_and_extract "$url" "$pkg" "zlib-$ver"

  cd "$THIRD_DIR/zlib-$ver"
  CFLAGS="-fPIC" ./configure --prefix="$THIRD_DIR/zlib"
  make -j$NPROC && make install
  cd "$ROOT_DIR"
  rm -rf "$THIRD_DIR/zlib-$ver"
}

# ============ build openssl ============
build_openssl() {
  local ver="3.3.1"
  local pkg="openssl-$ver.tar.gz"
  local url="https://www.openssl.org/source/$pkg"
  fetch_and_extract "$url" "$pkg" "openssl-$ver"

  cd "$THIRD_DIR/openssl-$ver"
  ./Configure --prefix="$THIRD_DIR/openssl" no-shared
  make -j$NPROC && make install_sw
  rm -f "$THIRD_DIR/openssl/lib"
  ln -sf "$THIRD_DIR/openssl/lib64" "$THIRD_DIR/openssl/lib"
  cd "$ROOT_DIR"
  rm -rf "$THIRD_DIR/openssl-$ver"
}

# ============ build protobuf ============
build_protobuf() {
  local ver="3.21.12"
  local pkg="v$ver.tar.gz"
  local url="https://github.com/protocolbuffers/protobuf/archive/refs/tags/v$ver.tar.gz"
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
}

# ============ build curl ============
build_curl() {
  local ver="8.7.1"
  local pkg="curl-$ver.tar.gz"
  local url="https://curl.se/download/$pkg"
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
    --without-libssh2

  make -j$NPROC && make install
  cd "$ROOT_DIR"
  rm -rf "$THIRD_DIR/curl-$ver"
}

# ============ header-only deps ============
install_headers() {
  # httplib
  mkdir -p "$THIRD_DIR/httplib"
  curl -sSL https://raw.githubusercontent.com/yhirose/cpp-httplib/master/httplib.h \
       -o "$THIRD_DIR/httplib/httplib.h"

  # nlohmann-json
  mkdir -p "$THIRD_DIR/nlohmann"
  curl -sSL https://github.com/nlohmann/json/releases/download/v3.11.3/json.hpp \
       -o "$THIRD_DIR/nlohmann/json.hpp"
}

# 主流程
build_zlib
build_openssl
build_protobuf
build_curl
install_headers

echo ">>> All dependencies are built successfully into $THIRD_DIR"

