"""Cut the 4K recordings into the YouTube video: cuts on bar lines of the music (112 BPM), captions, zoom on the
in-game newspaper and an end card.

Usage: edit.py <dir with the recordings> <out dir> [preview]
Writes body.mp4, end.mp4, sfx.json in <out dir>; then run music.py and mux.sh.
"""
import json, subprocess, sys
import numpy as np
from PIL import Image, ImageDraw, ImageFont, ImageFilter

SRC = sys.argv[1]
OUT = sys.argv[2]
PREVIEW = len(sys.argv) > 3
W, H = 3840, 2160
FPS = 60
BAR = 4 * 60 / 112
SC = 1
BOLD = '/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf'
REG = '/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf'


def f(bar):
    """First video frame of a bar."""
    return round(bar * BAR * FPS)


# (recording, first frame, bars). Bar sections must match music.py.
CLIPS = [
    ('mosty', 34, 3),      # 0: the map, the cab window opens in 2D; 3D switch exactly on bar 3
    ('mosty', 420, 3),     # 3: rail: signals, truss bridge, river, level crossing
    ('kopce', 140, 3),     # 6: town, station
    ('mosty', 1650, 3),    # 9: bridge, tunnel (music goes dark at bar 10)
    ('autobus', 2280, 4),  # 12: road: level crossing, tunnel
    ('autobus', 2880, 3),  # 16: into town
    ('lod', 1790, 3),      # 19: canal, bridge, into the lock
    ('lod', 2430, 1),      # 22: lifted up in the lock
    ('more', 960, 3),      # 23: sea, river mouth waterfall
    ('letadlo', 160, 4),   # 26: take-off and climb over the river
    ('letadlo', 1060, 1),  # 30: touch-down
]
END_START = 2080           # 'end' recording: the newspaper pops up at 2160
END_BARS = 4               # bars 31-35
CARD = 4.5                 # end card after bar 35 (seconds)


def run(cmd):
    print(' '.join(cmd[:6]), '...', flush=True)
    subprocess.run(cmd, check=True)


def body(clips):
    """Cut every clip to its own file (seek, then exact frame count), join them and add the captions."""
    size = '1280:720' if PREVIEW else f'{W}:{H}'
    preset = 'veryfast' if PREVIEW else 'medium'
    bar = 0
    names = []
    for k, (fn, start, bars) in enumerate(clips):
        n = f(bar + bars) - f(bar)
        name = f'{OUT}/clip{k:02d}.mp4'
        run(['ffmpeg', '-loglevel', 'error', '-y', '-i', f'{SRC}/{fn}.mp4', '-vf',
             f'select=gte(n\\,{start}),setpts=N/{FPS}/TB,scale={size}', '-frames:v', str(n), '-r', str(FPS),
             '-c:v', 'libx264', '-preset', preset, '-crf', '12', '-pix_fmt', 'yuv420p', name])
        names.append(name)
        bar += bars
    with open(f'{OUT}/clips.txt', 'w') as fl:
        fl.writelines(f"file '{x}'\n" for x in names)
    run(['ffmpeg', '-loglevel', 'error', '-y', '-f', 'concat', '-safe', '0', '-i', f'{OUT}/clips.txt',
         '-vf', captions(1280 / W if PREVIEW else 1), '-r', str(FPS),
         '-c:v', 'libx264', '-preset', preset, '-crf', '14', '-pix_fmt', 'yuv420p', f'{OUT}/body.mp4'])
    return bar


def text(t0, t1, txt, size, y, font=BOLD, x='(w-text_w)/2', fade=0.35):
    size = round(size * SC)
    alpha = f"if(lt(t,{t0}+{fade}),(t-{t0})/{fade},if(gt(t,{t1}-{fade}),({t1}-t)/{fade},1))"
    txt = txt.replace(':', '\\:').replace("'", "’")
    return (f"drawtext=fontfile={font}:text='{txt}':fontsize={size}:fontcolor=white:x={x}:y={y}"
            f":shadowcolor=black@0.7:shadowx={size // 18}:shadowy={size // 18}"
            f":enable='between(t,{t0},{t1})':alpha='{alpha}'")


def captions(sc=1):
    b = BAR
    global SC
    SC = sc
    items = [
        text(0.4, 2.6 * b, 'OpenTTD', 230, 'h*0.30'),
        text(0.8, 2.6 * b, 'First person view', 130, 'h*0.30+h*0.13', font=REG),
        text(3.25 * b, 5.6 * b, 'By rail', 150, 'h*0.08', x='w*0.05'),
        text(12.25 * b, 14.6 * b, 'By road', 150, 'h*0.08', x='w*0.05'),
        text(19.25 * b, 21.6 * b, 'By water', 150, 'h*0.08', x='w*0.05'),
        text(26.25 * b, 28.6 * b, 'By air', 150, 'h*0.08', x='w*0.05'),
    ]
    return ','.join(items)


def ease(t):
    t = float(t)
    t = min(max(t, 0.0), 1.0)
    return 0.5 - 0.5 * np.cos(np.pi * t)


def end():
    """The newspaper (zoomed in) and the end card, frame by frame with PIL."""
    n_news = f(END_BARS)
    n_card = round(CARD * FPS)
    ow, oh = (1280, 720) if PREVIEW else (W, H)
    dec = subprocess.Popen(['ffmpeg', '-loglevel', 'error', '-i', f'{SRC}/end.mp4', '-vf',
                            f'trim=start_frame={END_START}:end_frame={END_START + n_news},setpts=PTS-STARTPTS',
                            '-f', 'rawvideo', '-pix_fmt', 'rgb24', '-'], stdout=subprocess.PIPE)
    enc = subprocess.Popen(['ffmpeg', '-loglevel', 'error', '-y', '-f', 'rawvideo', '-pix_fmt', 'rgb24',
                            '-s', f'{ow}x{oh}', '-r', str(FPS), '-i', '-', '-c:v', 'libx264',
                            '-preset', 'veryfast' if PREVIEW else 'medium', '-crf', '14', '-pix_fmt', 'yuv420p',
                            f'{OUT}/end.mp4'], stdin=subprocess.PIPE)
    # News window in the recording: 1076,1464 size 1688x648 (slides up from the bottom first).
    zx, zy, zw = 1920 - 1250, 2160 - 1406, 2500
    zh = zw * 9 / 16
    z0 = 80 + 30   # zoom starts when the paper is up
    z1 = z0 + 100
    last = None
    fsz = lambda s: ImageFont.truetype(BOLD, round(s * ow / W))
    fsr = lambda s: ImageFont.truetype(REG, round(s * ow / W))
    for i in range(n_news + n_card):
        if i < n_news:
            raw = dec.stdout.read(W * H * 3)
            img = Image.frombuffer('RGB', (W, H), raw)
            last = img
        else:
            img = last
        e = ease((i - z0) / (z1 - z0))
        box = (zx * e, zy * e, min(W, zx * e + W + (zw - W) * e), min(H, zy * e + H + (zh - H) * e))
        frame = img.resize((ow, oh), Image.LANCZOS, box=box)
        if i >= n_news:
            # End card: blur and darken the last frame, then the titles.
            c = ease((i - n_news) / 30)
            bg = frame.filter(ImageFilter.GaussianBlur(radius=float(40 * ow / W * c)))
            frame = Image.blend(bg, Image.new('RGB', (ow, oh), (10, 14, 24)), 0.8 * c)
            d = ImageDraw.Draw(frame, 'RGBA')
            a = int(round(255 * float(ease((i - n_news - 10) / 30))))
            lines = [('OpenTTD: First person view', fsz(170), 0.24),
                     ('Trains · Buses · Ships · Aircraft', fsr(95), 0.38),
                     ('Alpha coming soon on GitHub', fsz(120), 0.56),
                     ('github.com/vaclavloubek/OpenTTD/releases', fsr(90), 0.66),
                     ('Game sounds: OpenSFX  ·  Music: original', fsr(60), 0.88)]
            for txt, font, y in lines:
                tw = d.textlength(txt, font=font)
                col = (252, 212, 0, a) if 'Alpha' in txt else (255, 255, 255, a)
                d.text((float((ow - tw) / 2), float(y * oh)), txt, font=font, fill=col)
        enc.stdin.write(frame.tobytes())
    enc.stdin.close()
    enc.wait()
    dec.wait()
    return (n_news + n_card) / FPS


if __name__ == '__main__':
    import os
    if not os.environ.get('SKIP_BODY'):
        assert body(CLIPS) == 31
    end()
    b = BAR
    # Game sounds: osfx_NN, NN = the hex number in the SND_ name (see src/sound_type.h).
    sfx = [
        {'sound': 0x15, 't': (240 - 34) / 60, 'gain': 0.5},               # click: cab view button
        {'sound': 0x15, 't': 3 * b - 0.03, 'gain': 0.55},                 # click: 3D
        {'sound': 0x0A, 't': 3 * b + 0.1, 'gain': 0.35},                  # train horn
        {'sound': 0x0E, 't': 5.2 * b, 'gain': 0.18, 'len': 1.5},          # level crossing
        {'sound': 0x05, 't': 10.2 * b, 'gain': 0.25},                     # into the tunnel
        {'sound': 0x1B, 't': 12 * b + 0.05, 'gain': 0.35},                # bus departs
        {'sound': 0x07, 't': 19 * b + 0.05, 'gain': 0.35},                # ferry horn
        {'sound': 0x09, 't': 26 * b + 0.05, 'gain': 0.35},                # jet take-off
        {'sound': 0x16, 't': 31 * b + 80 / 60, 'gain': 0.4},              # news ticker
        {'sound': 0x1D, 't': 31 * b + 1.6, 'gain': 0.18, 'len': 3.0, 'fade': 1.2},  # applause
    ]
    json.dump(sfx, open(f'{OUT}/sfx.json', 'w'), indent=1)
    print('done')
