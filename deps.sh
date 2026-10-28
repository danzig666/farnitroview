#!/bin/sh
# Fetches and builds the optional decoder dependencies into third_party/deps/ (not in git):
#   lunasvg + plutovg (MIT)       SVG, at the commit jpeg-nitro uses, with its patch
#   dav1d (BSD-2-Clause)          AV1 for AVIF, static, built with llvm-mingw; meson + ninja from
#                                 pip (in a venv of their own), nasm from nasm.us
# build.sh uses whatever is there (SVG / AVIF are left out when it is missing).
#   LLVM_MINGW=path/to/llvm-mingw (default: ext/llvm-mingw-*, else clang on PATH)
#   PYTHON=python3 interpreter for meson (default: python)
set -eu
cd "$(dirname "$0")"
ROOT=$PWD
NITRO=$ROOT/third_party/jpeg-nitro
D=$ROOT/third_party/deps
mkdir -p "$D"
LUNASVG_COMMIT=$(sed -n 's/^LUNASVG_COMMIT=\([0-9a-f]*\).*/\1/p' "$NITRO/scripts/get-deps.sh")
DAV1D_VER=$(sed -n 's/^DAV1D_VER=\([0-9.]*\).*/\1/p' "$NITRO/scripts/get-deps.sh")
NASM_VER=2.16.03
LLVM_MINGW=${LLVM_MINGW:-$(ls -d "$ROOT"/ext/llvm-mingw-* 2>/dev/null | head -1)}
if [ -n "$LLVM_MINGW" ]; then BIN="$LLVM_MINGW/bin"; else BIN=$(dirname "$(command -v x86_64-w64-mingw32-clang)"); fi
PY=${PYTHON:-python}

# lunasvg
STAMP=$LUNASVG_COMMIT-$(cksum < "$NITRO/patches/lunasvg.patch" | cut -d' ' -f1)
if [ ! -f "$D/lunasvg/.stamp-$STAMP" ]; then
    echo "lunasvg $LUNASVG_COMMIT"
    rm -rf "$D/lunasvg" "$D/lunasvg-$LUNASVG_COMMIT"
    curl -fsSL -o "$D/lunasvg.tar.gz" "https://github.com/sammycage/lunasvg/archive/$LUNASVG_COMMIT.tar.gz"
    tar xf "$D/lunasvg.tar.gz" -C "$D" && rm "$D/lunasvg.tar.gz"
    (cd "$D/lunasvg-$LUNASVG_COMMIT" && patch -p1 -s < "$NITRO/patches/lunasvg.patch")
    mv "$D/lunasvg-$LUNASVG_COMMIT" "$D/lunasvg"
    touch "$D/lunasvg/.stamp-$STAMP"
fi

# dav1d
if [ ! -f "$D/dav1d/.version-$DAV1D_VER" ]; then
    echo "dav1d $DAV1D_VER"
    if [ ! -x "$D/tools/nasm-$NASM_VER/nasm.exe" ]; then
        curl -fsSL -o "$D/nasm.zip" "https://www.nasm.us/pub/nasm/releasebuilds/$NASM_VER/win64/nasm-$NASM_VER-win64.zip"
        mkdir -p "$D/tools" && unzip -q -o "$D/nasm.zip" -d "$D/tools" && rm "$D/nasm.zip"
    fi
    if [ ! -x "$D/tools/venv/Scripts/meson.exe" ]; then
        "$PY" -m venv "$D/tools/venv"
        "$D/tools/venv/Scripts/python" -m pip install -q meson ninja
    fi
    rm -rf "$D/dav1d" "$D/dav1d-$DAV1D_VER"
    curl -fsSL -o "$D/dav1d.tar.xz" "https://downloads.videolan.org/pub/videolan/dav1d/$DAV1D_VER/dav1d-$DAV1D_VER.tar.xz"
    tar xf "$D/dav1d.tar.xz" -C "$D" && rm "$D/dav1d.tar.xz"
    W() { cygpath -m "$1" 2>/dev/null || echo "$1"; }
    cat > "$D/dav1d-$DAV1D_VER/native.ini" <<EOT
[binaries]
c = '$(W "$BIN/x86_64-w64-mingw32-clang.exe")'
ar = '$(W "$BIN/llvm-ar.exe")'
strip = '$(W "$BIN/llvm-strip.exe")'
windres = '$(W "$BIN/x86_64-w64-mingw32-windres.exe")'
nasm = '$(W "$D/tools/nasm-$NASM_VER/nasm.exe")'
EOT
    (cd "$D/dav1d-$DAV1D_VER" &&
     PATH="$D/tools/venv/Scripts:$D/tools/nasm-$NASM_VER:$PATH" &&
     meson setup build --native-file native.ini --buildtype=release --default-library=static \
         -Denable_tools=false -Denable_tests=false -Denable_examples=false \
         --prefix="$(W "$D/dav1d")" --libdir=lib > ../dav1d-setup.log &&
     meson install -C build > ../dav1d-install.log) || { tail -30 "$D/dav1d-setup.log"; exit 1; }
    rm -rf "$D/dav1d-$DAV1D_VER"
    touch "$D/dav1d/.version-$DAV1D_VER"
fi
echo "dependencies ready in third_party/deps (lunasvg $LUNASVG_COMMIT, dav1d $DAV1D_VER)"
