"""Own background music + OpenSFX game sounds for the teaser, synced to the cut. numpy only."""
import sys, wave
import numpy as np

SR = 48000
OUT = sys.argv[1]
SFX = sys.argv[2]  # OpenSFX src/wav dir
BEAT = 8.9167 / 16  # the 3D switch lands on beat 16
DUR = 1204 / 60
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


# --- Bars 1-2 (beats 0-8): curious intro: e-piano, shaker, soft bass.
for b, ch, bs in [(0, ['F3', 'A3', 'C4', 'E4'], 'F2'), (4, ['E3', 'G3', 'B3', 'D4'], 'E2')]:
    chord(ch, bt(b), BEAT * 1.8, 0.10)
    chord(ch, bt(b + 2.5), BEAT * 1.4, 0.07)
    bass(bs, bt(b), BEAT * 1.6, 0.22)
    bass(bs, bt(b + 3), BEAT * 0.8, 0.16)
for k in range(16):
    shaker(bt(k * 0.5), 0.03 if k % 2 else 0.045)
# little melodic pickup as the window opens (beat ~2.3)
for k, nm in enumerate(['E5', 'G5', 'A5']):
    blip(nm, bt(2.5 + k * 0.25), 0.06)

# --- Bar 3 start (beat 8) Dm7, then freeze at beat 9: drop everything, "question" motif.
chord(['D3', 'F3', 'A3', 'C4'], bt(8), BEAT * 1.0, 0.10)
bass('D2', bt(8), BEAT * 0.9, 0.2)
shaker(bt(8)); shaker(bt(8.5))
# suspended pad under the freeze (beats 9-12)
for nm in ['G3', 'C4', 'D4', 'F4']:
    n = int(BEAT * 3 * SR)
    tt = np.arange(n) / SR
    s = epiano(note_hz(nm), BEAT * 3) * 0.4 + 0.6 * np.sin(2 * np.pi * note_hz(nm) * tt) * np.minimum(tt / 0.4, 1) * np.exp(-tt / 2.5)
    add(s, bt(9), 0.07)
# questioning two-note rise + answer-less third
lead('E5', bt(9.0), BEAT * 0.45, 0.06)
lead('A5', bt(10.0), BEAT * 0.9, 0.07)      # with the "?" pop
lead('B5', bt(11.0), BEAT * 0.8, 0.05)

# --- Bar 4 (beats 12-16): cab opens, groove starts building.
for b, ch, bs in [(12, ['D3', 'F3', 'A3', 'C4'], 'D2'), (14, ['G3', 'B3', 'D4', 'F4'], 'G2')]:
    chord(ch, bt(b), BEAT * 1.6, 0.10)
    bass(bs, bt(b), BEAT * 0.9, 0.26)
    bass(bs, bt(b + 1), BEAT * 0.4, 0.2)
    bass(bs, bt(b + 1.5), BEAT * 0.4, 0.2)
for k in range(8):
    hat(bt(12 + k * 0.5), 0.05)
kick(bt(12)); kick(bt(14))
for k, b in enumerate(np.arange(14.5, 16, 0.25)):
    snare(bt(b), 0.08 + 0.025 * k)
riser(bt(13), bt(16), 0.09)

# --- Bars 5-8 (beats 16-32): 3D! full groove.
prog = [(['C3', 'E3', 'G3', 'B3'], 'C2', 'G2'), (['A2', 'C3', 'E3', 'G3'], 'A1', 'E2'),
        (['D3', 'F3', 'A3', 'C4'], 'D2', 'A2'), (['G2', 'B2', 'D3', 'F3'], 'G1', 'D2')]
cymbal(bt(16), 0.09)
for bar, (ch, r, f) in enumerate(prog):
    b0 = 16 + bar * 4
    chord(ch, bt(b0), BEAT * 1.4, 0.09)
    chord(ch, bt(b0 + 1.5), BEAT * 0.9, 0.07)
    chord(ch, bt(b0 + 3), BEAT * 0.8, 0.07)
    for k, (nm, ln) in enumerate([(r, 1), (r, 0.5), (f, 0.5), (r, 1), (f, 0.5), (r, 0.5)]):
        pass
    walk = [(0, r), (1, f), (1.5, r), (2, f), (3, r), (3.5, f)]
    for off, nm in walk:
        bass(nm, bt(b0 + off), BEAT * 0.45, 0.3)
    for k in range(4):
        kick(bt(b0 + k), 0.5 if k % 2 == 0 else 0.35)
    snare(bt(b0 + 1)); snare(bt(b0 + 3))
    for k in range(8):
        hat(bt(b0 + k * 0.5), 0.05 if k % 2 else 0.065, open_=(k == 7))
# melody (bright, a bit jazzy, like the game's soundtrack)
mel = [(16, 'E5', 1.0), (17, 'G5', 0.5), (17.5, 'A5', 0.5), (18, 'B5', 1.0), (19, 'A5', 0.5), (19.5, 'G5', 0.5),
       (20, 'E5', 1.0), (21, 'C5', 0.5), (21.5, 'D5', 0.5), (22, 'E5', 1.5), (23.5, 'G5', 0.5),
       (24, 'A5', 1.0), (25, 'C6', 0.5), (25.5, 'B5', 0.5), (26, 'A5', 1.0), (27, 'F5', 1.0),
       (28, 'G5', 1.0), (29, 'B5', 0.5), (29.5, 'D6', 0.5), (30, 'C6', 0.75), (30.75, 'B5', 0.25), (31, 'G5', 1.0)]
for b, nm, ln in mel:
    lead(nm, bt(b), BEAT * ln * 0.95, 0.065)
# fill into the newspaper ending
for k, b in enumerate(np.arange(31, 32, 0.25)):
    snare(bt(b), 0.1 + 0.03 * k)

# --- Beat 32: final chord, ring out to the end.
kick(bt(32), 0.55)
cymbal(bt(32), 0.11, 3.5)
chord(['C3', 'G3', 'B3', 'D4', 'E4'], bt(32), BEAT * 4.2, 0.12, spread=0.03)
bass('C2', bt(32), BEAT * 4, 0.3)
lead('C6', bt(32), BEAT * 3.6, 0.05)
for k, nm in enumerate(['G5', 'C6', 'E6', 'G6']):
    blip(nm, bt(33 + k * 0.5), 0.05, 0.3)

music = np.stack([mixL, mixR])
# gentle bus compression-ish: soft clip
music = np.tanh(music * 1.2) / 1.2


# --- Game sounds (OpenSFX), at the exact moments in the video.
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


def sadd(sig, t, gain, fade_out=None):
    i = int(t * SR)
    sig = sig[:N - i].copy()
    if fade_out:
        k = min(len(sig), int(fade_out * SR))
        sig[-k:] *= np.linspace(1, 0, k)
    sfx[:, i:i + len(sig)] += sig * gain


click = load(21)
for t in (78 / 60, 405 / 60, 535 / 60):  # train, cab button, 3D button
    sadd(click, t, 0.5)
sadd(load(10), 545 / 60, 0.45)            # diesel/electric horn as the 3D view rolls into the tunnel
sadd(load(10), 30 / 60, 0.18)             # distant horn as the train leaves the station at the start
pop = load(47)                            # "plop" for the question mark
sadd(pop, 331 / 60, 0.35)
app = load(29)                            # applause: the newspaper (like "citizens celebrate")
sadd(app[:int(3.0 * SR)], 900 / 60, 0.22, fade_out=1.2)

mix = music + sfx
# fade in / out
mix[:, :int(0.05 * SR)] *= np.linspace(0, 1, int(0.05 * SR))
end = int(DUR * SR)
mix = mix[:, :end]
mix[:, -int(0.6 * SR):] *= np.linspace(1, 0, int(0.6 * SR))
mix /= np.abs(mix).max() / 0.9
pcm = (mix.T * 32767).astype(np.int16)
w = wave.open(OUT, 'wb')
w.setnchannels(2); w.setsampwidth(2); w.setframerate(SR)
w.writeframes(pcm.tobytes())
w.close()
print('written', OUT, end / SR)
