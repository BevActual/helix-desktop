"""How much of the screen a surface painted: prints the share of pixels that differ from the background in a top strip
and in the centre. Catches the blank-surface failure that a window listing cannot (mapped but painting nothing).
  coverage.py <screenshot.png> <label>"""
import sys
from PIL import Image
im=Image.open(sys.argv[1]).convert("RGB"); w,h=im.size
bg=im.getpixel((w//2,h-5))
def cover(box):
    c=im.crop(box); px=list(c.getdata()); n=sum(1 for p in px if sum(abs(a-b) for a,b in zip(p,bg))>12); return 100*n/len(px)
print(f"{sys.argv[2]:28s} bar-strip {cover((0,0,w,60)):5.1f}%   centre-panel {cover((int(w*.35),int(h*.2),int(w*.65),int(h*.55))):5.1f}%")
