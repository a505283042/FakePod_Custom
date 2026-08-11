from pathlib import Path
import math
import numpy as np
from PIL import Image, ImageDraw

W=H=340
SS=4
CX=CY=170
FRAME_COUNT=12
PROGRESS_MAX=1000
COLLAPSED_OUTER=82
OUTER=150
COLLAPSED_WIDTH=20
RING_WIDTH=70
COLLAPSED_ICON_R=24
ICON_R=115
HALF_SPAN=23
OUTER_ICON_DELAY=140
# icon kind, angle, optical x/y, unit x/y x10000
ITEMS=[
('Music',180,-5,0,0,-10000),
('Nsf',129,0,0,7771,-6293),
('MicSpectrum',77,-2,0,9744,2250),
('Mjpg',26,0,0,4384,8988),
('Picture',334,0,0,-4384,8988),
('Ebook',283,0,0,-9744,2250),
('Settings',231,0,0,-7771,-6293),
]

# high-res coordinates at subpixel centers, in base-pixel units
ys=(np.arange(H*SS)+0.5)/SS
xs=(np.arange(W*SS)+0.5)/SS
X,Y=np.meshgrid(xs,ys)
DX=X-CX
DY=Y-CY
R=np.sqrt(DX*DX+DY*DY)
ANG=(np.degrees(np.arctan2(DX,DY))%360.0)

def angle_dist(a,b):
    d=np.abs((a-b+180)%360-180)
    return d

def lerp(p,a,b):
    return a + ((b-a)*p + PROGRESS_MAX//2)//PROGRESS_MAX

def scaled(v, pct):
    # C++ formula rounds away-ish with sign-specific offset.
    return int((v*pct + (50 if v>=0 else -50))/100)

def draw_icon(mask, kind, cx, cy, scale_percent):
    # draw white icon into high-res grayscale image, matching current line/dot geometry.
    img=Image.fromarray(mask, mode='L')
    d=ImageDraw.Draw(img)
    def s(v):
        q=scaled(v,scale_percent)
        if v!=0 and q==0:
            return 1 if v>0 else -1
        return q
    line_w=4 if scale_percent>=120 else (3 if scale_percent>=70 else 2)
    def line(x0,y0,x1,y1):
        pts=[((cx+s(x0))*SS,(cy+s(y0))*SS),((cx+s(x1))*SS,(cy+s(y1))*SS)]
        d.line(pts, fill=255, width=max(1,line_w*SS))
        # emulate round caps
        rr=line_w*SS/2
        for px,py in pts:
            d.ellipse((px-rr,py-rr,px+rr,py+rr),fill=255)
    def dot(x,y,diam):
        dd=max(3,s(diam))
        ccx=(cx+s(x))*SS; ccy=(cy+s(y))*SS; rr=(dd*SS)/2
        d.ellipse((ccx-rr,ccy-rr,ccx+rr,ccy+rr),fill=255)
    if kind=='Music':
        line(4,-17,4,9); line(4,-17,16,-20); line(16,-20,16,4); dot(-2,10,10); dot(10,5,10)
    elif kind=='Nsf':
        for a in [(-13,-13,13,-13),(13,-13,13,13),(13,13,-13,13),(-13,13,-13,-13),
                  (-8,-18,-8,-13),(0,-18,0,-13),(8,-18,8,-13),(-8,13,-8,18),(0,13,0,18),(8,13,8,18),
                  (-18,-8,-13,-8),(-18,0,-13,0),(-18,8,-13,8),(13,-8,18,-8),(13,0,18,0),(13,8,18,8),
                  (-7,4,-2,-4),(-2,-4,3,4),(3,4,8,-4)]: line(*a)
    elif kind=='MicSpectrum':
        for a in [(-11,-14,-11,6),(-11,-14,-4,-18),(-4,-18,3,-14),(3,-14,3,6),(3,6,-4,10),(-4,10,-11,6),
                  (-15,4,-15,7),(-15,7,-9,13),(-9,13,-4,14),(-4,14,2,12),(-4,14,-4,19),(-10,19,2,19),
                  (8,10,8,17),(13,4,13,17),(18,-3,18,17)]: line(*a)
    elif kind=='Mjpg':
        for a in [(-18,-13,11,-13),(11,-13,11,13),(11,13,-18,13),(-18,13,-18,-13),(11,-7,18,-12),(18,-12,18,12),(18,12,11,7),(-6,-7,-6,7),(-6,-7,5,0),(5,0,-6,7)]: line(*a)
    elif kind=='Picture':
        for a in [(-18,-15,18,-15),(18,-15,18,15),(18,15,-18,15),(-18,15,-18,-15),(-14,10,-5,0),(-5,0,1,6),(1,6,8,-4),(8,-4,15,10)]: line(*a)
        dot(9,-9,6)
    elif kind=='Ebook':
        for a in [(0,-14,0,15),(-1,-12,-7,-15),(-7,-15,-18,-12),(-18,-12,-18,12),(-18,12,-7,10),(-7,10,-1,13),(1,-12,7,-15),(7,-15,18,-12),(18,-12,18,12),(18,12,7,10),(7,10,1,13)]: line(*a)
    elif kind=='Settings':
        dot(0,0,10)
        for a in [(0,-18,0,-11),(0,11,0,18),(-18,0,-11,0),(11,0,18,0),(-13,-13,-8,-8),(8,8,13,13),(13,-13,8,-8),(-8,8,-13,13)]: line(*a)
    return np.array(img,dtype=np.uint8)

def frame_indices(p):
    out=np.zeros((H,W),dtype=np.uint8)
    if p<=0:
        return out
    outer=lerp(p,COLLAPSED_OUTER,OUTER)
    width=lerp(p,COLLAPSED_WIDTH,RING_WIDTH)
    inner=outer-width
    # highres sector label + coverage downsample
    # process each sector independently because gaps ensure no overlap.
    for i,(_,ang,*_) in enumerate(ITEMS):
        hi=((R>=inner)&(R<=outer)&(angle_dist(ANG,ang)<=HALF_SPAN)).astype(np.uint8)
        cov=hi.reshape(H,SS,W,SS).mean(axis=(1,3))
        # AA level index is 8+i, opaque is 1+i
        aa=(cov>0.0)&(cov<0.72)
        solid=cov>=0.72
        out[aa]=8+i
        out[solid]=1+i
    # outer icons, same animation geometry as R.32
    ip=0 if p<=OUTER_ICON_DELAY else ((p-OUTER_ICON_DELAY)*PROGRESS_MAX)//(PROGRESS_MAX-OUTER_ICON_DELAY)
    if ip>0:
        icon_r=lerp(p,COLLAPSED_ICON_R,ICON_R)
        scale=62+(38*ip)//PROGRESS_MAX
        icon_mask=np.zeros((H*SS,W*SS),dtype=np.uint8)
        for kind,ang,optx,opty,ux,uy in ITEMS:
            ix=CX + int((ux*icon_r + (5000 if ux>=0 else -5000))/10000) + (optx*ip)//PROGRESS_MAX
            iy=CY + int((uy*icon_r + (5000 if uy>=0 else -5000))/10000) + (opty*ip)//PROGRESS_MAX
            icon_mask=draw_icon(icon_mask,kind,ix,iy,scale)
        cov=icon_mask.reshape(H,SS,W,SS).mean(axis=(1,3))/255.0
        out[cov>=0.18]=15
    return out

def pack_i4(idx):
    assert idx.shape==(H,W)
    a=idx[:,0::2]
    b=idx[:,1::2]
    return ((a<<4)|b).astype(np.uint8).tobytes()

def packbits(data: bytes)->bytes:
    out=bytearray(); n=len(data); i=0
    while i<n:
        # detect run
        run=1
        while i+run<n and run<128 and data[i+run]==data[i]: run+=1
        if run>=3:
            out.append(0x80|(run-1)); out.append(data[i]); i+=run; continue
        # literal until next >=3 run or 128 bytes
        start=i; i+=run
        while i<n and i-start<128:
            run=1
            while i+run<n and run<128 and data[i+run]==data[i]: run+=1
            if run>=3: break
            i+=run
        ln=i-start
        out.append(ln-1); out.extend(data[start:i])
    return bytes(out)

def unpackbits(comp:bytes,expected:int)->bytes:
    out=bytearray(); i=0
    while i<len(comp):
        c=comp[i]; i+=1; ln=(c&0x7f)+1
        if c&0x80:
            v=comp[i]; i+=1; out.extend([v]*ln)
        else:
            out.extend(comp[i:i+ln]); i+=ln
    assert len(out)==expected,(len(out),expected)
    return bytes(out)

frames=[]
for fi in range(FRAME_COUNT):
    p=round(fi*PROGRESS_MAX/(FRAME_COUNT-1))
    raw=pack_i4(frame_indices(p))
    comp=packbits(raw)
    assert unpackbits(comp,len(raw))==raw
    frames.append((p,raw,comp))
    print(fi,p,len(raw),len(comp),f'{100*len(comp)/len(raw):.1f}%')

outdir=Path(__file__).resolve().parents[1] / 'src' / 'assets'
h=outdir/'launcher_animation_frames.h'
cpp=outdir/'launcher_animation_frames.cpp'
h.write_text('''#pragma once\n\n#include <stddef.h>\n#include <stdint.h>\n\nstruct LauncherAnimationFrameAsset {\n    const uint8_t *data;\n    uint32_t size;\n    uint16_t progress;\n};\n\nstatic constexpr uint16_t kLauncherAnimationAssetWidth = 340;\nstatic constexpr uint16_t kLauncherAnimationAssetHeight = 340;\nstatic constexpr uint16_t kLauncherAnimationAssetStride = 170;\nstatic constexpr uint32_t kLauncherAnimationAssetPixelBytes = 57800;\nstatic constexpr uint8_t kLauncherAnimationAssetFrameCount = 12;\n\nextern const LauncherAnimationFrameAsset g_launcher_animation_frames[kLauncherAnimationAssetFrameCount];\n''',encoding='utf-8')
lines=['#include "launcher_animation_frames.h"','', '// Generated by tools/generate_launcher_animation_frames.py.', '// Flash-resident PackBits-compressed LV_COLOR_FORMAT_I4 pixel templates.', '']
for i,(p,raw,comp) in enumerate(frames):
    lines.append(f'static const uint8_t kLauncherFrame{i:02d}[] = {{')
    for j in range(0,len(comp),20):
        chunk=comp[j:j+20]
        lines.append('    '+', '.join(f'0x{x:02X}' for x in chunk)+',')
    lines.append('};\n')
lines.append('const LauncherAnimationFrameAsset g_launcher_animation_frames[kLauncherAnimationAssetFrameCount] = {')
for i,(p,raw,comp) in enumerate(frames):
    lines.append(f'    {{kLauncherFrame{i:02d}, sizeof(kLauncherFrame{i:02d}), {p}}},')
lines.append('};\n')
cpp.write_text('\n'.join(lines),encoding='utf-8')
print('total compressed',sum(len(c) for _,_,c in frames))
