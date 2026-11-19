import os
import os, subprocess, numpy as np, sys
from PIL import Image
base='' + os.environ.get('CC_CAPTURES', '../../../Solarium/captures/27.0-arm64-cc/') + ''
bg=np.array(Image.open(base+'background.png').convert('RGBA')); raw=np.zeros_like(bg); raw[...,0]=bg[...,2]; raw[...,1]=bg[...,1]; raw[...,2]=bg[...,0]; raw[...,3]=bg[...,3]; raw.tofile('/tmp/fit2_in.raw')
real=np.array(Image.open(base+'cc-plain.png').convert('RGB')).astype(float)
PANEL=(116,46,280,420,37,9)
TILES=[(126,59,130,63,31,10),(126,131,130,63,31,10),(264,59,122,135,24,10),(126,203,63,63,31,10),(193,203,63,63,31,10),(264,203,122,63,31,10),(126,279,260,63,24,10),(126,351,260,63,24,10)]
def render(params, shapes):
    e=dict(os.environ); e['WSD_APPEARANCE']='dark'; e.update({k:str(v) for k,v in params.items()})
    subprocess.check_call(['./calib2','/tmp/fit2_in.raw','512','512','/tmp/fit2_out.raw']+['%d,%d,%d,%d,%d,%d'%s for s in shapes],env=e)
    return np.fromfile('/tmp/fit2_out.raw',dtype=np.uint8).reshape(512,512,4)[...,[2,1,0]].astype(float)
def sdf_of(s):
    x,y,w,h,r,m=s; yy,xx=np.mgrid[0:512,0:512]; cx,cy=x+w/2,y+h/2
    qx=np.abs(xx+0.5-cx)-(w/2-r); qy=np.abs(yy+0.5-cy)-(h/2-r)
    return np.hypot(np.maximum(qx,0),np.maximum(qy,0))+np.minimum(np.maximum(qx,qy),0)-r
def masks():
    inside_tiles=np.zeros((512,512),bool)
    for t in TILES: inside_tiles|=sdf_of(t)<-3
    near_tiles=np.zeros((512,512),bool)
    for t in TILES: near_tiles|=sdf_of(t)<6
    panel=(sdf_of(PANEL)<-3)&~near_tiles
    return inside_tiles, panel
IT,PM=masks()
def score(params):
    o=render(params,[PANEL]+TILES); d=np.abs(o-real).mean(axis=2)
    return d[IT].mean(), d[PM].mean(), o
if __name__=='__main__':
    t,p,o=score({}); print('tiles interior',round(t,2),'panel only',round(p,2)); Image.fromarray(o.astype(np.uint8)).save('/tmp/fit2_ours.png')
