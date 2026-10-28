"""Layout audit for docs/woods-backplate-wiring.svg: text that overflows or
escapes its enclosing box. Run before/after an edit and DIFF the two outputs -
the absolute list has known false positives (char widths are estimated), but
anything NEW is almost always something you just broke.

  python3 tools/chk.py docs/woods-backplate-wiring.svg
"""
import re,io,sys
s=io.open(sys.argv[1],encoding='utf-8').read()
CW={'tt':9.9,'t':6.8,'ts':5.6,'tw':6.9,'tk':5.9,'note':5.6,'pin':6.5,'pinno':5.2,'wtag':5.6,'nc':6.3}
boxes=[(int(m.group(1)),int(m.group(2)),int(m.group(3)),int(m.group(4)),m.group(5))
       for m in re.finditer(r'<rect x="(\d+)" y="(\d+)" width="(\d+)" height="(\d+)" class="(box|chk|warn|pwr|hdr|rf|gps)"',s)]
texts=[]
for m in re.finditer(r'<text x="(\d+)" y="(\d+)" class="(\w+)"[^>]*>(.*?)</text>',s,re.S):
    x,y,c=int(m.group(1)),int(m.group(2)),m.group(3)
    body=re.sub(r'<[^>]*>','',m.group(4))
    texts.append((x,y,c,body,x+len(body)*CW.get(c,6)))
out=[]
for (bx,by,bw,bh,cls) in boxes:
    for (x,y,c,body,xe) in texts:
        if bx<x<bx+bw and by<y<by+bh and xe>bx+bw-4:
            out.append("OVERFLOW-X %s(%d,%d,%d) @%d,%d ends%d %r"%(cls,bx,by,bw,x,y,int(xe),body[:40]))
        if bx<x<bx+bw and y>by+bh and y<by+bh+40:
            out.append("BELOW-BOX %s(%d,%d,%d,%d) @%d,%d %r"%(cls,bx,by,bw,bh,x,y,body[:40]))
print("\n".join(sorted(out)))
