#!/usr/bin/env bash
# android/build.sh — ariadne for Android: one binary per ABI, linked against a static libcurl
# (mbedTLS underneath) and the NDK's static libc++/libomp, so it needs nothing on the phone but
# bionic. No proot, no rootfs, no Python. OpenSSL rides along statically for the device gateway.
#
#   android/build.sh                      # arm64-v8a and x86_64 (the emulator) into build/android/<abi>/ariadne
#   ABIS=arm64-v8a android/build.sh       # one ABI
#
# An app ships the result as lib/<abi>/libariadne.so: Android only lets an app exec what the
# package manager put in its native library dir, never a file it wrote into its own data dir.
#
# -Wno-pass-failed: clang reports a vectorize hint it could not honour as a warning; gcc, the
# host compiler, says nothing. It is a remark about the optimizer, not about the code.
#
# env: ANDROID_NDK_HOME (default: newest under ~/Android/Sdk/ndk), ANDROID_API (default 28 --
# posix_spawn), ABIS, JOBS
set -euo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
NDK=${ANDROID_NDK_HOME:-$(ls -d "$HOME"/Android/Sdk/ndk/* 2>/dev/null | sort -V | tail -1)}
API=${ANDROID_API:-28}
ABIS=${ABIS:-"arm64-v8a x86_64"}
JOBS=${JOBS:-$(nproc)}
[ -d "$NDK" ] || { echo "no NDK (set ANDROID_NDK_HOME)" >&2; exit 1; }

# Pinned sources. A tarball whose hash differs is refused, not built.
CURL_VER=8.11.1
CURL_URL=https://curl.se/download/curl-$CURL_VER.tar.xz
CURL_SHA=c7ca7db48b0909743eaef34250da02c19bc61d4f1dcedd6603f109409536ab56
MBED_VER=3.6.2
MBED_URL=https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-$MBED_VER/mbedtls-$MBED_VER.tar.bz2
MBED_SHA=8b54fb9bcf4d5a7078028e0520acddefb7900b3e66fec7f7175bb5b7d85ccdca
# OpenSSL is the device gateway's (TLS server, AES-GCM, the relay's signature); curl keeps mbedTLS,
# because Android names its CA files by OpenSSL's *old* subject hash and an OpenSSL curl would find
# none of them, while mbedTLS reads the directory as it stands. The two share no symbols.
# The hash is the one both github.com/openssl releases and openssl.org publish.
OSSL_VER=3.5.4
OSSL_URL=https://github.com/openssl/openssl/releases/download/openssl-$OSSL_VER/openssl-$OSSL_VER.tar.gz
OSSL_SHA=967311f84955316969bdb1d8d4b983718ef42338639c621ec4c34fddef355e99

out=$root/build/android
src=$out/src
mkdir -p "$src"

fetch() {   # url sha file
    local url=$1 sha=$2 f=$src/$3
    if [ ! -f "$f" ]; then curl -fsSL --retry 3 -o "$f.part" "$url"; mv "$f.part" "$f"; fi
    echo "$sha  $f" | sha256sum -c --quiet - || { echo "checksum mismatch: $f" >&2; rm -f "$f"; exit 1; }
}
fetch "$CURL_URL" "$CURL_SHA" curl-$CURL_VER.tar.xz
fetch "$MBED_URL" "$MBED_SHA" mbedtls-$MBED_VER.tar.bz2
fetch "$OSSL_URL" "$OSSL_SHA" openssl-$OSSL_VER.tar.gz
[ -d "$src/curl-$CURL_VER" ] || tar -C "$src" -xf "$src/curl-$CURL_VER.tar.xz"
[ -d "$src/mbedtls-$MBED_VER" ] || tar -C "$src" -xf "$src/mbedtls-$MBED_VER.tar.bz2"
[ -d "$src/openssl-$OSSL_VER" ] || tar -C "$src" -xf "$src/openssl-$OSSL_VER.tar.gz"

cmake_android() {   # abi srcdir builddir prefix args...
    local abi=$1 s=$2 b=$3 p=$4; shift 4
    mkdir -p "$(dirname "$b")"
    cmake -S "$s" -B "$b" -G "Unix Makefiles" \
        -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
        -DANDROID_ABI="$abi" -DANDROID_PLATFORM="android-$API" \
        -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$p" \
        -DCMAKE_FIND_ROOT_PATH="$p" -DCMAKE_POLICY_VERSION_MINIMUM=3.5 "$@" >"$b.log" 2>&1 \
        || { tail -30 "$b.log" >&2; exit 1; }
    cmake --build "$b" -j "$JOBS" >>"$b.log" 2>&1 || { tail -30 "$b.log" >&2; exit 1; }
    cmake --install "$b" >>"$b.log" 2>&1
}

host=linux-x86_64
for abi in $ABIS; do
    case $abi in
        arm64-v8a) triple=aarch64-linux-android ;;
        x86_64)    triple=x86_64-linux-android ;;
        *) echo "unsupported ABI $abi" >&2; exit 1 ;;
    esac
    p=$out/$abi/deps
    if [ ! -f "$p/lib/libmbedtls.a" ]; then
        echo "== $abi: mbedtls $MBED_VER"
        cmake_android "$abi" "$src/mbedtls-$MBED_VER" "$out/$abi/mbedtls-build" "$p" \
            -DENABLE_TESTING=OFF -DENABLE_PROGRAMS=OFF -DUSE_SHARED_MBEDTLS_LIBRARY=OFF -DUSE_STATIC_MBEDTLS_LIBRARY=ON
    fi
    if [ ! -f "$p/lib/libssl.a" ]; then
        echo "== $abi: openssl $OSSL_VER"
        case $abi in arm64-v8a) ot=android-arm64 ;; x86_64) ot=android-x86_64 ;; esac
        ob=$out/$abi/openssl-build
        rm -rf "$ob" && mkdir -p "$ob"
        ( cd "$ob" && export PATH="$NDK/toolchains/llvm/prebuilt/$host/bin:$PATH" ANDROID_NDK_ROOT="$NDK" &&
          perl "$src/openssl-$OSSL_VER/Configure" "$ot" no-shared no-tests no-docs -D__ANDROID_API__="$API" \
              --prefix="$p" --libdir=lib &&
          make -j"$JOBS" build_libs && make install_dev ) >"$ob.log" 2>&1 || { tail -30 "$ob.log" >&2; exit 1; }
    fi
    if [ ! -f "$p/lib/libcurl.a" ]; then
        echo "== $abi: curl $CURL_VER"
        # HTTP(S) only: the model endpoint, the MCP servers and nothing else. The CA path is the
        # system store every Android ships; mbedTLS reads the directory as it stands.
        cmake_android "$abi" "$src/curl-$CURL_VER" "$out/$abi/curl-build" "$p" \
            -DBUILD_SHARED_LIBS=OFF -DBUILD_STATIC_LIBS=ON -DBUILD_CURL_EXE=OFF -DBUILD_TESTING=OFF \
            -DBUILD_LIBCURL_DOCS=OFF -DBUILD_MISC_DOCS=OFF -DENABLE_CURL_MANUAL=OFF \
            -DHTTP_ONLY=ON -DCURL_USE_MBEDTLS=ON -DCURL_USE_OPENSSL=OFF \
            -DMBEDTLS_INCLUDE_DIR="$p/include" -DMBEDTLS_LIBRARY="$p/lib/libmbedtls.a" \
            -DMBEDX509_LIBRARY="$p/lib/libmbedx509.a" -DMBEDCRYPTO_LIBRARY="$p/lib/libmbedcrypto.a" \
            -DCURL_USE_LIBPSL=OFF -DUSE_LIBIDN2=OFF -DCURL_USE_LIBSSH2=OFF -DUSE_NGHTTP2=OFF \
            -DCURL_BROTLI=OFF -DCURL_ZSTD=OFF -DCURL_ZLIB=OFF -DENABLE_THREADED_RESOLVER=ON \
            -DCURL_CA_BUNDLE=none -DCURL_CA_PATH=/system/etc/security/cacerts
    fi
    echo "== $abi: ariadne (API $API)"
    cxx="$NDK/toolchains/llvm/prebuilt/$host/bin/$triple$API-clang++"
    make -C "$root" -j1 BUILD="$out/$abi" CXX="$cxx" \
        CXXFLAGS="-std=c++20 -Wall -Wextra -Werror -Wno-pass-failed -O2 -pipe -Iinclude -I$p/include -static-libstdc++ -static-openmp" \
        CURL_LIBS="-L$p/lib -lcurl -lmbedtls -lmbedx509 -lmbedcrypto" TLS_LIBS="-lssl -lcrypto" \
        "$out/$abi/ariadne"
    "$NDK/toolchains/llvm/prebuilt/$host/bin/llvm-strip" "$out/$abi/ariadne"
    ls -l "$out/$abi/ariadne"
done
