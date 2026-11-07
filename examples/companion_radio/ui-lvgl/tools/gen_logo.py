# Renders the RainaCore wordmark in Audiowide with a subtle bevel into
# ui-lvgl/rc_logo.cpp as an RGB565 + alpha LVGL image, 14 px tall.
# Usage: python gen_logo.py path/to/Audiowide-Regular.ttf
import os, sys
from PIL import Image, ImageDraw, ImageFont, ImageChops

HERE = os.path.dirname(os.path.abspath(__file__))
FONT = sys.argv[1]
OUT = os.path.join(HERE, '..', 'rc_logo.cpp')
SS = 8            # drawn large, then shrunk for smooth edges
H = 14
PX = 13           # font size on the screen

font = ImageFont.truetype(FONT, PX * SS)
ascent, descent = font.getmetrics()
w_raina = font.getlength('Raina')
w_core = font.getlength('Core')
W = int((w_raina + w_core) / SS) + 3
big = (W * SS, H * SS)
base_y = (H * SS - (ascent + descent)) // 2 - SS // 4

def mask(text, x):
    m = Image.new('L', big, 0)
    ImageDraw.Draw(m).text((x, base_y), text, fill=255, font=font)
    return m

def bevel(m, face, light, dark):
    # the face, lit along its top-left edges and shaded along its bottom-right
    d = max(2, SS // 3)
    lit = ImageChops.subtract(m, ImageChops.offset(m, d, d))
    shade = ImageChops.subtract(m, ImageChops.offset(m, -d, -d))
    img = Image.new('RGB', big, face)
    img.paste(Image.new('RGB', big, light), (0, 0), lit)
    img.paste(Image.new('RGB', big, dark), (0, 0), shade)
    return img

m1 = mask('Raina', SS)
m2 = mask('Core', SS + w_raina)
rgb = Image.new('RGB', big, (0, 0, 0))
rgb.paste(bevel(m1, (240, 244, 248), (255, 255, 255), (150, 162, 178)), (0, 0), m1)
rgb.paste(bevel(m2, (43, 140, 255), (140, 200, 255), (16, 78, 170)), (0, 0), m2)
alpha = ImageChops.lighter(m1, m2)

small = rgb.resize((W, H), Image.LANCZOS)
sa = alpha.resize((W, H), Image.LANCZOS)

data = bytearray()
for y in range(H):
    for x in range(W):
        r, g, b = small.getpixel((x, y))
        v = (r >> 3) << 11 | (g >> 2) << 5 | b >> 3
        data += bytes((v & 255, v >> 8))
for y in range(H):
    for x in range(W):
        data.append(sa.getpixel((x, y)))

lines = []
for i in range(0, len(data), 20):
    lines.append('  ' + ','.join(str(b) for b in data[i:i + 20]) + ',')
src = f'''// RainaCore wordmark, set in Audiowide (SIL Open Font License) with a subtle
// bevel. Generated image: RGB565 pixels followed by one alpha byte per pixel.
#include <lvgl.h>

static const uint8_t rc_logo_data[] = {{
{chr(10).join(lines)}
}};

extern const lv_image_dsc_t rc_logo;
const lv_image_dsc_t rc_logo = {{
  .header = {{
    .magic = LV_IMAGE_HEADER_MAGIC,
    .cf = LV_COLOR_FORMAT_RGB565A8,
    .flags = 0,
    .w = {W},
    .h = {H},
    .stride = {W * 2},
  }},
  .data_size = sizeof(rc_logo_data),
  .data = rc_logo_data,
}};
'''
open(OUT, 'w', encoding='utf8').write(src)
print('logo', W, 'x', H, len(data), 'bytes')
