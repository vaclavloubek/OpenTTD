"""Own background music + OpenSFX game sounds for the YouTube video, cuts on bar lines. numpy only.

Usage: music.py out.wav <OpenSFX src/wav dir> sfx.json"""
import sys, wave, json
import numpy as np

SR = 48000
OUT = sys.argv[1]
SFX = sys.argv[2]  # OpenSFX src/wav dir
BEAT = 60 / 112  # 112 BPM, every cut lands on a bar line
BARS = 35  # the final chord on bar 35 rings out over the end card
DUR = BARS * 4 * BEAT + 4.5
N = int(DUR * SR) + SR
rng = np.random.default_rng(7)
mixL = np.zeros(N)
mixR = np.zeros(N)


def bt(b):
    return b * BEAT


def add(sig, t, gain=1.0, pan=0.0):
    i = int(t * SR)
    if i >= N:
        return
    sig = sig[:N - i]
    l = np.cos((pan + 1) * np.pi / 4) * np.sqrt(2)
    r = np.sin((pan + 1) * np.pi / 4) * np.sqrt(2)
    mixL[i:i + len(sig)] += sig * gain * l
    mixR[i:i + len(sig)] += sig * gain * r


def note_hz(n):
    names = {'C': 0, 'D': 2, 'E': 4, 'F': 5, 'G': 7, 'A': 9, 'B': 11}
    s = names[n[0]]
    k = 1
    if n[1] == '#':
        s += 1; k = 2
    elif n[1] == 'b':
        s -= 1; k = 2
    octave = int(n[k:])
    return 440.0 * 2 ** ((s + 12 * (octave + 1) - 69) / 12)


def env(n, a=0.005, d=0.3, s=0.0, r=0.05, length=None):
    t = np.arange(n) / SR
    e = np.minimum(t / a, 1.0) * (s + (1 - s) * np.exp(-t / d))
    rn = int(r * SR)
    if rn > 0 and n > rn:
        e[-rn:] *= np.linspace(1, 0, rn)
    return e


def lowpass(x, cutoff):
    X = np.fft.rfft(x)
    f = np.fft.rfftfreq(len(x), 1 / SR)
    X *= 1 / np.sqrt(1 + (f / cutoff) ** 4)
    return np.fft.irfft(X, len(x))


def bandpass(x, lo, hi):
    X = np.fft.rfft(x)
    f = np.fft.rfftfreq(len(x), 1 / SR)
    X *= (1 / np.sqrt(1 + (f / hi) ** 4)) * (1 / np.sqrt(1 + (lo / np.maximum(f, 1)) ** 4))
    return np.fft.irfft(X, len(x))


def epiano(hz, dur):
    n = int(dur * SR)
    t = np.arange(n) / SR
    idx = 1.6 * np.exp(-t / 0.25)
    s = np.sin(2 * np.pi * hz * t + idx * np.sin(2 * np.pi * hz * t))
    s += 0.25 * np.sin(2 * np.pi * hz * 2 * t) * np.exp(-t / 0.4)
    return s * env(n, 0.003, 0.9, 0.15, 0.08)


def chord(names, t, dur, gain=0.11, spread=0.012):
    for k, nm in enumerate(names):
        add(epiano(note_hz(nm), dur), t + k * spread, gain, pan=(k - len(names) / 2) * 0.15)


def bass(nm, t, dur, gain=0.32):
    hz = note_hz(nm)
    n = int(dur * SR)
    tt = np.arange(n) / SR
    s = np.sin(2 * np.pi * hz * tt) + 0.35 * np.sin(2 * np.pi * 2 * hz * tt) * np.exp(-tt / 0.12)
    add(s * env(n, 0.004, 0.35, 0.35, 0.04), t, gain)


def kick(t, gain=0.55):
    n = int(0.35 * SR)
    tt = np.arange(n) / SR
    f = 45 + 95 * np.exp(-tt / 0.035)
    ph = 2 * np.pi * np.cumsum(f) / SR
    add(np.sin(ph) * np.exp(-tt / 0.12), t, gain)


def snare(t, gain=0.22):
    n = int(0.25 * SR)
    tt = np.arange(n) / SR
    nz = bandpass(rng.standard_normal(n), 1500, 7000) * np.exp(-tt / 0.06)
    tone = np.sin(2 * np.pi * 185 * tt) * np.exp(-tt / 0.04)
    add(nz * 0.8 + tone * 0.5, t, gain, pan=0.1)


def hat(t, gain=0.06, open_=False):
    n = int((0.25 if open_ else 0.05) * SR)
    tt = np.arange(n) / SR
    nz = bandpass(rng.standard_normal(n), 7000, 16000) * np.exp(-tt / (0.08 if open_ else 0.015))
    add(nz, t, gain, pan=-0.3)


def shaker(t, gain=0.035):
    n = int(0.08 * SR)
    tt = np.arange(n) / SR
    nz = bandpass(rng.standard_normal(n), 5000, 12000) * np.sin(np.pi * tt / tt[-1]) ** 2
    add(nz, t, gain, pan=0.35)


def lead(nm, t, dur, gain=0.07):
    hz = note_hz(nm)
    n = int(dur * SR)
    tt = np.arange(n) / SR
    vib = 1 + 0.004 * np.sin(2 * np.pi * 5.5 * tt) * np.minimum(tt / 0.2, 1)
    ph = 2 * np.pi * np.cumsum(hz * vib) / SR
    sq = np.sign(np.sin(ph)) * 0.5 + 0.5 * np.sin(ph)
    s = lowpass(sq, 2600)
    add(s * env(n, 0.01, 0.5, 0.55, 0.06), t, gain, pan=0.2)


def cymbal(t, gain=0.08, length=2.5):
    n = int(length * SR)
    tt = np.arange(n) / SR
    nz = bandpass(rng.standard_normal(n), 4000, 15000) * np.exp(-tt / 0.9)
    add(nz, t, gain, pan=-0.2)


def riser(t0, t1, gain=0.10):
    n = int((t1 - t0) * SR)
    tt = np.linspace(0, 1, n)
    nz = rng.standard_normal(n)
    out = np.zeros(n)
    seg = SR // 20
    for i in range(0, n, seg):
        c = 800 + 9000 * tt[i] ** 2
        out[i:i + seg] = bandpass(nz[i:i + seg], c * 0.5, c * 1.5)
    add(out * tt ** 2, t0, gain)


def blip(nm, t, gain=0.12, dur=0.18):
    hz = note_hz(nm)
    n = int(dur * SR)
    tt = np.arange(n) / SR
    add(np.sin(2 * np.pi * hz * tt) * np.exp(-tt / 0.06), t, gain)


def bar(b):
    return bt(4 * b)


def pad(names, t, dur, gain=0.05):
    for nm in names:
        n = int(dur * SR)
        tt = np.arange(n) / SR
        s = np.sin(2 * np.pi * note_hz(nm) * tt) + 0.3 * np.sin(4 * np.pi * note_hz(nm) * tt)
        e = np.minimum(tt / 0.5, 1) * np.minimum((dur - tt) / 0.6, 1).clip(0, 1)
        add(s * e, t, gain, pan=rng.uniform(-0.4, 0.4))


def groove(b0, nbars, prog, drums='full', bass_pat='walk', comp=True):
    for k in range(nbars):
        b = b0 + k
        ch, r, f = prog[k % len(prog)]
        if comp:
            chord(ch, bar(b), BEAT * 1.4, 0.085)
            chord(ch, bar(b) + bt(1.5), BEAT * 0.9, 0.065)
            chord(ch, bar(b) + bt(3), BEAT * 0.8, 0.065)
        if bass_pat == 'walk':
            for off, nm in [(0, r), (1, f), (1.5, r), (2, f), (3, r), (3.5, f)]:
                bass(nm, bar(b) + bt(off), BEAT * 0.45, 0.3)
        elif bass_pat == 'bounce':
            for off, nm in [(0, r), (0.75, r), (1.5, f), (2, r), (2.75, r), (3.5, f)]:
                bass(nm, bar(b) + bt(off), BEAT * 0.4, 0.3)
        elif bass_pat == 'long':
            bass(r, bar(b), BEAT * 3.6, 0.26)
        if drums == 'full':
            for q in range(4):
                kick(bar(b) + bt(q), 0.5 if q % 2 == 0 else 0.35)
            snare(bar(b) + bt(1)); snare(bar(b) + bt(3))
            for q in range(8):
                hat(bar(b) + bt(q * 0.5), 0.05 if q % 2 else 0.065, open_=(q == 7))
        elif drums == 'half':
            kick(bar(b), 0.45); kick(bar(b) + bt(2.5), 0.3)
            snare(bar(b) + bt(2), 0.16)
            for q in range(8):
                shaker(bar(b) + bt(q * 0.5), 0.03 if q % 2 else 0.045)
        elif drums == 'kick':
            kick(bar(b), 0.45); kick(bar(b) + bt(2), 0.35)


def melody(b0, notes, gain=0.065, octave=0):
    for b, nm, ln in notes:
        if octave:
            nm = nm[:-1] + str(int(nm[-1]) + octave)
        lead(nm, bar(b0) + bt(b), BEAT * ln * 0.95, gain)


def roll(t0, t1, g0=0.08, g1=0.18):
    n = int(round((t1 - t0) / (BEAT / 4)))
    for k in range(n):
        snare(t0 + k * BEAT / 4, g0 + (g1 - g0) * k / max(1, n - 1))


PROG_A = [(['C3', 'E3', 'G3', 'B3'], 'C2', 'G2'), (['A2', 'C3', 'E3', 'G3'], 'A1', 'E2'),
          (['D3', 'F3', 'A3', 'C4'], 'D2', 'A2'), (['G2', 'B2', 'D3', 'F3'], 'G1', 'D2')]
PROG_B = [(['F3', 'A3', 'C4', 'E4'], 'F2', 'C3'), (['G3', 'B3', 'D4', 'F4'], 'G2', 'D3'),
          (['E3', 'G3', 'B3', 'D4'], 'E2', 'B2'), (['A2', 'C3', 'E3', 'G3'], 'A1', 'E2')]
PROG_W = [(['F3', 'A3', 'C4', 'E4'], 'F2', 'C3'), (['E3', 'G3', 'C4', 'D4'], 'E2', 'B2'),
          (['D3', 'F3', 'A3', 'C4'], 'D2', 'A2'), (['Bb2', 'D3', 'F3', 'A3'], 'Bb1', 'F2')]
MEL_A = [(0, 'E5', 1.0), (1, 'G5', 0.5), (1.5, 'A5', 0.5), (2, 'B5', 1.0), (3, 'A5', 0.5), (3.5, 'G5', 0.5),
         (4, 'E5', 1.0), (5, 'C5', 0.5), (5.5, 'D5', 0.5), (6, 'E5', 1.5), (7.5, 'G5', 0.5),
         (8, 'A5', 1.0), (9, 'C6', 0.5), (9.5, 'B5', 0.5), (10, 'A5', 1.0), (11, 'F5', 1.0),
         (12, 'G5', 1.0), (13, 'B5', 0.5), (13.5, 'D6', 0.5), (14, 'C6', 0.75), (14.75, 'B5', 0.25), (15, 'G5', 1.0)]
MEL_B = [(0, 'C5', 0.5), (0.5, 'F5', 0.5), (1, 'A5', 1.0), (2.5, 'G5', 0.5), (3, 'F5', 1.0),
         (4, 'D5', 0.5), (4.5, 'G5', 0.5), (5, 'B5', 1.0), (6.5, 'A5', 0.5), (7, 'G5', 1.0),
         (8, 'B4', 0.5), (8.5, 'E5', 0.5), (9, 'G5', 1.0), (10, 'B5', 0.5), (10.5, 'D6', 0.5), (11, 'B5', 1.0),
         (12, 'C6', 1.0), (13, 'A5', 0.5), (13.5, 'G5', 0.5), (14, 'E5', 1.0), (15, 'A5', 1.0)]

# --- Bars 0-3: the map and the 2D cab window. Curious, light.
for b, ch, bs in [(0, ['F3', 'A3', 'C4', 'E4'], 'F2'), (1, ['E3', 'G3', 'B3', 'D4'], 'E2'), (2, ['D3', 'F3', 'A3', 'C4'], 'D2')]:
    chord(ch, bar(b), BEAT * 1.8, 0.10)
    chord(ch, bar(b) + bt(2.5), BEAT * 1.4, 0.07)
    bass(bs, bar(b), BEAT * 1.6, 0.22)
    bass(bs, bar(b) + bt(3), BEAT * 0.8, 0.16)
for k in range(24):
    shaker(bt(k * 0.5), 0.03 if k % 2 else 0.045)
for k, nm in enumerate(['E5', 'G5', 'A5']):
    blip(nm, bar(1) + bt(2 + k * 0.25), 0.05)
riser(bar(2), bar(3), 0.09)
roll(bar(2) + bt(2), bar(3))

# Section starts in bars (must match the cut list in edit.py).
RAIL, TUNNEL, ROAD, WATER, AIR, END, BARS_END = 3, 12, 12, 19, 26, 31, 35

# --- Rail: full groove from the 3D switch (TUNNEL == ROAD: no dark tunnel part in this cut).
cymbal(bar(RAIL), 0.1)
groove(RAIL, TUNNEL - RAIL, PROG_A)
melody(RAIL, MEL_A)
melody(RAIL + 4, MEL_A, 0.055)
dark = [(['A2', 'C3', 'E3', 'G3'], 'A1', 'E2'), (['E2', 'G#2', 'B2', 'D3'], 'E1', 'B1')]
if TUNNEL < ROAD:
    groove(TUNNEL, ROAD - TUNNEL, dark, drums='kick', bass_pat='long', comp=False)
    for k, (ch, r, f) in enumerate(dark[:ROAD - TUNNEL]):
        pad(ch, bar(TUNNEL + k), BEAT * 4, 0.045)
roll(bar(ROAD - 1) + bt(2), bar(ROAD), 0.06, 0.16)

# --- Road: bouncier bass, new tune.
cymbal(bar(ROAD), 0.09)
groove(ROAD, WATER - ROAD, PROG_B, bass_pat='bounce')
melody(ROAD, MEL_B)
melody(ROAD + 4, MEL_B[:9], 0.06)
roll(bar(WATER - 1) + bt(3), bar(WATER), 0.08, 0.14)

# --- Water: half time, flowing arpeggios.
cymbal(bar(WATER), 0.07)
groove(WATER, AIR - WATER, PROG_W, drums='half', bass_pat='long', comp=False)
for k in range(AIR - WATER):
    ch = PROG_W[k % 4][0]
    up = ch + [n[:-1] + str(int(n[-1]) + 1) for n in ch[1:3]]
    for q in range(8):
        nm = up[[0, 1, 2, 3, 4, 3, 2, 1][q]]
        add(epiano(note_hz(nm), BEAT * 0.9), bar(WATER + k) + bt(q * 0.5), 0.07, pan=-0.5 + q / 8)
    pad(ch, bar(WATER + k), BEAT * 4, 0.03)
melody(WATER + 1, [(0, 'A5', 2.0), (2, 'G5', 1.0), (3, 'E5', 1.0), (4, 'F5', 3.0), (8, 'E5', 2.0), (10, 'D5', 1.0), (11, 'C5', 1.0), (12, 'D5', 3.0)], 0.05)
riser(bar(AIR - 1.5), bar(AIR), 0.11)
roll(bar(AIR - 1), bar(AIR), 0.06, 0.2)

# --- Air: lift off, high pads.
cymbal(bar(AIR), 0.12, 3.5)
groove(AIR, END - AIR, PROG_A)
melody(AIR, MEL_A, 0.06)
for k in range(END - AIR):
    ch = PROG_A[k % 4][0]
    pad([n[:-1] + str(int(n[-1]) + 1) for n in ch], bar(AIR + k), BEAT * 4, 0.025)
roll(bar(END - 1) + bt(2), bar(END), 0.1, 0.22)

# --- The newspaper and the end card.
kick(bar(END), 0.55)
cymbal(bar(END), 0.1, 3.0)
for k, (ch, bs) in enumerate([(['F3', 'A3', 'C4', 'E4'], 'F2'), (['G3', 'B3', 'D4', 'F4'], 'G2'),
                              (['E3', 'G3', 'B3', 'D4'], 'E2'), (['A2', 'C3', 'E3', 'G3'], 'A1')]):
    b = END + k
    chord(ch, bar(b), BEAT * 1.8, 0.09)
    chord(ch, bar(b) + bt(2.5), BEAT * 1.2, 0.06)
    bass(bs, bar(b), BEAT * 2.5, 0.22)
    pad(ch, bar(b), BEAT * 4, 0.035)
    for q in range(8):
        shaker(bar(b) + bt(q * 0.5), 0.03 if q % 2 else 0.04)
melody(END, [(0, 'E5', 1.5), (1.5, 'G5', 0.5), (2, 'A5', 2.0), (4, 'B5', 1.5), (5.5, 'A5', 0.5), (6, 'G5', 2.0),
             (8, 'G5', 1.5), (9.5, 'B5', 0.5), (10, 'D6', 2.0), (12, 'C6', 2.0), (14, 'B5', 2.0)], 0.05)
kick(bar(BARS_END), 0.55)
cymbal(bar(BARS_END), 0.11, 4.0)
chord(['C3', 'G3', 'B3', 'D4', 'E4'], bar(BARS_END), BEAT * 6, 0.12, spread=0.03)
bass('C2', bar(BARS_END), BEAT * 6, 0.3)
lead('C6', bar(BARS_END), BEAT * 4, 0.05)
for k, nm in enumerate(['G5', 'C6', 'E6', 'G6']):
    blip(nm, bar(BARS_END) + bt(1 + k * 0.5), 0.05, 0.3)

music = np.stack([mixL, mixR])
music = np.tanh(music * 1.2) / 1.2


# --- Game sounds (OpenSFX). osfx_NN is the sound with that (hex) number in the SND_ names.
def load(i):
    w = wave.open(f'{SFX}/osfx_{i:02d}.wav')
    sr = w.getframerate()
    d = np.frombuffer(w.readframes(w.getnframes()), np.int16 if w.getsampwidth() == 2 else np.uint8).astype(float)
    d = d / 32768 if w.getsampwidth() == 2 else (d - 128) / 128
    if w.getnchannels() == 2:
        d = d.reshape(-1, 2).mean(1)
    x = np.arange(int(len(d) * SR / sr)) * sr / SR
    return np.interp(x, np.arange(len(d)), d)


sfx = np.zeros((2, N))
for ev in json.load(open(sys.argv[3])):
    sig = load(ev['sound'])
    if 'len' in ev:
        sig = sig[:int(ev['len'] * SR)].copy()
    i = int(ev['t'] * SR)
    sig = sig[:N - i].copy()
    k = min(len(sig), int(ev.get('fade', 0.05) * SR))
    sig[-k:] *= np.linspace(1, 0, k)
    sfx[:, i:i + len(sig)] += sig * ev['gain']

mix = music + sfx
mix[:, :int(0.05 * SR)] *= np.linspace(0, 1, int(0.05 * SR))
end = int(DUR * SR)
mix = mix[:, :end]
mix[:, -int(1.5 * SR):] *= np.linspace(1, 0, int(1.5 * SR))
mix /= np.abs(mix).max() / 0.9
pcm = (mix.T * 32767).astype(np.int16)
w = wave.open(OUT, 'wb')
w.setnchannels(2); w.setsampwidth(2); w.setframerate(SR)
w.writeframes(pcm.tobytes())
w.close()
print('written', OUT, end / SR)
