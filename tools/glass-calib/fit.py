import os
import subprocess, os, sys, random, numpy as np
from PIL import Image
base='' + os.environ.get('SOLARIUM_CAPTURES', '../../../Solarium/captures/27.0-arm64/') + ''
bg=np.array(Image.open(base+'background.png').convert('RGBA')); raw=np.zeros_like(bg); raw[...,0]=bg[...,2]; raw[...,1]=bg[...,1]; raw[...,2]=bg[...,0]; raw[...,3]=bg[...,3]
raw.tofile('/tmp/fit_in.raw')
SC={'A':('regular-340x200-r60.png',(86,156,340,200),60,9),'B':('regular-200x200-r100.png',(156,156,200,200),100,9),'C':('regular-120x120-r60.png',(196,196,120,120),60,10)}
REAL={k:np.array(Image.open(base+v[0]).convert('RGB')).astype(float) for k,v in SC.items()}
def score(params, keys, tag):
    e=dict(os.environ); e['WSD_APPEARANCE']='dark'; e.update({k:str(v) for k,v in params.items()})
    tot=0
    for k in keys:
        name,rect,rad,mode=SC[k]; x,y,w,h=rect
        subprocess.check_call(['./calib','/tmp/fit_in.raw','512','512',*map(str,rect),str(rad),str(mode),'/tmp/fit_out_%s.raw'%tag],env=e)
        o=np.fromfile('/tmp/fit_out_%s.raw'%tag,dtype=np.uint8).reshape(512,512,4)[...,[2,1,0]].astype(float)
        m=np.zeros((512,512),bool); m[y+2:y+h-2,x+2:x+w-2]=True; m[236:276,216:296]=False   # the "Glass" label
        tot+=np.abs(o-REAL[k]).mean(axis=2)[m].mean()
    return tot/len(keys)
def fit(keys, space, start, iters=3):
    cur=dict(start); best=score(cur,keys,'f'); print('start',round(best,2))
    for it in range(iters):
        for name,(lo,hi) in space.items():
            for f in (0.6,0.8,0.9,1.1,1.25,1.6):
                trial=dict(cur); v=cur[name]*f if cur[name]!=0 else (hi-lo)*0.05*(f-0.5)
                trial[name]=min(hi,max(lo,v)); s=score(trial,keys,'f')
                if s<best-1e-3: best=s; cur=trial
        print('iter',it,round(best,2),{k:round(v,3) for k,v in cur.items()})
    return cur,best
if __name__=='__main__':
    which=sys.argv[1]
    if which=='9':
        space={'WS_G9_SIGMA':(2,20),'WS_G9_SAT':(0.6,1.8),'WS_G9_DGAIN':(0.3,1.2),'WS_G9_DLIFT':(-0.05,0.4),'WS_G9_REFR':(0,1.5),'WS_G9_RIM':(0,2.5),'WS_G9_RING':(0.0,0.6),'WS_G9_RINGW':(4,40),'WS_G9_RH':(8,40)}
        start={'WS_G9_SIGMA':14,'WS_G9_SAT':1.3,'WS_G9_DGAIN':0.8,'WS_G9_DLIFT':0.03,'WS_G9_REFR':0.7,'WS_G9_RIM':1.0,'WS_G9_RING':0.05,'WS_G9_RINGW':14,'WS_G9_RH':20}
        fit(['A','B'],space,start)
    else:
        space={'WS_G9_SIGMA':(2,20),'WS_G9_SAT':(0.6,1.8),'WS_G10_DGAIN':(0.3,1.2),'WS_G10_DLIFT':(-0.05,0.4),'WS_G9_REFR':(0,1.5),'WS_G9_RIM':(0,2.5),'WS_G9_RING':(0.0,0.6),'WS_G9_RINGW':(4,40),'WS_G9_RH':(8,40)}
        start={'WS_G9_SIGMA':14,'WS_G9_SAT':1.3,'WS_G10_DGAIN':0.78,'WS_G10_DLIFT':0.09,'WS_G9_REFR':0.7,'WS_G9_RIM':1.0,'WS_G9_RING':0.05,'WS_G9_RINGW':14,'WS_G9_RH':20}
        fit(['C'],space,start)
