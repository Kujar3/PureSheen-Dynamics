"""Synthesizes a ~30 s 44.1 kHz stereo test mix (kick, snare, hats, bass, chords/pads, a vocal-like formant voice)
with a song structure (intro, verse, chorus, breakdown, final chorus). Deterministic (fixed seed). Writes 24-bit PCM.
Usage: python tools/audio/make_test_mix.py test_audio/int_mix.wav  (test_audio/ is gitignored)"""
import sys
import wave
import numpy as np

FS = 44100
DUR = 30.0
BPM = 124.0
N = int(FS * DUR)
t = np.arange(N) / FS
beat = 60.0 / BPM
rng = np.random.default_rng(1234)

def env_section(times_levels):
    """Piecewise-linear gain over time: list of (time, level)."""
    xs = [a for a, _ in times_levels]
    ys = [b for _, b in times_levels]
    return np.interp(t, xs, ys)

def onepole_lp(x, fc):
    a = np.exp(-2 * np.pi * fc / FS)
    y = np.empty_like(x)
    s = 0.0
    for i in range(len(x)):  # fine for a few signals
        s = (1 - a) * x[i] + a * s
        y[i] = s
    return y

def fft_filter(x, gain_fn):
    X = np.fft.rfft(x)
    f = np.fft.rfftfreq(len(x), 1 / FS)
    return np.fft.irfft(X * gain_fn(f), len(x))

# --- structure gains -------------------------------------------------------------------------------------------
# intro 0-4, verse 4-12, chorus 12-20, breakdown 20-24, final chorus 24-30
drums_on = env_section([(0, 0), (3.9, 0), (4.0, 1), (19.9, 1), (20.0, 0), (23.9, 0), (24.0, 1), (29.5, 1), (30, 0)])
chorus = env_section([(0, 0), (11.9, 0), (12.0, 1), (19.9, 1), (20.0, 0), (23.9, 0), (24.0, 1), (30, 1)])
master = env_section([(0, 0.0), (0.05, 1), (29.0, 1), (30, 0.0)])

pos_in_beat = np.mod(t, beat)
beat_index = np.floor(t / beat).astype(int)
bar = (beat_index // 4) % 4

# --- kick ------------------------------------------------------------------------------------------------------
f_kick = 48 + 90 * np.exp(-pos_in_beat * 35)
ph = 2 * np.pi * np.cumsum(f_kick) / FS
ph -= ph[0]
# restart phase at each beat
start_phase = np.zeros(N)
idx = np.searchsorted(t, beat_index * beat)
kick_phase = ph - ph[np.clip(idx, 0, N - 1)]
kick = np.sin(kick_phase) * np.exp(-pos_in_beat * 9) * 0.55
kick += 0.08 * np.exp(-pos_in_beat * 300) * rng.standard_normal(N)  # click
kick *= drums_on * (0.85 + 0.15 * chorus)

# --- snare on 2 and 4 (chorus louder) --------------------------------------------------------------------------
is_backbeat = (beat_index % 2) == 1
sn_noise = rng.standard_normal(N)
sn_noise = sn_noise - onepole_lp(sn_noise, 1500)  # high-passed noise
snare = (0.35 * sn_noise + 0.4 * np.sin(2 * np.pi * 185 * pos_in_beat)) * np.exp(-pos_in_beat * 18)
snare *= 0.7 * is_backbeat * drums_on * (0.45 + 0.55 * chorus)

# --- hats (8ths, off-beat accent), slightly right ----------------------------------------------------------------
eighth = beat / 2
pos8 = np.mod(t, eighth)
idx8 = np.floor(t / eighth).astype(int)
hat_noise = rng.standard_normal(N)
hat_noise = np.diff(np.diff(hat_noise, prepend=0), prepend=0) / 4
hat_amp = np.where(idx8 % 2 == 1, 1.0, 0.55)
hats = 0.22 * hat_noise * np.exp(-pos8 * 70) * hat_amp
hats *= np.maximum(drums_on, env_section([(0, 0.4), (3.9, 0.6), (4, 0), (30, 0)])) * (0.8 + 0.2 * chorus)

# --- chords ----------------------------------------------------------------------------------------------------
chords = [(110.0, 130.81, 164.81, 196.0), (87.31, 110.0, 130.81, 164.81),
          (98.0, 123.47, 146.83, 196.0), (82.41, 103.83, 123.47, 164.81)]
padL = np.zeros(N)
padR = np.zeros(N)
for v in range(4):
    f = np.array([c[v] for c in chords])[bar] * 2
    for det, side in ((0.997, 'L'), (1.003, 'R')):
        phv = 2 * np.pi * np.cumsum(f * det) / FS
        saw = sum(np.sin(k * phv) / k for k in range(1, 9))
        if side == 'L':
            padL += saw
        else:
            padR += saw
pad_level = env_section([(0, 0.0), (2.0, 0.6), (4, 0.55), (12, 0.6), (12.01, 0.75), (20, 0.75), (20.01, 0.9),
                         (24, 0.9), (24.01, 0.75), (30, 0.75)])
pump = 1 - 0.45 * np.exp(-pos_in_beat * 8) * drums_on
padL = fft_filter(padL, lambda f: 1 / np.sqrt(1 + (f / 2500) ** 2)) * 0.075 * pad_level * pump
padR = fft_filter(padR, lambda f: 1 / np.sqrt(1 + (f / 2500) ** 2)) * 0.075 * pad_level * pump

# --- bass --------------------------------------------------------------------------------------------------------
fb = np.array([c[0] for c in chords])[bar] / 2
phb = 2 * np.pi * np.cumsum(fb) / FS
bass = np.sin(phb) + 0.35 * np.sin(2 * phb) + 0.12 * np.sin(3 * phb)
bass_env = (1 - 0.8 * np.exp(-pos_in_beat * 7)) * np.minimum(1, pos_in_beat * 200)
bass *= 0.36 * bass_env * drums_on * (0.85 + 0.15 * chorus)

# --- vocal-like formant voice ------------------------------------------------------------------------------------
melody = [220.0, 246.94, 261.63, 293.66, 261.63, 246.94, 220.0, 196.0]
note_len = beat * 2
note_idx = (np.floor(t / note_len).astype(int)) % len(melody)
f0 = np.array(melody)[note_idx]
f0 = onepole_lp(f0, 12.0)  # portamento
vib = 1 + 0.012 * np.sin(2 * np.pi * 5.5 * t) * np.minimum(1, np.mod(t, note_len) / 0.25)
f0 = f0 * vib
vowels = [((700, 1220, 2600), (1.0, 0.5, 0.25)), ((300, 2300, 3000), (1.0, 0.35, 0.3)),
          ((450, 800, 2830), (1.0, 0.6, 0.2)), ((500, 1700, 2500), (1.0, 0.45, 0.25))]
vow_idx = (np.floor(t / note_len).astype(int)) % len(vowels)
phv = 2 * np.pi * np.cumsum(f0) / FS
voice = np.zeros(N)
for k in range(1, 40):
    fk = k * f0
    amp = np.zeros(N)
    for vi, (fs_, gs) in enumerate(vowels):
        mask = vow_idx == vi
        if not mask.any():
            continue
        a = sum(g / (1 + ((fk[mask] - F) / (0.09 * F + 60)) ** 2) for F, g in zip(fs_, gs))
        amp[mask] = a
    amp *= 1 / k ** 0.6
    amp[fk > 0.45 * FS] = 0
    voice += amp * np.sin(k * phv)
voice += 0.02 * rng.standard_normal(N) * (voice != 0)  # breath
voice_gate = np.minimum(1, np.mod(t, note_len) / 0.03) * np.minimum(1, (note_len - np.mod(t, note_len)) / 0.06)
voice_level = env_section([(0, 0), (3.9, 0), (4.5, 0.6), (12, 0.6), (12.01, 0.85), (20, 0.85), (20.01, 0.75),
                           (24, 0.75), (24.01, 0.9), (30, 0.9)])
voice_dyn = 0.75 + 0.25 * np.sin(2 * np.pi * t / (note_len * 4))  # phrase dynamics
voice *= 0.085 * voice_gate * voice_level * voice_dyn
voice_l = voice * 1.0
voice_r = np.concatenate([np.zeros(9), voice[:-9]]) * 0.97  # tiny width

# --- mix ---------------------------------------------------------------------------------------------------------
L = kick + snare * 1.0 + hats * 0.8 + bass + padL + voice_l
R = kick + snare * 0.95 + hats * 1.0 + bass + padR + voice_r
L *= master
R *= master
peak = max(np.max(np.abs(L)), np.max(np.abs(R)))
g = 10 ** (-4.0 / 20) / peak  # premaster: -4 dBFS peak
L *= g
R *= g

out = sys.argv[1]
data = np.stack([L, R], axis=1)
ints = np.clip(np.round(data * 8388607), -8388608, 8388607).astype(np.int32)
b = ints.astype('<i4').tobytes()
b3 = bytearray()
arr = np.frombuffer(b, dtype=np.uint8).reshape(-1, 4)[:, :3]
with wave.open(out, 'wb') as w:
    w.setnchannels(2)
    w.setsampwidth(3)
    w.setframerate(FS)
    w.writeframes(arr.tobytes())
print('wrote', out, 'peak gain', g)
