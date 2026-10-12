#!/bin/zsh
# macos/build_deps.sh — build the libraries a release LlamaBoss.app links
# statically, for Apple silicon and macOS 13.3 or later.
#
# The dev build links Homebrew's wxWidgets/Poco/OpenSSL, which Homebrew builds
# for the newest macOS only and which a user's Mac does not have.  A release
# build links these static libraries instead, so the app runs on macOS 13.3+
# (llama.cpp's own minimum) without Homebrew.
#
#   macos/build_deps.sh [deps-dir]      (default ~/LlamaBossDeps)
#
# Sources are checked against pinned SHA-256 values (OpenSSL's published
# checksum; Homebrew's recorded checksums for wxWidgets and Poco).  The result
# lands in <deps-dir>/prefix; configure the release build with
#   cmake -S . -B build_release -G Ninja -DCMAKE_BUILD_TYPE=Release \
#         -DLLAMABOSS_DEPS_PREFIX=<deps-dir>/prefix
set -euo pipefail

DEPS=${1:-$HOME/LlamaBossDeps}
PREFIX=$DEPS/prefix
SRC=$DEPS/src
DL=$DEPS/dl
JOBS=$(sysctl -n hw.ncpu)
export MACOSX_DEPLOYMENT_TARGET=13.3
ARCH=arm64

OPENSSL_VER=3.6.2
OPENSSL_SHA=aaf51a1fe064384f811daeaeb4ec4dce7340ec8bd893027eee676af31e83a04f
POCO_VER=1.15.4
POCO_SHA=d92e9e6711957a6b4415d4ffe0df5470b229bfa123334865c3b6a065030cd3a8
WX_VER=3.2.11
WX_SHA=6a129015bce2e914e4bf61ec4411854ad962801d47e92f2eb8340adb6a90af08

mkdir -p $PREFIX $SRC $DL

fetch() {   # url file sha
    [[ -f $DL/$2 ]] || curl -fL --retry 3 -o $DL/$2 $1
    local got=$(shasum -a 256 $DL/$2 | cut -d' ' -f1)
    [[ $got == $3 ]] || { echo "checksum mismatch for $2: $got" >&2; exit 1; }
}

fetch https://github.com/openssl/openssl/releases/download/openssl-$OPENSSL_VER/openssl-$OPENSSL_VER.tar.gz \
      openssl-$OPENSSL_VER.tar.gz $OPENSSL_SHA
fetch https://pocoproject.org/releases/poco-$POCO_VER/poco-$POCO_VER-all.tar.bz2 \
      poco-$POCO_VER-all.tar.bz2 $POCO_SHA
fetch https://github.com/wxWidgets/wxWidgets/releases/download/v$WX_VER/wxWidgets-$WX_VER.tar.bz2 \
      wxWidgets-$WX_VER.tar.bz2 $WX_SHA

# ── OpenSSL (static) ─────────────────────────────────────────────────
# openssldir=/private/etc/ssl: the default CA bundle is macOS's own
# /etc/ssl/cert.pem, which every Mac has (Homebrew's path would not exist).
if [[ ! -f $PREFIX/lib/libssl.a ]]; then
    rm -rf $SRC/openssl-$OPENSSL_VER && tar -xzf $DL/openssl-$OPENSSL_VER.tar.gz -C $SRC
    cd $SRC/openssl-$OPENSSL_VER
    ./Configure darwin64-arm64-cc no-shared no-tests no-docs no-apps \
        --prefix=$PREFIX --libdir=lib --openssldir=/private/etc/ssl \
        -mmacosx-version-min=$MACOSX_DEPLOYMENT_TARGET
    make -j$JOBS
    make install_sw
fi

# ── Poco (static; bundled expat/pcre2/zlib/utf8proc) ─────────────────
if [[ ! -f $PREFIX/lib/libPocoNetSSL.a ]]; then
    rm -rf $SRC/poco-$POCO_VER-all && tar -xjf $DL/poco-$POCO_VER-all.tar.bz2 -C $SRC
    cmake -S $SRC/poco-$POCO_VER-all -B $SRC/poco-build -G Ninja \
        -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=$PREFIX \
        -DCMAKE_OSX_ARCHITECTURES=$ARCH -DCMAKE_OSX_DEPLOYMENT_TARGET=$MACOSX_DEPLOYMENT_TARGET \
        -DBUILD_SHARED_LIBS=OFF -DPOCO_UNBUNDLED=OFF \
        -DOPENSSL_ROOT_DIR=$PREFIX -DOPENSSL_USE_STATIC_LIBS=TRUE \
        -DENABLE_FOUNDATION=ON -DENABLE_JSON=ON -DENABLE_NET=ON -DENABLE_NETSSL=ON \
        -DENABLE_CRYPTO=ON -DENABLE_UTIL=ON -DENABLE_XML=ON -DENABLE_ZIP=ON \
        -DENABLE_DATA=OFF -DENABLE_DATA_SQLITE=OFF -DENABLE_DATA_MYSQL=OFF \
        -DENABLE_DATA_POSTGRESQL=OFF -DENABLE_DATA_ODBC=OFF -DENABLE_MONGODB=OFF \
        -DENABLE_REDIS=OFF -DENABLE_PDF=OFF -DENABLE_PAGECOMPILER=OFF \
        -DENABLE_PAGECOMPILER_FILE2PAGE=OFF -DENABLE_ACTIVERECORD=OFF \
        -DENABLE_ACTIVERECORD_COMPILER=OFF -DENABLE_PROMETHEUS=OFF -DENABLE_JWT=OFF \
        -DENABLE_ENCODINGS=OFF -DENABLE_SEVENZIP=OFF -DENABLE_CPPPARSER=OFF \
        -DENABLE_POCODOC=OFF -DENABLE_TESTS=OFF -DENABLE_SAMPLES=OFF
    cmake --build $SRC/poco-build -j$JOBS
    cmake --install $SRC/poco-build
fi

# ── wxWidgets (static; built-in image/regex/expat/zlib) ──────────────
if [[ ! -f $PREFIX/bin/wx-config ]]; then
    rm -rf $SRC/wxWidgets-$WX_VER && tar -xjf $DL/wxWidgets-$WX_VER.tar.bz2 -C $SRC
    mkdir -p $SRC/wx-build && cd $SRC/wx-build
    $SRC/wxWidgets-$WX_VER/configure --prefix=$PREFIX \
        --with-osx_cocoa --with-macosx-version-min=$MACOSX_DEPLOYMENT_TARGET \
        --enable-macosx_arch=$ARCH --disable-shared --enable-unicode --with-cxx=17 \
        --disable-sys-libs --with-libpng=builtin --with-libjpeg=builtin \
        --with-libtiff=builtin --with-regex=builtin --with-expat=builtin \
        --with-zlib=builtin --without-liblzma --disable-webview --disable-mediactrl \
        --disable-tests
    make -j$JOBS
    make install
fi

echo "Done: $PREFIX"
