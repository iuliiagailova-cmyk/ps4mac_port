#!/usr/bin/env python3
"""Sums xctrace metal-gpu-intervals by our encoder label (SPARTAN_GPU_PROFILE=labels with
KK_LABEL_ENCODERS=1: the label after ' & '), ms per frame. Usage: gpu_labels.py <xml> <fps> [top]"""
import sys, collections, re, xml.etree.ElementTree as ET
vals = {}
for _, e in ET.iterparse(sys.argv[1]):
    if 'id' in e.attrib and e.attrib['id'] not in vals: vals[e.attrib['id']] = (e.text or '', e.attrib.get('fmt', ''))
def val(e):
    if 'ref' in e.attrib: return vals[e.attrib['ref']]
    v = (e.text or '', e.attrib.get('fmt', ''))
    if 'id' in e.attrib: vals[e.attrib['id']] = v
    for c in e: val(c)
    return v
by = collections.Counter(); cnt = collections.Counter(); chan_total = collections.Counter()
t0 = t1 = None
for ev, row in ET.iterparse(sys.argv[1]):
    if row.tag != 'row': continue
    cols = list(row)
    start = int(val(cols[0])[0]); dur = int(val(cols[1])[0])
    chan = val(cols[2])[1]; depth = val(cols[5])[1]; label = val(cols[6])[1]
    for c in cols[7:]: val(c)
    if depth != '0' or 'ps4mac_service' not in label and '&' not in label: continue
    if label.startswith('GPU Execution'): continue
    t0 = start if t0 is None else min(t0, start); t1 = max(t1 or 0, start + dur)
    ours = label.split(' & ', 1)[1] if ' & ' in label else '(no label)'
    ours = re.sub(r'\s+\(ps4mac_service.*$', '', ours)
    kind = 'dispatch' if ours.startswith('dispatch') else 'pass' if ours.startswith('pass') else 'other'
    by[(chan, ours)] += dur; cnt[(chan, ours)] += 1; chan_total[chan] += dur
span = (t1 - t0) / 1e9
frames = span * float(sys.argv[2])
print(f'span {span:.2f} s, ~{frames:.0f} frames at {sys.argv[2]} FPS')
for c, d in chan_total.most_common(): print(f'{c:10s} {d/1e6/frames:7.2f} ms/frame')
top = int(sys.argv[3]) if len(sys.argv) > 3 else 30
for (c, l), d in by.most_common(top):
    print(f'  {c:9s} {d/1e6/frames:6.2f} ms/frame {cnt[(c,l)]/frames:5.1f}/frame  {l[:120]}')
