import os, subprocess, numpy as np
from PIL import Image
R=os.environ.get('REAL_CC', '../../../Solarium/captures/27.0-arm64-cc-real/')
bg=np.array(Image.open(R+'background-region.png').convert('RGBA')); H,W=bg.shape[:2]
r=np.zeros_like(bg); r[...,0]=bg[...,2]; r[...,1]=bg[...,1]; r[...,2]=bg[...,0]; r[...,3]=255; r.tofile('/tmp/f5_in.raw')
REAL=np.array(Image.open(R+'panel-region.png').convert('RGB')).astype(float)
# glass tiles only (x,y,w,h,r): the white/blue active buttons are filled shapes, not glass
T=[(250,118,64,64,32),(174,194,140,64,32),(326,194,64,64,32),(402,194,64,64,32),(174,271,64,64,32),(250,271,64,64,32),(326,271,140,64,32),(174,346,292,64,32),(250,423,140,64,32),(326,499,140,64,32),(326,575,140,64,32),(326,41,140,140,36),(174,499,140,140,36)]
yy,xx=np.mgrid[0:H,0:W]
def sdf(t):
    x,y,w,h,r=t; cx,cy=x+w/2,y+h/2; qx=np.abs(xx+0.5-cx)-(w/2-r); qy=np.abs(yy+0.5-cy)-(h/2-r)
    return np.hypot(np.maximum(qx,0),np.maximum(qy,0))+np.minimum(np.maximum(qx,qy),0)-r
INT=np.zeros((H,W),bool); NEAR=np.zeros((H,W),bool)
for t in T: INT|=sdf(t)<-4; NEAR|=sdf(t)<8
# the text / icons: pixels brighter than the glass around by a lot are labels: ignore the brightest 12 % of the real pixels of the interiors
def env(p):
    e=dict(os.environ); e['WSD_APPEARANCE']='dark'; e.update({k:str(v) for k,v in p.items()}); return e
def render(p):
    subprocess.check_call(['./calib2','/tmp/f5_in.raw',str(W),str(H),'/tmp/f5_out.raw']+['%d,%d,%d,%d,%d,10'%t for t in T],env=env(p))
    return np.fromfile('/tmp/f5_out.raw',dtype=np.uint8).reshape(H,W,4)[...,[2,1,0]].astype(float)
def score(p):
    o=render(p); d=np.abs(o-REAL).mean(axis=2); m=INT.copy()
    return d[m].mean()
if __name__=='__main__':
    space={'WS_G10_SIGMA':(0.5,30),'WS_G10_K':(0.5,2.0),'WS_G10_RING':(0,0.6),'WS_G10_RINGW':(1,30),'WS_G10_RIM':(0,3),'WS_GS_SIGMA':(0.5,8),'WS_G10_SMIX':(0,1),'WS_GS_DEPTH':(2,60),'WS_G10_REFR':(0,1.5),'WS_G10_LGAIN':(0.3,2.2),'WS_G10_LLIFT':(-0.4,0.9),'WS_G10_SAT':(0.4,2.5)}
    cur={'WS_G10_SIGMA':6.91,'WS_G10_K':1.1,'WS_G10_RING':0.224,'WS_G10_RINGW':5.75,'WS_G10_RIM':0.143,'WS_GS_SIGMA':3.773,'WS_G10_SMIX':0.8,'WS_GS_DEPTH':10.3,'WS_G10_REFR':1.087,'WS_G10_LGAIN':0.8,'WS_G10_LLIFT':0.2,'WS_G10_SAT':1.43}
    best=score(cur); print('start',round(best,2),flush=True)
    o=render(cur); Image.fromarray(np.hstack([o,REAL]).astype(np.uint8)).save('/tmp/f5_start.png')
    for it in range(3):
        for n,(lo,hi) in space.items():
            for f in (0.5,0.75,1.3,1.8):
                t=dict(cur); t[n]=min(hi,max(lo,cur[n]*f if cur[n] else 0.02)); s=score(t)
                if s<best-1e-3: best,cur=s,t
        print('iter',it,round(best,2),{k:round(v,3) for k,v in cur.items()},flush=True)
    o=render(cur); Image.fromarray(np.hstack([o,REAL]).astype(np.uint8)).save('/tmp/f5_end.png')
