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
for f in sorted(os.listdir(out)):
    if f.endswith(".rgb"): continue
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
