import os
import subprocess, sys, numpy as np
from PIL import Image
base='' + os.environ.get('SOLARIUM_CAPTURES', '../../../Solarium/captures/27.0-arm64/') + ''
def run(name, rect, radius, mode, env=None):
    bg=Image.open(base+'background.png').convert('RGBA'); a=np.array(bg)
    # to premultiplied BGRA raw
    raw=np.zeros_like(a); raw[...,0]=a[...,2]; raw[...,1]=a[...,1]; raw[...,2]=a[...,0]; raw[...,3]=a[...,3]
    raw.tofile('/tmp/calib_in.raw')
    import os
    e=dict(os.environ); e.update(env or {})
    subprocess.check_call(['./calib','/tmp/calib_in.raw','512','512']+[str(x) for x in rect]+[str(radius),str(mode),'/tmp/calib_out.raw'], env=e)
    o=np.fromfile('/tmp/calib_out.raw',dtype=np.uint8).reshape(512,512,4)
    out=np.zeros_like(o); out[...,0]=o[...,2]; out[...,1]=o[...,1]; out[...,2]=o[...,0]; out[...,3]=255
    real=np.array(Image.open(base+name).convert('RGBA'))
    x,y,w,h=rect; m=np.zeros((512,512),bool); m[y+8:y+h-8, x+8:x+w-8]=True
    d=np.abs(out[...,:3].astype(float)-real[...,:3].astype(float)).mean(axis=2)
    return out, real, d[m].mean(), d[y:y+h,x:x+w].mean()
if __name__=='__main__':
    out,real,dm,dall=run('regular-340x200-r60.png',(86,156,340,200),60,int(sys.argv[1]) if len(sys.argv)>1 else 9)
    print('mean abs diff inside (interior, whole box):',round(dm,1),round(dall,1))
    Image.fromarray(out).save('/tmp/calib_ours.png'); Image.fromarray(real).save('/tmp/calib_real.png')
    both=Image.new('RGB',(1024,512)); both.paste(Image.fromarray(out[...,:3]),(0,0)); both.paste(Image.fromarray(real[...,:3]),(512,0)); both.save('/tmp/calib_both.png')
