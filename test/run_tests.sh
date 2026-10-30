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
DAV1D=$(ls third_party/deps/dav1d/lib/libdav1d.a 2>/dev/null || true)
DEFS=$( [ -n "$DAV1D" ] && echo -DNV_DAV1D)
PY=${PYTHON:-python}

"${BIN}x86_64-w64-mingw32-clang++" -O2 -std=c++20 -march=$ARCH -municode $DEFS -Ithird_party/jpeg-nitro/src \
    test/harness.cpp $O/decode.o $O/render.o $O/pdf.o $O/postproc.o $O/par.o $O/nitro*.o $O/svg/*.o $DAV1D -static \
    -lwindowscodecs -lole32 -loleaut32 -lshlwapi -luuid -lpthread -o build/harness.exe
"$PY" -I test/mkimages.py test/images
PDFS=""
if [ -f third_party/deps/pdfium/bin/pdfium.dll ]; then
    cp third_party/deps/pdfium/bin/pdfium.dll build/
    PDFS="vector.pdf damaged.pdf"
fi
cd test/images
../../build/harness.exe base420.jpg prog.jpg s444.jpg gray.jpg orient6.jpg cmyk.jpg rgba.png rgb.png pal.png \
    lzw.tif lossy.webp lossless.webp anim.gif plain.bmp big.jpg tiny.png anim6.gif pages3.tif \
    $(ls vector.svg photo.avif 2>/dev/null) $PDFS | tee ../../build/test-results.txt
# exact everywhere except lossy WebP (nitrowebp has no loop filter yet) and CMYK (WIC and Pillow
# convert CMYK differently)
# (lossless WebP needs Windows' WebP codec, which Windows Server lacks: only a warning)
awk '
    $1 == "lossless.webp" && / FAILED/ { print "::warning::lossless.webp: no WIC WebP codec on this system"; next }
    $1 == "damaged.pdf" { if ($0 !~ / FAILED/) { print "FAIL: damaged.pdf was not refused"; bad = 1 } next }
    / FAILED/ { print "FAIL: " $0; bad = 1; next }
    { psnr = $(NF - 2); want = 99 }
    $1 == "lossy.webp" { want = 35 }
    $1 == "cmyk.jpg" { want = 0 }
    $1 == "photo.avif" { want = 35 }   # (nitroavif and libavif round the colour conversion differently)
    $1 == "anim6.gif" && $(NF - 6) + 0 != 6 { print "FAIL: anim6.gif: " $(NF - 6) " frames, not 6"; bad = 1 }
    $1 == "pages3.tif" && $(NF - 4) !~ /^3\(300\)/ { print "FAIL: pages3.tif: pages " $(NF - 4); bad = 1 }
    $1 == "vector.pdf" && $(NF - 4) !~ /^3\(400\)/ { print "FAIL: vector.pdf: pages " $(NF - 4); bad = 1 }
    psnr + 0 < want { print "FAIL: " $1 " PSNR " psnr " < " want; bad = 1 }
    END { if (bad) exit 1; print "all decoder tests passed" }
' ../../build/test-results.txt
