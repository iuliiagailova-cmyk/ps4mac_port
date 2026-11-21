#!/usr/bin/env python3
"""Time Profiler export (time-profile table) of the guest: CPU samples per thread, a timeline of
the busiest threads per 100 ms, and their top leaf frames. Usage: guest_profile.py <xml>"""
import sys, collections, xml.etree.ElementTree as ET
ids = {}
for _, e in ET.iterparse(sys.argv[1]):
    if 'id' in e.attrib and e.attrib['id'] not in ids: ids[e.attrib['id']] = e
def res(e): return ids[e.attrib['ref']] if 'ref' in e.attrib else e
samples = []
for _, row in ET.iterparse(sys.argv[1]):
    if row.tag != 'row': continue
    cols = list(row)
    t = int(res(cols[0]).text)
    thread = res(cols[1]).attrib.get('fmt', '?')
    state = res(cols[4]).attrib.get('fmt', '')
    leaf = '?'; second = ''
    bt = res(cols[6]) if len(cols) > 6 else None
    if bt is not None:
        frames = [res(f) for f in bt.findall('frame')]
        def label(f):
            n = f.attrib.get('name', '?')
            if not n.startswith('0x'): return n
            a = int(f.attrib.get('addr', n), 16)
            return 'guest code' if 0xf400000000 <= a < 0xfc00000000 else f'?{a:#x}'
        names = [label(f) for f in frames]
        # leaf: the first frame that is not unknown native, plus the first named frame
        if names: leaf = names[0] if not names[0].startswith('?') else 'native ' + next((n for n in names if not n.startswith('?') and n != 'guest code'), '?')
        second = next((n for n in names[1:] if not n.startswith('?') and n != 'guest code'), '')
    samples.append((t, thread, state, leaf, second))
t0 = min(s[0] for s in samples)
per = collections.Counter(s[1] for s in samples)
print(f'{len(samples)} samples, {(max(s[0] for s in samples) - t0) / 1e9:.2f} s')
top = [t for t, _ in per.most_common(10)]
for t in top: print(f'  {per[t]:6d}  {t}')
bucket = 100_000_000
nb = (max(s[0] for s in samples) - t0) // bucket + 1
for t in top[:8]:
    cnt = collections.Counter((s[0] - t0) // bucket for s in samples if s[1] == t)
    mx = max(cnt.values())
    line = ''.join(' .:-=+*#%@'[min(9, int(cnt[b] / max(mx, 1) * 9.99))] for b in range(nb))
    print(f'{t[:38]:38s} |{line}| max {mx}/100ms')
for t in top[:6]:
    leaves = collections.Counter(s[3] + ('  <  ' + s[4] if s[4] else '') for s in samples if s[1] == t)
    print(f'== {t}')
    for l, c in leaves.most_common(6): print(f'   {c:5d}  {l[:150]}')
