"""Cut the 4K vertical recording into a 1080x1920 teaser with camera zooms, a '?' and a freeze."""
import subprocess, sys, math
import numpy as np
from PIL import Image

S = sys.argv[1]
OUT = sys.argv[2]
PREVIEW = len(sys.argv) > 3
SW, SH = 2160, 3840
OW, OH = 1080, 1920

# "?" glyph from the game's large font (news window run).
q = np.asarray(Image.open(f'{S}/q.png').convert('RGB')).astype(int)
qx0, qy0 = 200 + 830, 3840 - 700 + 270
reg = q[qy0:qy0 + 100, qx0:qx0 + 100]
mask = (reg.max(axis=2) < 80)
ys, xs = np.where(mask)
glyph = mask[ys.min():ys.max() + 1, xs.min():xs.max() + 1]


def make_q(height):
    s = height / glyph.shape[0]
    g = Image.fromarray((glyph * 255).astype(np.uint8)).resize((round(glyph.shape[1] * s), height), Image.NEAREST)
    g = np.asarray(g) > 127
    pad = max(4, height // 14)
    h, w = g.shape
    img = np.zeros((h + 2 * pad, w + 2 * pad, 4), np.uint8)
    # black outline + drop shadow, then yellow glyph (OpenTTD's yellow).
    for dy in range(-pad // 2, pad // 2 + 1):
        for dx in range(-pad // 2, pad // 2 + 1):
            img[pad + dy:pad + dy + h, pad + dx:pad + dx + w][g] = (0, 0, 0, 255)
    img[pad + pad // 2:pad + pad // 2 + h, pad + pad // 2:pad + pad // 2 + w][g & ~(img[pad + pad // 2:pad + pad // 2 + h, pad + pad // 2:pad + pad // 2 + w, 3] > 0)] = (0, 0, 0, 160)
    img[pad:pad + h, pad:pad + w][g] = (252, 212, 0, 255)
    return Image.fromarray(img, 'RGBA')


Q = make_q(300)

# Output timeline: (raw frame, overlay '?' scale or 0).
FREEZE_AT = 300
FREEZE_LEN = 100
END_RAW = 935
HOLD = 120
timeline = []
for r in range(0, FREEZE_AT + 1):
    timeline.append((r, None))
for k in range(FREEZE_LEN):
    timeline.append((FREEZE_AT, k))
for r in range(FREEZE_AT + 1, END_RAW + 1):
    timeline.append((r, None))
for k in range(HOLD):
    timeline.append((END_RAW, None))

# Camera keys on output frame index: (t, cx, cy, cw)
F0 = FREEZE_AT + 1  # first freeze frame index in output
A = F0 + FREEZE_LEN  # first output frame after freeze; raw = FREEZE_AT + 1 + (t - A)


def out_t(raw):
    return raw if raw <= FREEZE_AT else raw + FREEZE_LEN


BTN = (960, 548)
keys = [
    (0, 1080, 1920, 1500),
    (out_t(80), 1080, 1920, 1500),
    (out_t(140), 600, 1000, 1150),
    (out_t(250), 600, 1000, 1150),
    (F0, 860, 640, 800),
    (F0 + 28, 820, 600, 520),
    (A - 4, 820, 600, 520),
    (out_t(345), 1080, 1920, 2160),
    (out_t(880), 1080, 1920, 2160),
    (out_t(930), 1080, 2300, 1760),
]


def ease(x):
    return 0.5 - 0.5 * math.cos(math.pi * min(max(x, 0), 1))


def camera(t):
    for (t0, x0, y0, w0), (t1, x1, y1, w1) in zip(keys, keys[1:]):
        if t0 <= t < t1:
            e = ease((t - t0) / (t1 - t0))
            # zoom in log space so it feels even
            w = math.exp(math.log(w0) + (math.log(w1) - math.log(w0)) * e)
            return x0 + (x1 - x0) * e, y0 + (y1 - y0) * e, w
    return keys[-1][1:]


def crop_box(cx, cy, cw):
    ch = cw * OH / OW
    x = min(max(cx - cw / 2, 0), SW - cw)
    y = min(max(cy - ch / 2, 0), SH - ch)
    return x, y, x + cw, y + ch


dec = subprocess.Popen(['ffmpeg', '-loglevel', 'error', '-i', f'{S}/raw4k.mp4', '-f', 'rawvideo', '-pix_fmt', 'rgb24', '-'], stdout=subprocess.PIPE)
enc_cmd = ['ffmpeg', '-loglevel', 'error', '-y', '-f', 'rawvideo', '-pix_fmt', 'rgb24', '-s', f'{OW}x{OH}', '-r', '60', '-i', '-',
           '-f', 'lavfi', '-i', 'anullsrc=r=48000:cl=stereo', '-shortest',
           '-c:v', 'libx264', '-preset', 'slow', '-crf', '16', '-pix_fmt', 'yuv420p', '-profile:v', 'high', '-movflags', '+faststart',
           '-c:a', 'aac', '-b:a', '128k', OUT]
if PREVIEW:
    enc_cmd = ['ffmpeg', '-loglevel', 'error', '-y', '-f', 'rawvideo', '-pix_fmt', 'rgb24', '-s', f'{OW}x{OH}', '-r', '60', '-i', '-',
               '-vf', 'select=not(mod(n\\,30)),scale=270:480', '-vsync', '0', OUT]
enc = subprocess.Popen(enc_cmd, stdin=subprocess.PIPE)

fsize = SW * SH * 3
cur_raw = -1
frame = None
for t, (r, qk) in enumerate(timeline):
    while cur_raw < r:
        buf = dec.stdout.read(fsize)
        frame = Image.frombuffer('RGB', (SW, SH), buf)
        cur_raw += 1
    box = crop_box(*camera(t))
    img = frame.resize((OW, OH), Image.LANCZOS, box=box)
    if qk is not None and qk >= 30:
        k = qk - 30
        pop = min(1.0, k / 10)
        s = pop * (1 + 0.25 * math.sin(pop * math.pi))  # small overshoot
        s *= 1 + 0.03 * math.sin(k * 0.25)
        if s > 0.02:
            qi = Q.resize((max(1, round(Q.width * s)), max(1, round(Q.height * s))), Image.NEAREST)
            # place left-above the button
            bx0, by0, bx1, by1 = box
            sc = OW / (bx1 - bx0)
            px = (BTN[0] - bx0) * sc - 230
            py = (BTN[1] - by0) * sc - 330
            img.paste(qi, (round(px - qi.width / 2), round(py - qi.height / 2)), qi)
    enc.stdin.write(img.tobytes())
enc.stdin.close()
enc.wait()
dec.kill()
print('frames', len(timeline), 'seconds', len(timeline) / 60)
