#!/bin/sh
# Builds dist/FarNitroView/ (FarNitroView.dll + language and help files) for Far Manager 3 x64
# with llvm-mingw (clang: the jpeg-nitro decoders use blocks, -fblocks).
#
#   LLVM_MINGW=path/to/llvm-mingw   default: ext/llvm-mingw-* (see README), else clang on PATH
#   ARCH=x86-64-v3 (AVX2, default) | x86-64-v2 (older CPUs)
#   VERSION=1.2.3                   version written into the DLL and shown by Far (default 1.0.0)
#   OUT=dir                         default: dist/FarNitroView
set -e
cd "$(dirname "$0")"
ROOT=$PWD
NITRO=$ROOT/third_party/jpeg-nitro
FARSDK=$ROOT/third_party/farsdk
LLVM_MINGW=${LLVM_MINGW:-$(ls -d "$ROOT"/ext/llvm-mingw-* 2>/dev/null | head -1)}
if [ -n "$LLVM_MINGW" ]; then BIN="$LLVM_MINGW/bin/"; else BIN=""; fi
CC="${BIN}x86_64-w64-mingw32-clang"
CXX="${BIN}x86_64-w64-mingw32-clang++"
RC="${BIN}x86_64-w64-mingw32-windres"
ARCH=${ARCH:-x86-64-v3}
VERSION=${VERSION:-1.0.0}
OUT=${OUT:-$ROOT/dist/FarNitroView}
OBJ=$ROOT/build/obj-$ARCH

[ -f "$NITRO/src/nitrojpeg.c" ] || { echo "jpeg-nitro missing: git submodule update --init" >&2; exit 1; }
V1=$(echo "$VERSION" | cut -d. -f1); V2=$(echo "$VERSION" | cut -d. -f2); V3=$(echo "$VERSION" | cut -d. -f3)
VDEFS="-DNV_VERSION_MAJOR=${V1:-1} -DNV_VERSION_MINOR=${V2:-0} -DNV_VERSION_BUILD=${V3:-0}"
mkdir -p "$OBJ" "$OUT"

CPU="-march=$ARCH"
[ "$ARCH" = x86-64-v3 ] && CPU="$CPU -mbranches-within-32B-boundaries"
CFLAGS="-O3 $CPU -fblocks -Wall -Wno-unused-parameter -I$NITRO/src -D_WIN32_WINNT=0x0A00 -DUNICODE -D_UNICODE"
CXXFLAGS="$CFLAGS -std=c++20 -I$FARSDK $VDEFS -Wno-missing-field-initializers -Wno-unused-function"

pids=""
for f in nitrojpeg nitropng nitropsd nitroheic nitrohevc nitrodng nitrowebp nitrotiff nitro_load nitro_convert; do
    [ "$OBJ/$f.o" -nt "$NITRO/src/$f.c" ] || { "$CC" $CFLAGS -c "$NITRO/src/$f.c" -o "$OBJ/$f.o" & pids="$pids $!"; }
done
"$CC" $CFLAGS -c src/par.c -o "$OBJ/par.o" & pids="$pids $!"
for f in plugin config viewer overlay render decode; do
    "$CXX" $CXXFLAGS -c src/$f.cpp -o "$OBJ/$f.o" & pids="$pids $!"
done
"$RC" $VDEFS src/version.rc -O coff -o "$OBJ/version.o"
fail=0
for p in $pids; do wait $p || fail=1; done
[ $fail = 0 ] || { echo "compile failed" >&2; exit 1; }

"$CXX" -shared -static -O3 -o "$OUT/FarNitroView.dll" "$OBJ"/*.o src/farnitroview.def \
    -lwindowscodecs -lole32 -loleaut32 -lshlwapi -luiautomationcore -luser32 -lgdi32 -luuid -lpthread \
    -Wl,--gc-sections -s
cp package/* "$OUT/"
cp "$NITRO/LICENSE" "$OUT/LICENSE-jpeg-nitro.txt"
cp LICENSE "$OUT/LICENSE.txt"
echo "built $OUT/FarNitroView.dll ($ARCH, $VERSION)"
