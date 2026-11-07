import os, subprocess, shutil, glob, re
from concurrent.futures import ThreadPoolExecutor
from PIL import Image
T = os.path.dirname(os.path.abspath(__file__)); os.chdir(T)
R = os.path.abspath(os.path.join(T, '..', '..'))
L = os.path.join(R, '.pio', 'libdeps', 'RainaCore_L2_1V8_touch', 'lvgl')  # run a firmware build first
U = os.path.join(R, 'examples', 'companion_radio', 'ui-lvgl')
DEF = ['-DMESHRAIN_PC_TEST', '-DLV_CONF_SKIP=1', '-DLV_COLOR_DEPTH=16', '-DLV_FONT_MONTSERRAT_12=1', '-DLV_FONT_MONTSERRAT_14=1', '-DLV_FONT_MONTSERRAT_16=1', '-DLV_FONT_MONTSERRAT_28=1',
       '-DLV_USE_IMGFONT=1', '-DLV_USE_LODEPNG=1', '-DLV_LVGL_H_INCLUDE_SIMPLE', '-DLV_MEM_SIZE=6291456', '-DLV_CACHE_DEF_SIZE=2097152', '-DLV_USE_LOG=0', '-DLV_USE_ASSERT_MALLOC=0']
INC = ['-Istub', '-I.', '-I' + L, '-I' + os.path.dirname(L)]
SKIP = re.compile(r'nxp|renesas|vg_lite|thorvg|[\\/]drivers[\\/]|dma2d|helium|neon|arm2d|opengl|sdl|stm32|ambiq|[\\/]test')
os.makedirs('obj', exist_ok=True)
files = [f for f in glob.glob(L + r'\src\**\*.c', recursive=True) if not SKIP.search(f[len(L):])]
def cc(f):
    o = os.path.join('obj', re.sub(r'[\\/:]', '_', f[len(L):]) + '.o')
    if not os.path.exists(o):
        r = subprocess.run(['gcc', '-c', '-O1', '-w'] + DEF + INC + [f, '-o', o], capture_output=True, text=True)
        if r.returncode: print(f, r.stderr[:400])
    return o
with ThreadPoolExecutor(8) as ex:
    objs = [o for o in ex.map(cc, files) if os.path.exists(o)]
s = open(U + r'\MapView.cpp', encoding='utf8').read()
s = s.replace('"../MyMesh.h"', '"MyMesh.h"').replace('"/sdcard/', '"sd/')
s += '''
void benchState(int* z, double* fx, double* fy) { *z = map_z; *fx = center_fx; *fy = center_fy; }
int benchClusters(int* total) { int n = 0, t = 0; for (int i = 0; i < MAX_MAP_NODES; i++) if (!lv_obj_has_flag(node_marks[i], LV_OBJ_FLAG_HIDDEN)) { n++; t += clusters[i].count; } *total = t; return n; }
int benchFindCircle(int* x, int* y) { int b = 0; for (int i = 0; i < MAX_MAP_NODES; i++) if (!lv_obj_has_flag(node_marks[i], LV_OBJ_FLAG_HIDDEN) && clusters[i].count > b) { lv_area_t a; lv_obj_get_coords(node_marks[i], &a); int cx = (a.x1 + a.x2) / 2, cy = (a.y1 + a.y2) / 2; if (cx < 46 || cy < 22 || cx >= 320 || cy >= 240) continue; b = clusters[i].count; *x = cx; *y = cy; } return b; }
'''
open('MapView_b.cpp', 'w', encoding='utf8').write(s)
shutil.copy(U + r'\MapView.h', 'MapView.h'); shutil.copy(U + chr(92) + 'UIPlaces.h', 'UIPlaces.h')
shutil.copy(U + chr(92) + 'UIRepeaterList.cpp', 'UIRepeaterList_b.cpp')
app = []
for src in ['main.cpp', 'MapView_b.cpp', 'UIRepeaterList_b.cpp'] + [U + '\\' + n for n in ['UIPlaces.cpp', 'rc_logo.cpp', 'fa_icons_14.cpp', 'rc_emoji.cpp', 'rc_font_12.cpp', 'rc_font_14.cpp', 'rc_font_16.cpp', 'rc_font_28.cpp']]:
    o = 'obj/app_' + os.path.basename(src) + '.o'
    if os.path.basename(src).startswith(('rc_f', 'rc_e', 'fa_')) and os.path.exists(o):
        app.append(o); continue
    r = subprocess.run(['g++', '-c', '-O1', '-w', '-std=gnu++17', '-fpermissive'] + DEF + INC + [src, '-o', o], capture_output=True, text=True)
    if r.returncode: print(src, r.stderr[:2500]); raise SystemExit(1)
    app.append(o)
r = subprocess.run(['g++', '-o', 'mapbench.exe'] + app + objs + ['-static'], capture_output=True, text=True)
if r.returncode: print(r.stderr[:2500]); raise SystemExit(1)
for f in glob.glob('*.rgb') + glob.glob('s*.png'): os.remove(f)
r = subprocess.run([os.path.join(T, 'mapbench.exe')], capture_output=True, text=True, encoding='utf8', errors='replace', timeout=170)
print(r.stdout[-3000:]); print(r.stderr[:500]); print('exit', r.returncode)
rgb = sorted(glob.glob('*.rgb'), key=lambda n: int(re.search(r's(\d+)', n).group(1)))
ims = []
for f in rgb:
    d = open(f, 'rb').read(); px = bytearray()
    for i in range(0, len(d), 2):
        v = d[i] | d[i + 1] << 8
        r5, g6, b5 = v >> 11, (v >> 5) & 63, v & 31
        px += bytes([r5 << 3 | r5 >> 2, g6 << 2 | g6 >> 4, b5 << 3 | b5 >> 2])
    im = Image.frombytes('RGB', (320, 240), bytes(px))
    im.resize((640, 480), Image.NEAREST).save(f[:-4] + '.png')
    ims.append(im)
cols = 3
sheet = Image.new('RGB', (cols * 648, ((len(ims) + cols - 1) // cols) * 488), (90, 0, 90))
for i, im in enumerate(ims):
    sheet.paste(im.resize((640, 480), Image.NEAREST), ((i % cols) * 648 + 4, (i // cols) * 488 + 4))
sheet.save('sheet.png')
print(len(ims), 'screens')
