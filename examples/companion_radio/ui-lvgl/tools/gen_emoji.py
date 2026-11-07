# Generates rc_emoji.cpp: 16x16 colour emoji glyphs from Noto Color Emoji.
import sys, io, struct
from fontTools.ttLib import TTFont
from PIL import Image
src, out = sys.argv[1], sys.argv[2]
SIZE = 16
f = TTFont(src)
cmap = f.getBestCmap()
strikes = f['CBDT'].strikeData
strike = strikes[0]
SKIP = {0x23, 0x2A, 0xA9, 0xAE, 0x200D, 0x20E3, 0xFE0F, 0xFE0E, 0x2122} | set(range(0x30, 0x3A)) \
     | set(range(0x1F3FB, 0x1F400)) | set(range(0xE0020, 0xE0080))
cps = []
blob = bytearray()
preview = []
for cp in sorted(cmap):
    if cp in SKIP or cp < 0x80 or cp >= 0xE0000: continue
    g = cmap[cp]
    if g not in strike: continue
    im = strike[g]
    im.decompile() if hasattr(im, 'decompile') and not hasattr(im, 'imageData') else None
    data = im.imageData
    if isinstance(data, bytes) and data[:8] != b'\x89PNG\r\n\x1a\n':
        i = data.find(b'\x89PNG'); data = data[i:]
    png = Image.open(io.BytesIO(data)).convert('RGBA')
    s = max(png.size)
    sq = Image.new('RGBA', (s, s), (0, 0, 0, 0))
    sq.paste(png, ((s - png.width) // 2, (s - png.height) // 2))
    small = sq.convert('RGBa').resize((SIZE, SIZE), Image.LANCZOS).convert('RGBA')
    alpha = bytearray()
    for r, g2, b, a in small.getdata():
        v = ((r >> 3) << 11) | ((g2 >> 2) << 5) | (b >> 3)
        blob += struct.pack('<H', v)
        alpha.append(a)
    blob += alpha
    cps.append(cp)
    preview.append(small)
n = len(cps)
print('glyphs', n, 'bytes', len(blob), 'range', hex(cps[0]), hex(cps[-1]))
with open(out, 'w', newline='\n') as o:
    o.write('// Generated from Noto Color Emoji (SIL OFL 1.1, see licenses/NotoEmoji-OFL.txt).\n')
    o.write('// 16x16 RGB565A8. Do not edit by hand.\n\n')
    o.write('#include <lvgl.h>\n\n')
    o.write('#define EMOJI_COUNT %d\n#define EMOJI_SIZE %d\n\n' % (n, SIZE))
    o.write('static const uint32_t emoji_cps[EMOJI_COUNT] = {\n')
    for i in range(0, n, 10): o.write(','.join('0x%X' % c for c in cps[i:i+10]) + ',\n')
    o.write('};\n\nstatic const uint8_t emoji_px[] LV_ATTRIBUTE_LARGE_CONST = {\n')
    for i in range(0, len(blob), 36): o.write(','.join(str(b) for b in blob[i:i+36]) + ',\n')
    o.write('};\n\n#define G(i) {{LV_IMAGE_HEADER_MAGIC, LV_COLOR_FORMAT_RGB565A8, 0, EMOJI_SIZE, EMOJI_SIZE, EMOJI_SIZE * 2, 0}, EMOJI_SIZE * EMOJI_SIZE * 3, emoji_px + (i) * EMOJI_SIZE * EMOJI_SIZE * 3, NULL}\n')
    o.write('static const lv_image_dsc_t emoji_img[EMOJI_COUNT] = {\n')
    for i in range(0, n, 12): o.write(','.join('G(%d)' % j for j in range(i, min(i+12, n))) + ',\n')
    o.write('};\n#undef G\n')
# contact sheet for checking the result
sheet = Image.new('RGB', (40*18, 18*((n+39)//40)), (27, 33, 40))
for i, im in enumerate(preview):
    sheet.paste(im, ((i % 40)*18+1, (i//40)*18+1), im)
sheet.save(out + '.png')
