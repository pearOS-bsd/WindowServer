import os, subprocess, numpy as np, sys
from PIL import Image
G='/home/alxb421/.claude/jobs/b6dee681/tmp/glassref/'
T=G+'tone/'
LIGHT=sys.argv[1]=='l'
sfx='_L' if LIGHT else ''
# scene C of the Solarium captures (dark) or of the light captures
if LIGHT:
    BG=G+'light/gp/background.png'; CREAL=G+'light/gp/regular-120x120-r60.png'
else:
    BG='/home/alxb421/pearOS/Solarium/captures/27.0-arm64/background.png'; CREAL='/home/alxb421/pearOS/Solarium/captures/27.0-arm64/regular-120x120-r60.png'
bg=np.array(Image.open(BG).convert('RGBA')); r=np.zeros_like(bg); r[...,0]=bg[...,2]; r[...,1]=bg[...,1]; r[...,2]=bg[...,0]; r[...,3]=bg[...,3]; r.tofile('/tmp/f4_c.raw')
creal=np.array(Image.open(CREAL).convert('RGB')).astype(float)
def env(p):
    e=dict(os.environ); e['WSD_APPEARANCE']='light' if LIGHT else 'dark'; e.update({k+sfx:str(v) for k,v in p.items()}); return e
def toneerr(p):
    errs=[]
    for nm in ['grey064','grey192','col0','col1','col3','col7']:
        im=np.array(Image.open(T+('l' if LIGHT else 'd')+'/'+nm+'.png').convert('RGB')).astype(float); bgc=im[5,5]
        W,H=300,200; a=np.zeros((H,W,4),np.uint8); a[...,0]=bgc[2]; a[...,1]=bgc[1]; a[...,2]=bgc[0]; a[...,3]=255; a.tofile('/tmp/f4_t.raw')
        subprocess.check_call(['./calib','/tmp/f4_t.raw',str(W),str(H),'20','67','140','63','31','10','/tmp/f4_o.raw'],env=env(p))
        o=np.fromfile('/tmp/f4_o.raw',dtype=np.uint8).reshape(H,W,4)[...,[2,1,0]].astype(float)
        # the whole pill interior (not the rim): a box 100 x 25
        errs.append(np.abs(o[80:114,45:135].reshape(-1,3).mean(axis=0)-im[80:114,45:135].reshape(-1,3).mean(axis=0)).mean())
    return float(np.mean(errs))
def cerr(p):
    subprocess.check_call(['./calib','/tmp/f4_c.raw','512','512','196','196','120','120','60','10','/tmp/f4_o2.raw'],env=env(p))
    o=np.fromfile('/tmp/f4_o2.raw',dtype=np.uint8).reshape(512,512,4)[...,[2,1,0]].astype(float)
    m=np.zeros((512,512),bool); m[198:314,198:314]=True; m[236:276,216:296]=False; return np.abs(o-creal).mean(axis=2)[m].mean()
def total(p): return toneerr(p)*2+cerr(p)
space={'WS_G10_SIGMA':(1,30),'WS_G10_K':(0.8,1.6),'WS_G10_RING':(0,0.6),'WS_G10_RINGW':(2,40),'WS_G10_RIM':(0,3),'WS_GS_SIGMA':(0.5,8),'WS_G10_SMIX':(0,1),'WS_GS_DEPTH':(2,60),'WS_G9_REFR':(0,1.5)}
cur={'WS_G10_SIGMA':24 if not LIGHT else 20.4,'WS_G10_K':1.1,'WS_G10_RING':0.028 if not LIGHT else 0.134,'WS_G10_RINGW':2.3 if not LIGHT else 20.4,'WS_G10_RIM':0.31 if not LIGHT else 2.5,'WS_GS_SIGMA':3.93 if not LIGHT else 1.98,'WS_G10_SMIX':0.48 if not LIGHT else 0.021,'WS_GS_DEPTH':10.3 if not LIGHT else 5.6,'WS_G9_REFR':0.87 if not LIGHT else 1.0}
best=total(cur); print('start',round(best,2),'tone',round(toneerr(cur),2),'C',round(cerr(cur),2),flush=True)
for it in range(3):
    for name,(lo,hi) in space.items():
        for f in (0.6,0.8,1.25,1.6):
            t=dict(cur); t[name]=min(hi,max(lo,cur[name]*f if cur[name] else 0.02)); s=total(t)
            if s<best-1e-3: best,cur=s,t
    print('iter',it,round(best,2),'tone',round(toneerr(cur),2),'C',round(cerr(cur),2),{k:round(v,3) for k,v in cur.items()},flush=True)
