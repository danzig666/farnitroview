#!/bin/sh
# Decoder regression test: makes test images with Pillow, decodes them the way the plugin does
# (test/harness.cpp) and compares with Pillow's decoding. Needs build.sh to have run (ARCH as there)
# and Python with Pillow.
set -e
cd "$(dirname "$0")/.."
ROOT=$PWD
ARCH=${ARCH:-x86-64-v3}
LLVM_MINGW=${LLVM_MINGW:-$(ls -d "$ROOT"/ext/llvm-mingw-* 2>/dev/null | head -1)}
if [ -n "$LLVM_MINGW" ]; then BIN="$LLVM_MINGW/bin/"; else BIN=""; fi
O=build/obj-$ARCH
PY=${PYTHON:-python}

"${BIN}x86_64-w64-mingw32-clang++" -O2 -std=c++20 -march=$ARCH -municode -Ithird_party/jpeg-nitro/src \
    test/harness.cpp $O/decode.o $O/render.o $O/par.o $O/nitro*.o -static \
    -lwindowscodecs -lole32 -loleaut32 -lshlwapi -luuid -lpthread -o build/harness.exe
"$PY" -I test/mkimages.py test/images
cd test/images
../../build/harness.exe base420.jpg prog.jpg s444.jpg gray.jpg orient6.jpg cmyk.jpg rgba.png rgb.png pal.png \
    lzw.tif lossy.webp lossless.webp anim.gif plain.bmp big.jpg tiny.png | tee ../../build/test-results.txt
# exact everywhere except lossy WebP (nitrowebp has no loop filter yet) and CMYK (WIC and Pillow
# convert CMYK differently)
awk '
    / FAILED/ { print "FAIL: " $0; bad = 1; next }
    { psnr = $(NF - 2); want = 99 }
    $1 == "lossy.webp" { want = 35 }
    $1 == "cmyk.jpg" { want = 0 }
    psnr + 0 < want { print "FAIL: " $1 " PSNR " psnr " < " want; bad = 1 }
    END { if (bad) exit 1; print "all decoder tests passed" }
' ../../build/test-results.txt
