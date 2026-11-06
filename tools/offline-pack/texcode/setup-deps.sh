#!/usr/bin/env bash
# setup-deps.sh -- fetch the third-party headers/binaries the offline-pack
# prototype needs, without sudo and without touching the system.
#
#   boost 1.74 headers  (astc.cpp needs boost/container/static_vector.hpp)
#   fmt 11.1.4 headers  (Eden's logging.h needs the fmt v9+ format_string API;
#                        Ubuntu 22.04 ships fmt 8.1, which is too old)
#   astcenc 4.7.0        (reference ASTC encoder for the selftest; 5.7.1 needs
#                        glibc 2.38, 253 has 2.35, so 4.7.0 is the newest
#                        release whose binary runs here)
#
# Everything lands in $HOME/.cache/eden-offline-pack/ (persistent on 253) and
# is reused on later runs. Nothing is installed system-wide.
set -euo pipefail

CACHE="${EDEN_OFFLINE_PACK_CACHE:-$HOME/.cache/eden-offline-pack}"
mkdir -p "$CACHE"
cd "$CACHE"

if [[ ! -f boost/usr/include/boost/container/static_vector.hpp ]]; then
    echo "--- boost 1.74 headers (apt-get download, no sudo) ---"
    mkdir -p boost-dl && cd boost-dl
    apt-get download libboost1.74-dev libboost-container1.74-dev >/dev/null 2>&1
    mkdir -p ../boost
    dpkg -x libboost1.74-dev_*.deb ../boost
    dpkg -x libboost-container1.74-dev_*.deb ../boost
    cd ..
    rm -rf boost-dl
fi

if [[ ! -f fmt11/fmt-11.1.4/include/fmt/format.h ]]; then
    echo "--- fmt 11.1.4 headers ---"
    curl -sL --max-time 120 -o fmt11.zip \
        https://github.com/fmtlib/fmt/releases/download/11.1.4/fmt-11.1.4.zip
    unzip -o -q fmt11.zip -d fmt11
    rm -f fmt11.zip
fi

if [[ ! -x astcenc-4.7.0/bin/astcenc-avx2 && ! -x astcenc-4.7.0/bin/astcenc-sse2 ]]; then
    echo "--- astcenc 4.7.0 ---"
    curl -sL --max-time 120 -o astcenc47.zip \
        https://github.com/ARM-software/astc-encoder/releases/download/4.7.0/astcenc-4.7.0-linux-x64.zip
    unzip -o -q astcenc47.zip -d astcenc-4.7.0
    rm -f astcenc47.zip
fi

echo "deps ready in $CACHE:"
ls "$CACHE"
echo "boost:  $CACHE/boost/usr/include"
echo "fmt:    $CACHE/fmt11/fmt-11.1.4/include"
echo "astcenc: $CACHE/astcenc-4.7.0/bin/astcenc-avx2 (fallback: astcenc-sse2)"
