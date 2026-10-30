"""Phase 2 integration render check (from the repo root):
    python tools/audio/make_test_mix.py test_audio/p2int_mix.wav
    python tools/audio/render_phase2.py [buildDir] [input.wav]
Renders the input through the default chain (null test), every global factory preset (true peak against the
Maximizer output level) and each phase 2 module alone with one of its factory presets, in Stereo and, where the
module offers it, in Transient/Sustain. Prints a table: integrated loudness, true peak, latency, the NaN / click
check (tools/audio/check_clicks.py) and the verdict. Renders go to out/renders-p2. Exit code 1 if a check fails."""
import os
import re
import subprocess
import sys

B = sys.argv[1] if len(sys.argv) > 1 else 'build-p2int'
IN = sys.argv[2] if len(sys.argv) > 2 else 'test_audio/p2int_mix.wav'
DEV = os.path.join(B, 'PureSheenDev_artefacts', 'Release', 'PureSheenDev.exe')
OUT = 'out/renders-p2'
os.makedirs(OUT, exist_ok=True)

GLOBALS = ["Default", "Soft Sheen", "Streaming Ready", "Loud and Clear", "Club Loud", "Wide Supersaw Master",
           "Deep Bass Anchor", "Acoustic Air", "Warm Glue", "Limit Only", "Radio Pop", "Rock Drive", "Hip-Hop Knock",
           "Mid-Side Detail", "Modern Loud Balance", "Gentle Acoustic Master", "Heavy Low End Master",
           "Clean Vocal Pop"]

# (prefix, module preset, offers T/S)
MODULES = [("deq", "Low-Mid Bloom Control", True), ("meq", "Pink Balance", True), ("tbal", "Gentle Balance", True),
           ("defn", "Open Up", True), ("smth", "De-Harsh Master", True), ("pnch", "Gentle Snap", False),
           ("lowc", "Tight Kick", True), ("bass", "Warm Sustain", False)]


def render(name, args):
    out = os.path.join(OUT, re.sub(r'[^A-Za-z0-9]+', '_', name).strip('_') + '.wav')
    log = subprocess.run([DEV, 'render', IN, out] + args, capture_output=True, text=True).stdout
    m = {}
    o = re.search(r'output: integrated (-?[0-9.]+|-inf) LUFS, LRA (-?[0-9.]+) LU, true peak (-?[0-9.]+|-inf) dBTP', log)
    if o:
        m['lufs'], m['lra'], m['tp'] = o.group(1), o.group(2), o.group(3)
    lat = re.search(r'latency (\d+) samples( \(CHANGED)?', log)
    m['lat'] = lat.group(1) + ('*' if lat and lat.group(2) else '') if lat else '?'
    d = re.search(r'difference output - input: peak (-?[0-9.]+|-inf) dBFS', log)
    m['diff'] = d.group(1) if d else '?'
    c = re.search(r'maximizer output level (-?[0-9.]+) dB', log)
    m['ceiling'] = float(c.group(1)) if c else None
    clicks = subprocess.run([sys.executable, 'tools/audio/check_clicks.py', IN, out], capture_output=True, text=True)
    m['clicks'] = clicks.stdout.strip()
    m['log'] = log
    return m


def clicks_ok(text):
    nan = re.search(r'nan (\d+)', text)
    score = re.search(r'click (?:score )?(-?[0-9.]+)', text)
    if nan is None:
        return False
    return int(nan.group(1)) == 0 and (score is None or float(score.group(1)) < 6.0)


failed = False
rows = []
print('%-34s %8s %8s %6s %7s  %-8s %s' % ('render', 'LUFS-I', 'TP dBTP', 'ceil', 'lat', 'verdict', 'click check'))

m = render('null default chain', [])
ok = m['diff'] == '-inf' and clicks_ok(m['clicks'])
failed |= not ok
print('%-34s %8s %8s %6s %7s  %-8s %s (difference %s dBFS)' % ('default chain (null)', m.get('lufs'), m.get('tp'), '-',
      m['lat'], 'ok' if ok else 'FAIL', m['clicks'], m['diff']))

for p in GLOBALS:
    m = render('global ' + p, ['--preset', p])
    ok = 'tp' in m and clicks_ok(m['clicks'])
    if ok and m['ceiling'] is not None and m['tp'] != '-inf':
        ok = float(m['tp']) <= m['ceiling'] + 0.05
    failed |= not ok
    print('%-34s %8s %8s %6s %7s  %-8s %s' % (p, m.get('lufs'), m.get('tp'),
          '%.1f' % m['ceiling'] if m['ceiling'] is not None else '-', m['lat'], 'ok' if ok else 'FAIL', m['clicks']))

for prefix, preset, ts in MODULES:
    variants = [('Stereo', [])]
    if ts:
        variants.append(('T/S', ['%s.chMode=Transient/Sustain' % prefix]))
    for label, extra in variants:
        m = render('%s %s %s' % (prefix, preset, label),
                   ['--chain', prefix, '--module-preset', '%s:%s' % (prefix, preset)] + extra)
        ok = 'tp' in m and clicks_ok(m['clicks']) and 'bad override' not in m['log']
        failed |= not ok
        print('%-34s %8s %8s %6s %7s  %-8s %s' % ('%s "%s" %s' % (prefix, preset, label), m.get('lufs'), m.get('tp'),
              '-', m['lat'], 'ok' if ok else 'FAIL', m['clicks']))

sys.exit(1 if failed else 0)
