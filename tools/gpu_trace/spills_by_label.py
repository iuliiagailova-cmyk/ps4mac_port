#!/usr/bin/env python3
"""Register spill events (xctrace graphics-compiler-spill-events) by encoder label (encoder ids
joined with metal-gpu-intervals). Usage: spills_by_label.py <spills.xml> <intervals.xml> [top]"""
import sys, re, collections, xml.etree.ElementTree as ET
def load(f):
    vals = {}
    for _, e in ET.iterparse(f):
        if 'id' in e.attrib and e.attrib['id'] not in vals: vals[e.attrib['id']] = (e.text or '', e.attrib.get('fmt', ''))
    def val(e):
        if 'ref' in e.attrib: return vals[e.attrib['ref']]
        v = (e.text or '', e.attrib.get('fmt', ''))
        if 'id' in e.attrib: vals[e.attrib['id']] = v
        for c in e: val(c)
        return v
    return [[val(c) for c in row] for _, row in ET.iterparse(f) if row.tag == 'row']
spills, ivs = load(sys.argv[1]), load(sys.argv[2])
lab, chan = {}, {}
for r in ivs:
    if len(r) > 16:
        enc, label = r[16][0], r[6][1]
        chan.setdefault(enc, r[2][1])
        if ' & ' in label: lab[enc] = re.sub(r'\s+\(ps4mac_service.*$', '', label.split(' & ', 1)[1])
by, cnt, sizes = collections.Counter(), collections.Counter(), collections.defaultdict(set)
for r in spills:
    enc = r[2][0]; b = int(re.sub(r'\D', '', r[3][1]) or 0)
    l = lab.get(enc, f'(no label, {chan.get(enc, "?")})')
    by[l] += b; cnt[l] += 1; sizes[l].add(b)
print(f'{len(spills)} spill events')
for l, b in by.most_common(int(sys.argv[3]) if len(sys.argv) > 3 else 25):
    print(f'{cnt[l]:5d} events, spill sizes {sorted(sizes[l])} B  {l[:150]}')
