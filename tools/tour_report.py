#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
"""A SLY1_TOUR run's log by stop: python3 tools/tour_report.py LOG (docs/RENDERER.md)."""
import re,sys
log=sys.argv[1]
stops=[]; d={}; loads=0
for l in open(log,errors='replace'):
    m=re.match(r'tour: stop (\d+) at .* frame (\d+) shown',l)
    if m: stops.append((int(m.group(1)),int(m.group(2))))
    m=re.match(r'tour: over, frame (\d+)',l)
    if m: stops.append((-1,int(m.group(1))))
    if 'a level load' in l and stops: loads+=1
    m=re.match(r'perf: frames to (\d+): ([\d.]+) ms a frame; render thread ([\d.]+), GS thread ([\d.]+), the game.s threads ([\d.]+)',l)
    if m: d.setdefault(int(m.group(1)),{}).update(ms=float(m.group(2)),r=float(m.group(3)),g=float(m.group(4)),game=float(m.group(5)))
    m=re.match(r'perfgpu: to (\d+): a present waited ([\d.]+) ms for a screen image and ([\d.]+) ms for the GPU',l)
    if m: d.setdefault(int(m.group(1)),{}).update(gpu=float(m.group(3)))
    m=re.match(r'perfpos: to (\d+): the player at (-?\d+) (-?\d+) (-?\d+)',l)
    if m: d.setdefault(int(m.group(1)),{})['p']=(int(m.group(2)),int(m.group(3)))
print('level loads after the tour began:',loads)
tot=[]
for (k,a),(_,b) in zip(stops,stops[1:]):
    rs=[x for f,x in d.items() if a+120<f<=b and 'ms' in x]
    if not rs: continue
    av=lambda key: sum(r.get(key,0) for r in rs)/len(rs)
    ps={r.get('p') for r in rs}
    tot.append(av('ms'))
    print('stop %d: %.1f ms a frame (worst window %.1f); render %.1f, GS %.1f, game %.1f, GPU wait %.1f; positions %s'%(k,av('ms'),max(r['ms'] for r in rs),av('r'),av('g'),av('game'),av('gpu'),sorted(ps)[:2]))
print('mean of stops: %.1f ms'%(sum(tot)/max(len(tot),1)))
