import fit2, time
space={'WS_GS_SIGMA':(0.5,6),'WS_G9_SMIX':(0,1),'WS_G10_SMIX':(0,1),'WS_GS_DEPTH':(2,60),'WS_G9_SIGMA':(2,30),'WS_G9_SAT':(0.6,1.8),'WS_G9_DGAIN':(0.3,1.2),'WS_G9_DLIFT':(-0.05,0.4),'WS_G9_RING':(0.0,0.6),'WS_G9_RINGW':(4,40),'WS_G9_REFR':(0,1.5),'WS_G9_RIM':(0,2.5),
       'WS_G10_SIGMA':(1,30),'WS_G10_SAT':(0.6,2.0),'WS_G10_DGAIN':(0.3,1.2),'WS_G10_DLIFT':(-0.05,0.4),'WS_G10_RING':(0.0,0.6),'WS_G10_RINGW':(2,40),'WS_G10_RIM':(0,2.5)}
cur={'WS_GS_SIGMA':2.5,'WS_G9_SMIX':0.5,'WS_G10_SMIX':0.5,'WS_GS_DEPTH':18,'WS_G9_SIGMA':8.67,'WS_G9_SAT':1.3,'WS_G9_DGAIN':0.88,'WS_G9_DLIFT':0.05,'WS_G9_RING':0.154,'WS_G9_RINGW':39.2,'WS_G9_REFR':0.755,'WS_G9_RIM':0.829,'WS_G10_SIGMA':12,'WS_G10_SAT':1.43,'WS_G10_DGAIN':0.78,'WS_G10_DLIFT':0.099,'WS_G10_RING':0.05,'WS_G10_RINGW':4,'WS_G10_RIM':0.12}
def total(p):
    t,pn,_=fit2.score(p); return t+pn
best=total(cur); print('start',round(best,2),flush=True)
for it in range(3):
    for name,(lo,hi) in space.items():
        for f in (0.7,0.85,1.15,1.4):
            trial=dict(cur); trial[name]=min(hi,max(lo,cur[name]*f)); s=total(trial)
            if s<best-1e-3: best=s; cur=trial
    t,pn,_=fit2.score(cur); print('iter',it,round(best,2),'tiles',round(t,2),'panel',round(pn,2),{k:round(v,3) for k,v in cur.items()},flush=True)
