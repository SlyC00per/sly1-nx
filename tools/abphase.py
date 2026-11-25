#!/usr/bin/env python3
"""abphase.py LOG ONPHASE [LOG ONPHASE ...]: a bench log split by 600-frame phases (an env switch
that alternates): windows in 3D at 60, mean frame, late frames, for the phases where the switch
was on (ONPHASE = 0 or 1: frames/600 % 2) and off."""
import re, sys, statistics as st
tot={True:[],False:[]}
late={True:0,False:0}
for path,on in zip(sys.argv[1::2],sys.argv[2::2]):
    on=int(on)
    for l in open(path,errors='replace'):
        m=re.match(r"perf: frames to (\d+): ([\d.]+) ms a frame; render thread ([\d.]+), GS thread ([\d.]+), the game's threads ([\d.]+)",l)
        if m:
            k=int(m[1]); f=float(m[2])
            if f>40 or float(m[3])<1.0: continue      # loads, 2D
            ph=((k-30)//600)%2==on
            tot[ph].append((f,float(m[3]),float(m[4]),float(m[5])))
        m=re.match(r"late: frame (\d+) after",l)
        if m: late[((int(m[1]))//600)%2==on]+=1
for ph,name in ((True,'on '),(False,'off')):
    w=tot[ph]
    if not w: continue
    print("%s: %3d windows, %.1f%% at 60 (<=16.9), mean frame %.2f, render %.2f, GS %.2f, game %.2f; windows >18 ms %.1f%%; late lines %d (%.0f per 100 windows)"%(
        name,len(w),100*sum(1 for x in w if x[0]<=16.9)/len(w),st.mean(x[0] for x in w),st.mean(x[1] for x in w),st.mean(x[2] for x in w),st.mean(x[3] for x in w),
        100*sum(1 for x in w if x[0]>18)/len(w),late[ph],100*late[ph]/len(w)))
