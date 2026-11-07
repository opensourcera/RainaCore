import math, os, random, struct, io
from PIL import Image, ImageDraw
os.chdir(os.path.dirname(os.path.abspath(__file__)))

# Made-up street-map tiles in the card's .pak format, light like real day tiles.
TERRAIN = False
def tile(z, x, y):
    random.seed(z * 1000003 + x * 1009 + y)
    im = Image.new('RGB', (256, 256), (222, 214, 178) if TERRAIN else (242, 239, 233))
    d = ImageDraw.Draw(im)
    for _ in range(3):
        a = random.randint(0, 200)
        b = random.randint(0, 200)
        d.rectangle([a, b, a + 60, b + 40], fill=(205, 235, 190))
    if (x + y) % 5 == 0:
        d.polygon([(0, 256), (120, 170), (256, 256)], fill=(170, 211, 223))
    for i in range(0, 256, 32):
        d.line([(i, 0), (i + random.randint(-8, 8), 256)], fill=(255, 255, 255), width=3)
        d.line([(0, i), (256, i + random.randint(-8, 8))], fill=(255, 255, 255), width=2)
    d.line([(0, random.randint(0, 256)), (256, random.randint(0, 256))], fill=(250, 200, 110), width=5)
    b = io.BytesIO()
    im.save(b, 'PNG')
    return b.getvalue()

for z in range(9, 18):
  for TERRAIN, base in ((False, 'sd/maps'), (True, 'sd/maps_terrain')):
    n = 2 ** z
    cx = int((-122.42 + 180) / 360 * n)
    cy = int((1 - math.asinh(math.tan(math.radians(37.78))) / math.pi) / 2 * n)
    for x in range(cx - 4, cx + 5):
        y0, y1 = cy - 4, cy + 4
        pngs = [tile(z, x, y) for y in range(y0, y1 + 1)]
        os.makedirs(f'{base}/{z}', exist_ok=True)
        off = [12 + 4 * (len(pngs) + 1)]
        for p in pngs:
            off.append(off[-1] + len(p))
        with open(f'{base}/{z}/{x}.pak', 'wb') as f:
            f.write(b'TPK1' + struct.pack('<II', y0, y1) + struct.pack(f'<{len(off)}I', *off) + b''.join(pngs))
