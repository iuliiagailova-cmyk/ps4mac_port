#!/usr/bin/env python3
"""Where a frame's time goes on its critical path: the guest's timeline (SPARTAN_LATENCY CSV) and
the service's SpartanTrace JSON (SPARTAN_TRACE, with SPARTAN_LAT_ADDR marks) on one clock.

    latency.py <prefix>      reads <prefix>-guest.csv and <prefix>-service.json

Per frame (guest PREPARE_FLIP to the next one, inside the trace window): the game's main thread
time waiting for the GPU result (rip in SPARTAN_LAT_WAIT), in the pause loop (SPARTAN_LAT_PAUSE:
R&C waits there for its render thread) and elsewhere; guest page-fault waits; and for the GPU
result the main thread waited on last, the chain of the readback that delivered it:
queued (copy recorded) -> submitted to Metal -> GPU done -> published -> main thread leaves the wait.
"""
import bisect, collections, json, re, sys
from pathlib import Path

prefix = sys.argv[1]
long_ms = float(sys.argv[2]) if len(sys.argv) > 2 else 40.0

# Message kinds from the bridge header.
hdr = (Path(__file__).resolve().parents[2] / 'bridge/ps4mac_bridge.h').read_text()
enum = re.search(r'enum Ps4macToGraphics \{(.*?)\};', hdr, re.S).group(1)
kinds, v = {}, 0
for m in re.finditer(r'(PS4MAC_\w+)\s*(?:=\s*(\d+))?\s*,', enum):
    v = int(m.group(2)) if m.group(2) else v + 1
    kinds[m.group(1)] = v
FLIP = kinds['PS4MAC_GFX_PREPARE_FLIP']
SUBMIT = {kinds['PS4MAC_SUBMIT_GFX'], kinds['PS4MAC_SUBMIT_COMPUTE']}
DONE = kinds['PS4MAC_SUBMIT_DONE']

# Guest timeline (absolute ns).
g = []
for line in open(prefix + '-guest.csv'):
    p = line.strip().split(',')
    if len(p) == 3:
        g.append((int(p[0]), p[1], int(p[2])))
g.sort()

# Service trace (ts in us from base_ns).
d = json.load(open(prefix + '-service.json'))
base = d['base_ns']
ev = d['traceEvents']
tnames = {e['tid']: e['args']['name'] for e in ev if e['ph'] == 'M'}
def ns(e): return base + int(e['ts'] * 1000)
marks = collections.defaultdict(list)   # name -> [(t, arg)]
spans = collections.defaultdict(list)   # name -> [(t0, t1, arg, thread)]
for e in ev:
    if e['ph'] == 'i':
        marks[e['name']].append((ns(e), e['args']['v']))
    elif e['ph'] == 'X':
        t0 = ns(e)
        spans[e['name']].append((t0, t0 + int(e['dur'] * 1000), e['args']['v'], tnames.get(e['tid'], '?')))
for v in marks.values(): v.sort()
for v in spans.values(): v.sort()
t_lo = base
t_hi = max([t for v in marks.values() for t, _ in v] + [t1 for v in spans.values() for _, t1, _, _ in v])
print(f'trace window {(t_hi - t_lo) / 1e9:.2f} s; guest events {len(g)}')

# Clock check: guest message send -> service receipt (should be small and positive).
sends = [t for t, k, val in g if k == 'S' and val in SUBMIT and t_lo <= t <= t_hi]
recv = [t for t, a in marks.get('guest message', []) if a in SUBMIT]
if sends and recv:
    lags = []
    for t in sends:
        i = bisect.bisect_left(recv, t)
        if i < len(recv): lags.append(recv[i] - t)
    lags.sort()
    print(f'clock check: guest submit -> service receipt median {lags[len(lags)//2]/1e3:.0f} us '
          f'(p90 {lags[int(len(lags)*.9)]/1e3:.0f} us, min {lags[0]/1e3:.0f} us)')

# Main thread state intervals.
state_iv = []  # (t0, t1, state)
cur, t_cur = None, None
for t, k, val in g:
    if k != 'M': continue
    if cur is not None: state_iv.append((t_cur, t, cur))
    cur, t_cur = val, t
faults = [(t, val // 2, val & 1) for t, k, val in g if k == 'F']
render_iv = []  # (t0, t1, in game code)
cur, t_cur = None, None
for t, k, val in g:
    if k != 'R': continue
    if cur is not None: render_iv.append((t_cur, t, cur))
    cur, t_cur = val, t
# Slow faults (over 2 ms): X address, P pc, T on the main thread; their wait ends at the F before.
slow = []
for i, (t, k, val) in enumerate(g):
    if k == 'X' and i and g[i - 1][1] == 'F':
        slow.append((t - (g[i - 1][2] // 2), t))
flips = [t for t, k, val in g if k == 'S' and val == FLIP and t_lo <= t <= t_hi]

pub = marks.get('watched readback published', [])
queued = {}
for t, tick in marks.get('watched readback queued', []): queued.setdefault(tick, t)
submits = {}
for t0, t1, tick, _ in spans.get('submit to Metal', []): submits.setdefault(tick, (t0, t1))
gpu_done = {}
for t, tick in marks.get('GPU tick done', []): gpu_done.setdefault(tick, t)
dropped = len(marks.get('watched readback dropped', []))

def overlap(a0, a1, b0, b1): return max(0, min(a1, b1) - max(a0, b0))

rows = []
for f0, f1 in zip(flips, flips[1:]):
    r = {'t': (f0 - t_lo) / 1e6, 'frame': (f1 - f0) / 1e6, 'wait': 0, 'pause': 0, 'other': 0}
    last_wait = None
    for s0, s1, st in state_iv:
        o = overlap(f0, f1, s0, s1)
        if not o: continue
        r[('other', 'wait', 'pause')[st]] += o / 1e6
        if st == 1: last_wait = (s0, s1)
    r['render_game'] = sum(overlap(f0, f1, a, b) for a, b, st in render_iv if st == 1) / 1e6
    r['render_out'] = sum(overlap(f0, f1, a, b) for a, b, st in render_iv if st == 0) / 1e6
    r['slow_faults'] = sum(overlap(f0, f1, a, b) for a, b in slow) / 1e6
    r['faults'] = sum(w for t, w, _ in faults if f0 <= t < f1) / 1e6
    r['nfaults'] = sum(1 for t, _, _ in faults if f0 <= t < f1)
    r['chain'] = None
    if last_wait:
        w0, w1 = last_wait
        i = bisect.bisect_right(pub, (w1, 1 << 62)) - 1   # the last publish before the wait ended
        if i >= 0 and w1 - pub[i][0] < 100_000_000:
            tp, tick = pub[i]
            q = queued.get(tick); s = submits.get(tick); gd = gpu_done.get(tick)
            r['chain'] = {
                'wait': (w1 - w0) / 1e6,
                'q->submit': (s[0] - q) / 1e6 if q and s else None,
                'submit': (s[1] - s[0]) / 1e6 if s else None,
                'submit->gpu done': (gd - s[1]) / 1e6 if gd and s else None,
                'gpu done->publish': (tp - gd) / 1e6 if gd else None,
                'publish->seen': (w1 - tp) / 1e6,
                'wait start->q': (q - w0) / 1e6 if q else None,
            }
    rows.append(r)

if not rows:
    sys.exit('no frames in the window')

def fmt(x): return '   -  ' if x is None else f'{x:6.1f}'
CH = ['wait start->q', 'q->submit', 'submit', 'submit->gpu done', 'gpu done->publish', 'publish->seen']
print(f'\n{len(rows)} frames, {sum(r["frame"] > long_ms for r in rows)} over {long_ms:.0f} ms; '
      f'watched readbacks dropped {dropped}')
print('ms per frame (main thread): GPU-result wait / pause loop (render thread) / other; faults (count)')
for label, sel in (('all', rows), (f'>{long_ms:.0f} ms', [r for r in rows if r['frame'] > long_ms]),
                   (f'<={long_ms:.0f} ms', [r for r in rows if r['frame'] <= long_ms])):
    if not sel: continue
    n = len(sel)
    print(f'  {label:>8}: n={n:4}  frame {sum(r["frame"] for r in sel)/n:5.1f}  wait {sum(r["wait"] for r in sel)/n:5.1f}  '
          f'pause {sum(r["pause"] for r in sel)/n:5.1f}  other {sum(r["other"] for r in sel)/n:5.1f}  '
          f'faults {sum(r["faults"] for r in sel)/n:4.1f} ({sum(r["nfaults"] for r in sel)/n:.1f})')
    if render_iv:
        print(f'            render thread: game code {sum(r["render_game"] for r in sel)/n:5.1f}, outside it '
              f'{sum(r["render_out"] for r in sel)/n:5.1f} (slow faults, any thread: {sum(r["slow_faults"] for r in sel)/n:5.1f})')
    ch = [r['chain'] for r in sel if r['chain']]
    if ch:
        avg = []
        for k in CH:
            vals = [c[k] for c in ch if c[k] is not None]
            avg.append(f'{k} {sum(vals)/len(vals):.1f}' if vals else f'{k} -')
        print(f'            chain of the last GPU result (n={len(ch)}): ' + ', '.join(avg))

print(f'\nframes over {long_ms:.0f} ms (t s into the window):')
print('     t   frame   wait  pause  other faults | ' + ' | '.join(f'{k:>16}' for k in CH))
for r in rows:
    if r['frame'] <= long_ms: continue
    c = r['chain'] or {}
    print(f'{r["t"]/1e3:6.2f} {r["frame"]:6.1f} {r["wait"]:6.1f} {r["pause"]:6.1f} {r["other"]:6.1f} {r["faults"]:6.1f} | '
          + ' | '.join(f'{fmt(c.get(k)):>16}' for k in CH))
