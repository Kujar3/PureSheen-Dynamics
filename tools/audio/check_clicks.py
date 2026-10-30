"""Click / NaN check of a render against its input (both aligned, same length).
Usage: python tools/audio/check_clicks.py <in.wav> <out.wav>   (renders aligned with their input)
Prints: nan count and the worst click score with its time. Per sample, z = |4th difference| / local RMS of the 4th
difference (+-5 ms), for output and input; a click is a sample whose z in the output is far above the input's
largest z nearby (+-1 ms), i.e. a broadband impulse the input does not have. Score = that excess (in sigma)."""
import sys
import struct
import numpy as np

def read_wav(path):
    with open(path, 'rb') as f:
        data = f.read()
    assert data[:4] == b'RIFF' and data[8:12] == b'WAVE'
    pos = 12
    fmt = None
    while pos < len(data):
        cid = data[pos:pos + 4]
        size = struct.unpack('<I', data[pos + 4:pos + 8])[0]
        body = data[pos + 8:pos + 8 + size]
        if cid == b'fmt ':
            tag, ch, rate, _, _, bits = struct.unpack('<HHIIHH', body[:16])
            if tag == 0xFFFE:
                tag = struct.unpack('<H', body[24:26])[0]
            fmt = (tag, ch, rate, bits)
        elif cid == b'data':
            tag, ch, rate, bits = fmt
            if tag == 3 and bits == 32:
                x = np.frombuffer(body, dtype='<f4').astype(np.float64)
            elif tag == 1 and bits == 24:
                b = np.frombuffer(body, dtype=np.uint8).reshape(-1, 3)
                v = (b[:, 0].astype(np.int32) | (b[:, 1].astype(np.int32) << 8) | (b[:, 2].astype(np.int32) << 16))
                v = np.where(v >= 1 << 23, v - (1 << 24), v)
                x = v / 8388608.0
            elif tag == 1 and bits == 16:
                x = np.frombuffer(body, dtype='<i2') / 32768.0
            else:
                raise ValueError('format %s' % (fmt,))
            return x.reshape(-1, ch), rate
        pos += 8 + size + (size & 1)
    raise ValueError('no data')


def zscore(x):
    d = np.diff(x, n=4, axis=0)
    e = np.sum(d * d, axis=1)
    w = 441
    c = np.cumsum(np.concatenate([[0.0], e]))
    lo = np.clip(np.arange(len(e)) - w, 0, len(e))
    hi = np.clip(np.arange(len(e)) + w, 0, len(e))
    rms = np.sqrt((c[hi] - c[lo]) / (hi - lo) + 1e-24)
    return np.sqrt(e) / rms, e

def local_max(z, r):
    from numpy.lib.stride_tricks import sliding_window_view
    p = np.pad(z, r, mode='edge')
    return sliding_window_view(p, 2 * r + 1).max(axis=1)

def click_score(x, y, rate):
    zx, ex = zscore(x)
    zy, ey = zscore(y)
    ref = local_max(zx, int(0.001 * rate))
    s = zy - np.maximum(ref, 3.0)
    i = int(np.argmax(s))
    return s[i], i / rate

if __name__ == '__main__':
    x, rate = read_wav(sys.argv[1])
    y, _ = read_wav(sys.argv[2])
    n = min(len(x), len(y))
    x, y = x[:n], y[:n]
    nans = int(np.sum(~np.isfinite(y)))
    y = np.nan_to_num(y)
    sc, at = click_score(x, y, rate)
    print('nan %d  click %.1f sigma at %.2f s' % (nans, sc, at))
