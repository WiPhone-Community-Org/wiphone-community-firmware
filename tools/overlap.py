"""Overlap audit for the wiring sheet: every text bbox against every other, and
against component bodies/wire tags. A label wholly inside its own body is not
reported. Estimated glyph widths, so treat single-digit overlaps as "go look".

  python3 tools/overlap.py docs/woods-backplate-wiring.svg
"""
import re,io,sys
s=io.open(sys.argv[1],encoding='utf-8').read()
FS={'tt':16,'t':13,'ts':11,'tw':12,'tk':11.5,'note':11,'pin':12.5,'pinno':10,'wtag':10.5,'nc':12}
CWF=0.53
texts=[]
for m in re.finditer(r'<text x="(-?\d+)" y="(-?\d+)" class="(\w+)"[^>]*>(.*?)</text>',s,re.S):
    x,y,c=float(m.group(1)),float(m.group(2)),m.group(3)
    body=re.sub(r'<[^>]*>','',m.group(4))
    if not body.strip(): continue
    f=FS.get(c,12); w=len(body)*f*CWF
    texts.append((x,y-f*0.78,x+w,y+f*0.24,body[:46],c))
# component bodies + wire tags (small white/coloured rects)
rects=[]
for m in re.finditer(r'<rect x="(-?\d+)" y="(-?\d+)" width="(\d+)" height="(\d+)"(?![^>]*class="(?:box|chk|warn|pwr|hdr|rf|gps|bg)")',s):
    x,y,w,h=(float(m.group(i)) for i in (1,2,3,4))
    if w<=90 and h<=30: rects.append((x,y,x+w,y+h))
def ov(a,b):
    ix=min(a[2],b[2])-max(a[0],b[0]); iy=min(a[3],b[3])-max(a[1],b[1])
    return ix,iy
prob=[]
for i in range(len(texts)):
    for j in range(i+1,len(texts)):
        a,b=texts[i],texts[j]
        ix,iy=ov(a,b)
        if ix>3 and iy>3:
            prob.append("TEXT/TEXT  %.0fx%.0f  %r @%.0f,%.0f  ||  %r @%.0f,%.0f"%(ix,iy,a[4],a[0],a[1]+12,b[4],b[0],b[1]+12))
for t in texts:
    for r in rects:
        ix,iy=ov(t,r)
        contained = t[0]>=r[0]-2 and t[2]<=r[2]+2 and t[1]>=r[1]-4 and t[3]<=r[3]+4
        if ix>3 and iy>3 and not contained:
            prob.append("TEXT/PART  %.0fx%.0f  %r @%.0f,%.0f vs rect(%.0f,%.0f,%.0f,%.0f)"%(ix,iy,t[4],t[0],t[1]+12,r[0],r[1],r[2]-r[0],r[3]-r[1]))
print("OVERLAPS: %d"%len(prob))
for x in sorted(set(prob)): print("  -",x)
