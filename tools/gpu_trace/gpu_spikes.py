#!/usr/bin/env python3
"""Stutter hunt on xctrace metal-gpu-intervals (labels on): the longest single GPU intervals, and
GPU busy per 100 ms. Usage: gpu_spikes.py <xml>"""
import sys, re, collections, xml.etree.ElementTree as ET
vals = {}
for _, e in ET.iterparse(sys.argv[1]):
    if 'id' in e.attrib and e.attrib['id'] not in vals: vals[e.attrib['id']] = (e.text or '', e.attrib.get('fmt', ''))
def val(e):
    if 'ref' in e.attrib: return vals[e.attrib['ref']]
    v = (e.text or '', e.attrib.get('fmt', ''))
    if 'id' in e.attrib: vals[e.attrib['id']] = v
    for c in e: val(c)
    return v
rows = []
for ev, row in ET.iterparse(sys.argv[1]):
    if row.tag != 'row': continue
    cols = list(row)
    start = int(val(cols[0])[0]); dur = int(val(cols[1])[0])
    chan = val(cols[2])[1]; depth = val(cols[5])[1]; label = val(cols[6])[1]
    for c in cols[7:]: val(c)
    if depth != '0' or ('ps4mac_service' not in label and '&' not in label): continue
    ours = label.split(' & ', 1)[1] if ' & ' in label else label
    ours = re.sub(r'\s+\(ps4mac_service.*$', '', ours)
    rows.append((start, dur, chan, ours))
rows.sort()
t0 = rows[0][0]
print(f'{len(rows)} intervals over {(rows[-1][0]-t0)/1e9:.2f} s')
print('Longest single intervals:')
for start, dur, chan, l in sorted(rows, key=lambda r: -r[1])[:25]:
    print(f'  {dur/1e6:7.2f} ms at {(start-t0)/1e9:6.3f} s  {chan:9s} {l[:110]}')
# Busy per 100 ms bucket (union of intervals, all channels).
bucket = 100_000_000
busy = collections.Counter()
ivs = sorted((s, s + d) for s, d, c, l in rows)
cur_s, cur_e = None, None
merged = []
for s, e in ivs:
    if cur_e is None or s > cur_e:
        if cur_e is not None: merged.append((cur_s, cur_e))
        cur_s, cur_e = s, e
    else:
        cur_e = max(cur_e, e)
merged.append((cur_s, cur_e))
for s, e in merged:
    while s < e:
        b = (s - t0) // bucket; be = t0 + (b + 1) * bucket
        busy[b] += min(e, be) - s; s = min(e, be)
line = ''.join(' .:-=+*#%@'[min(9, int(busy[b] / bucket * 10))] for b in range(max(busy) + 1))
print('GPU busy per 100 ms (space idle ... @ full):')
for i in range(0, len(line), 50): print(f'  {i/10:5.1f}s |{line[i:i+50]}|')
# Gaps: GPU idle stretches over 30 ms.
gaps = [(merged[i][1], merged[i+1][0]) for i in range(len(merged)-1) if merged[i+1][0] - merged[i][1] > 30_000_000]
print(f'GPU idle gaps over 30 ms: {len(gaps)}')
for a, b in gaps[:20]: print(f'  {(b-a)/1e6:7.1f} ms idle at {(a-t0)/1e9:6.3f} s')
