import os, subprocess, numpy as np
from PIL import Image
L='' + os.environ.get('LIGHT_CAPTURES', '../../../Solarium/captures/27.0-arm64-light/') + ''
def raw(path,out):
    bg=np.array(Image.open(path).convert('RGBA')); r=np.zeros_like(bg); r[...,0]=bg[...,2]; r[...,1]=bg[...,1]; r[...,2]=bg[...,0]; r[...,3]=bg[...,3]; r.tofile(out)
raw(L+'glass-probe/background.png','/tmp/f3_gp.raw'); raw(L+'cc/background.png','/tmp/f3_cc.raw')
def env(params):
    e=dict(os.environ); e['WSD_APPEARANCE']='light'; e.update({k:str(v) for k,v in params.items()}); return e
SC={'A':('regular-340x200-r60.png',(86,156,340,200),60,9),'B':('regular-200x200-r100.png',(156,156,200,200),100,9),'C':('regular-120x120-r60.png',(196,196,120,120),60,10)}
REAL={k:np.array(Image.open(L+'glass-probe/'+v[0]).convert('RGB')).astype(float) for k,v in SC.items()}
CCREAL=np.array(Image.open(L+'cc/cc-plain.png').convert('RGB')).astype(float)
PANEL=(116,46,280,420,37,9)
TILES=[(126,59,130,63,31,10),(126,131,130,63,31,10),(264,59,122,135,24,10),(126,203,63,63,31,10),(193,203,63,63,31,10),(264,203,122,63,31,10),(126,279,260,63,24,10),(126,351,260,63,24,10)]
def sdf_of(s):
    x,y,w,h,r,m=s; yy,xx=np.mgrid[0:512,0:512]; cx,cy=x+w/2,y+h/2
    qx=np.abs(xx+0.5-cx)-(w/2-r); qy=np.abs(yy+0.5-cy)-(h/2-r)
    return np.hypot(np.maximum(qx,0),np.maximum(qy,0))+np.minimum(np.maximum(qx,qy),0)-r
IT=np.zeros((512,512),bool); NT=np.zeros((512,512),bool)
for t in TILES: IT|=sdf_of(t)<-3; NT|=sdf_of(t)<6
PM=(sdf_of(PANEL)<-3)&~NT
def run1(k,params):
    name,rect,rad,mode=SC[k]; x,y,w,h=rect
    subprocess.check_call(['./calib','/tmp/f3_gp.raw','512','512',*map(str,rect),str(rad),str(mode),'/tmp/f3_o.raw'],env=env(params))
    o=np.fromfile('/tmp/f3_o.raw',dtype=np.uint8).reshape(512,512,4)[...,[2,1,0]].astype(float)
    m=np.zeros((512,512),bool); m[y+2:y+h-2,x+2:x+w-2]=True; m[236:276,216:296]=False
    return np.abs(o-REAL[k]).mean(axis=2)[m].mean()
def runcc(params):
    subprocess.check_call(['./calib2','/tmp/f3_cc.raw','512','512','/tmp/f3_c.raw']+['%d,%d,%d,%d,%d,%d'%s for s in [PANEL]+TILES],env=env(params))
    o=np.fromfile('/tmp/f3_c.raw',dtype=np.uint8).reshape(512,512,4)[...,[2,1,0]].astype(float); d=np.abs(o-CCREAL).mean(axis=2)
    return d[IT].mean(), d[PM].mean(), o
def total(params):
    a=run1('A',params); b=run1('B',params); t,p,_=runcc(params); return a+b+t+p,(a,b,t,p)
if __name__=='__main__':
    space={'WS_G9_SIGMA_L':(2,30),'WS_G9_SAT_L':(0.4,2.0),'WS_G9_LGAIN_L':(0.3,2.2),'WS_G9_LLIFT_L':(-0.3,0.9),'WS_G9_RING_L':(0,0.6),'WS_G9_RINGW_L':(4,40),'WS_G9_REFR_L':(0,1.5),'WS_G9_RIM_L':(0,3),
           'WS_G10_SIGMA_L':(1,30),'WS_G10_SAT_L':(0.4,2.0),'WS_G10_LGAIN_L':(0.3,2.2),'WS_G10_LLIFT_L':(-0.3,0.9),'WS_G10_RING_L':(0,0.6),'WS_G10_RINGW_L':(2,40),'WS_G10_RIM_L':(0,3),
           'WS_GS_SIGMA_L':(0.5,8),'WS_G9_SMIX_L':(0,1),'WS_G10_SMIX_L':(0,1),'WS_GS_DEPTH_L':(2,60),'WS_G9_SH_L':(0,0.8)}
    cur={'WS_G9_SIGMA_L':10.983,'WS_G9_SAT_L':1.245,'WS_G9_LGAIN_L':1.3,'WS_G9_LLIFT_L':0.184,'WS_G9_RING_L':0.121,'WS_G9_RINGW_L':40,'WS_G9_REFR_L':1.0,'WS_G9_RIM_L':2.555,
         'WS_G10_SIGMA_L':20.4,'WS_G10_SAT_L':1.4,'WS_G10_LGAIN_L':0.92,'WS_G10_LLIFT_L':0.2,'WS_G10_RING_L':0.083,'WS_G10_RINGW_L':9.6,'WS_G10_RIM_L':1.294,
         'WS_GS_SIGMA_L':8,'WS_G9_SMIX_L':0.103,'WS_G10_SMIX_L':0.101,'WS_GS_DEPTH_L':26.7,'WS_G9_SH_L':0.196}
    best,parts=total(cur); print('start',round(best,2),[round(x,2) for x in parts],flush=True)
    for it in range(3):
        for name,(lo,hi) in space.items():
            for f in (0.7,0.85,1.15,1.4):
                trial=dict(cur); trial[name]=min(hi,max(lo,cur[name]*f if cur[name] else 0.02)); s,pp=total(trial)
                if s<best-1e-3: best,parts,cur=s,pp,trial
        print('iter',it,round(best,2),[round(x,2) for x in parts],{k:round(v,3) for k,v in cur.items()},flush=True)
