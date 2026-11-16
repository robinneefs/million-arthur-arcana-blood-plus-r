# Makes assets/icon/arcana_plusr.ico (launcher, installer, shortcuts) from assets/icon/plus-r.png:
# the gold +R on a dark crimson tile with a red border. Needs Pillow.  python3 tools/make-icon.py
import os
from PIL import Image, ImageDraw
root = os.path.join(os.path.dirname(__file__), '..')
art = Image.open(os.path.join(root, 'assets/icon/plus-r.png')).convert('RGBA')
a = art.split()[3].point(lambda v: 255 if v > 24 else 0)
art = art.crop(a.getbbox())

def tile(size):
    s = size * 4                                   # draw big, scale down (smooth edges)
    t = Image.new('RGBA', (s, s), (0, 0, 0, 0))
    pad, rad, bw = s * 0.03, s * 0.19, max(4, s * 0.035)
    g = Image.new('RGBA', (s, s))
    d = ImageDraw.Draw(g)
    for y in range(s):
        k = y / s
        d.line((0, y, s, y), fill=(int(78 - 56 * k), int(9 - 7 * k), int(11 - 9 * k), 255))
    m = Image.new('L', (s, s), 0)
    ImageDraw.Draw(m).rounded_rectangle((pad, pad, s - pad, s - pad), radius=rad, fill=255)
    t.paste(g, (0, 0), m)
    ImageDraw.Draw(t).rounded_rectangle((pad, pad, s - pad, s - pad), radius=rad, outline=(215, 38, 32, 255), width=int(bw))
    fill = 0.86 if size >= 32 else 0.94           # small sizes: the letters a bit bigger
    w = int(s * fill); h = int(art.height * w / art.width)
    t.alpha_composite(art.resize((w, h), Image.LANCZOS), ((s - w) // 2, (s - h) // 2))
    return t.resize((size, size), Image.LANCZOS)

sizes = [256, 128, 64, 48, 40, 32, 24, 20, 16]
imgs = [tile(z) for z in sizes]
imgs[0].save(os.path.join(root, 'assets/icon/arcana_plusr.ico'), format='ICO', sizes=[(z, z) for z in sizes], append_images=imgs[1:])
imgs[0].save(os.path.join(root, 'assets/icon/arcana_plusr-256.png'))
print('assets/icon/arcana_plusr.ico')
