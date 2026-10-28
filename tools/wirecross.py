"""The one that earns its keep: every wire segment intersected against every
text bbox, so the sheet cannot quietly draw a wire through its own words.
Most hits are the sheet's own conventions - a wire through its W# tag, a
component body sitting on its wire - so read the list, do not just count it.

Segments are inflated by stroke width: a vertical wire has zero bbox area and
will otherwise never intersect anything.

  python3 tools/wirecross.py docs/woods-backplate-wiring.svg
"""
import re,io,sys
s=io.open(sys.argv[1],encoding='utf-8').read()
FS={'tt':16,'t':13,'ts':11,'tw':12,'tk':11.5,'note':11,'pin':12.5,'pinno':10,'wtag':10.5,'nc':12}
texts=[]
for m in re.finditer(r'<text x="(-?\d+)" y="(-?\d+)" class="([\w ]+)"[^>]*>(.*?)</text>',s,re.S):
    x,y,c=float(m.group(1)),float(m.group(2)),m.group(3).split()[0]
    body=re.sub(r'<[^>]*>','',m.group(4))
    if not body.strip(): continue
    f=FS.get(c,12); w=len(body)*f*0.50
    texts.append((x,y-f*0.75,x+w,y+f*0.2,body[:44],'halo' in m.group(3)))
segs=[]
for m in re.finditer(r'<polyline class="(\w+)" points="([^"]+)"',s):
    cls=m.group(1); pts=[tuple(map(float,p.split(','))) for p in m.group(2).split()]
    for a,b in zip(pts,pts[1:]): segs.append((cls,a,b))
for m in re.finditer(r'<line x1="(-?\d+)" y1="(-?\d+)" x2="(-?\d+)" y2="(-?\d+)" class="(\w+)"',s):
    x1,y1,x2,y2=(float(m.group(i)) for i in (1,2,3,4)); segs.append((m.group(5),(x1,y1),(x2,y2)))
prob=[]
for cls,a,b in segs:
    if cls in ('bg',): continue
    x0,x1=sorted((a[0],b[0])); y0,y1=sorted((a[1],b[1]))
    x0-=1.6; x1+=1.6; y0-=1.6; y1+=1.6   # a wire has width; a zero-area segment still covers text
    for t in texts:
        if t[5]: continue                      # haloed text is deliberately readable over wires
        ix=min(x1,t[2])-max(x0,t[0]); iy=min(y1,t[3])-max(y0,t[1])
        if ix>2 and iy>2:
            prob.append("%-5s seg(%.0f,%.0f)-(%.0f,%.0f) through %r @%.0f,%.0f"%(cls,a[0],a[1],b[0],b[1],t[4],t[0],t[1]+12))
print("WIRE-THROUGH-TEXT: %d"%len(prob))
for x in sorted(set(prob)): print("  -",x)
