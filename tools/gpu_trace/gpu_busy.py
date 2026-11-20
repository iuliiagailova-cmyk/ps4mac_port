#!/usr/bin/env python3
"""Union of GPU intervals (all channels, rows of the given process) from xctrace metal-gpu-intervals:
how much of the time the GPU was busy at all, and per channel; plus the same for 'GPU Execution' rows
alone and encoder rows alone. Usage: gpu_busy.py <xml> <process substring>"""
import sys, collections, xml.etree.ElementTree as ET
vals = {}
for _, e in ET.iterparse(sys.argv[1]):
    if 'id' in e.attrib and e.attrib['id'] not in vals: vals[e.attrib['id']] = (e.text or '', e.attrib.get('fmt', ''))
def val(e):
    if 'ref' in e.attrib: return vals[e.attrib['ref']]
    v = (e.text or '', e.attrib.get('fmt', ''))
    if 'id' in e.attrib: vals[e.attrib['id']] = v
    for c in e: val(c)
    return v
groups = collections.defaultdict(list)
for ev, row in ET.iterparse(sys.argv[1]):
    if row.tag != 'row': continue
    cols = list(row)
    start = int(val(cols[0])[0]); dur = int(val(cols[1])[0])
    chan = val(cols[2])[1]; depth = val(cols[5])[1]; label = val(cols[6])[1]
    for c in cols[7:]: val(c)
    if depth != '0' or sys.argv[2] not in label: continue
    kind = 'exec' if label.startswith('GPU Execution') else 'enc'
    for key in ('all', 'all:' + kind, chan, chan + ':' + kind):
        groups[key].append((start, start + dur))
def union(iv):
    iv.sort(); total = 0; cur_s = cur_e = None
    for s, e in iv:
        if cur_e is None or s > cur_e:
            if cur_e is not None: total += cur_e - cur_s
            cur_s, cur_e = s, e
        else: cur_e = max(cur_e, e)
    if cur_e is not None: total += cur_e - cur_s
    return total
a = groups['all']; span = max(e for _, e in a) - min(s for s, _ in a)
print(f'span {span/1e9:.2f} s')
for k in sorted(groups): print(f'  {k:22s} busy {100*union(groups[k])/span:5.1f}%  ({len(groups[k])} intervals)')
