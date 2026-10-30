# makes test images (and Pillow's own decoding of each, as raw RGB, for comparison)
import os, sys, math
from PIL import Image, ImageDraw
out = sys.argv[1]
os.makedirs(out, exist_ok=True)
W, H = 1600, 1000
im = Image.new("RGB", (W, H))
px = im.load()
for y in range(H):
    for x in range(W):
        px[x, y] = (x * 255 // W, y * 255 // H, int(128 + 127 * math.sin(x / 37.0 + y / 23.0)))
d = ImageDraw.Draw(im)
d.rectangle([40, 40, 400, 300], fill=(255, 255, 255))
d.text((60, 60), "TOP LEFT", fill=(0, 0, 0))
d.ellipse([900, 300, 1400, 800], outline=(255, 0, 0), width=12)
for i in range(0, W, 16): d.line([i, 900, i, 1000], fill=(0, 0, 0) if (i // 16) % 2 else (255, 255, 255), width=1)
im.save(f"{out}/base420.jpg", quality=92)
im.save(f"{out}/prog.jpg", quality=90, progressive=True)
im.save(f"{out}/s444.jpg", quality=95, subsampling=0)
im.convert("L").save(f"{out}/gray.jpg", quality=90)
ex = Image.Exif(); ex[0x0112] = 6
im.save(f"{out}/orient6.jpg", quality=90, exif=ex.tobytes())
im.convert("CMYK").save(f"{out}/cmyk.jpg", quality=90)
rgba = im.copy().convert("RGBA")
a = Image.new("L", (W, H))
ap = a.load()
for y in range(H):
    for x in range(W): ap[x, y] = 255 if x < W // 2 else (y * 255 // H)
rgba.putalpha(a)
rgba.save(f"{out}/rgba.png")
im.save(f"{out}/rgb.png")
im.quantize(64).save(f"{out}/pal.png")
im.save(f"{out}/lzw.tif", compression="tiff_lzw")
im.save(f"{out}/lossy.webp", quality=85)
im.save(f"{out}/lossless.webp", lossless=True)
im.quantize(128).save(f"{out}/anim.gif")
im.save(f"{out}/plain.bmp")
im.resize((7000, 4500)).save(f"{out}/big.jpg", quality=88)
im.resize((40, 25), Image.NEAREST).save(f"{out}/tiny.png")
# AVIF (nitroavif + dav1d), lossless-ish quality so the comparison is meaningful
from PIL import features
if features.check("avif"):
    im.save(f"{out}/photo.avif", quality=90)
# animation: 6 frames, a square moving over a gradient; multi-page TIFF: 3 pages
frames = []
for i in range(6):
    f = im.resize((320, 200)).copy()
    ImageDraw.Draw(f).rectangle([20 + i * 40, 60, 80 + i * 40, 120], fill=(255, 255, 0))
    frames.append(f.quantize(64))
frames[0].save(f"{out}/anim6.gif", save_all=True, append_images=frames[1:], duration=80, loop=0)
pages = [im.resize((800, 500)), im.resize((600, 400)).rotate(180), Image.new("RGB", (300, 300), (0, 128, 255))]
pages[0].save(f"{out}/pages3.tif", save_all=True, append_images=pages[1:], compression="tiff_lzw")
open(f"{out}/vector.svg", "w").write(
    '<svg xmlns="http://www.w3.org/2000/svg" width="400" height="250" viewBox="0 0 400 250">'
    '<rect width="400" height="250" fill="#1f6feb"/><circle cx="200" cy="125" r="100" fill="#ffd33d"/>'
    '<path d="M40 210 L360 40" stroke="#d73a49" stroke-width="12"/></svg>')
# PDF, written by hand: 3 pages; a blue page with a yellow square (as vector.svg's disc, the harness
# looks for it), the same turned by /Rotate, a smaller one. And a file that only claims to be a PDF.
def pdf(path, pages):
    objs = [b"<< /Type /Catalog /Pages 2 0 R >>", None]
    kids = []
    for (w, h, rot, content) in pages:
        kids.append(f"{len(objs) + 1} 0 R")
        objs.append(f"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 {w} {h}] /Rotate {rot} /Resources << >> /Contents {len(objs) + 2} 0 R >>".encode())
        objs.append(b"<< /Length %d >>\nstream\n%s\nendstream" % (len(content), content))
    objs[1] = f"<< /Type /Pages /Count {len(pages)} /Kids [{' '.join(kids)}] >>".encode()
    b = bytearray(b"%PDF-1.7\n")
    offs = []
    for i, o in enumerate(objs):
        offs.append(len(b))
        b += b"%d 0 obj\n%s\nendobj\n" % (i + 1, o)
    xref = len(b)
    b += b"xref\n0 %d\n0000000000 65535 f \n" % (len(objs) + 1)
    for o in offs: b += b"%010d 00000 n \n" % o
    b += b"trailer\n<< /Size %d /Root 1 0 R >>\nstartxref\n%d\n%%%%EOF\n" % (len(objs) + 1, xref)
    open(path, "wb").write(b)
page1 = b"0.122 0.435 0.922 rg 0 0 400 250 re f 1 0.827 0.239 rg 100 50 200 150 re f 0.843 0.227 0.286 rg 10 10 40 40 re f"
pdf(f"{out}/vector.pdf", [(400, 250, 0, page1), (400, 250, 90, page1), (300, 300, 0, b"0 0.5 1 rg 0 0 300 300 re f")])
open(f"{out}/damaged.pdf", "wb").write(b"%PDF-1.7\nnothing a reader could use\n" * 50)
# for the updater's test: a release's zip, and one without the plugin in it
import shutil, zipfile
with zipfile.ZipFile(f"{out}/update-ok.zip", "w") as z:
    z.writestr("FarNitroView/FarNitroView.dll", "new dll")
    z.writestr("FarNitroView/FarNitroViewEng.lng", "new lng")
    z.writestr("FarNitroView/licenses-pdfium/a.txt", "new license")
with zipfile.ZipFile(f"{out}/update-nodll.zip", "w") as z:
    z.writestr("FarNitroView/FarNitroViewEng.lng", "new lng")
shutil.rmtree(f"{out}/plugin", ignore_errors=True)
for f in sorted(os.listdir(out)):
    if f.endswith((".rgb", ".pdf", ".zip")) or f == "plugin": continue
    try:
        r = Image.open(f"{out}/{f}")
        from PIL import ImageOps
        r = ImageOps.exif_transpose(r)
        if r.mode in ("RGBA", "LA", "P", "PA"):
            r = r.convert("RGBA"); bg = Image.new("RGBA", r.size, (0, 0, 0, 255)); bg.alpha_composite(r); r = bg
        r = r.convert("RGB")
        open(f"{out}/{f}.rgb", "wb").write(r.size[0].to_bytes(4, "little") + r.size[1].to_bytes(4, "little") + r.tobytes())
    except Exception as e:
        print("ref fail", f, e)
print("ok")
