import sys, os, math
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import numpy as np
from _tempo5_lock_sim import RATE, odf_of, STYLES, TRUTHS, bin_range, _parabolic, tie_break
BS=list(range(2,9)); LO,HI=90.0,175.0; RAY=60.0*RATE/1024.0
def spec(odf, nfft=4096, L=1024):
    x=odf[-L:].astype(np.float64); x-=x.mean()
    X=np.fft.rfft(x,nfft)
    kmin,kmax=bin_range(LO,HI,nfft)
    mags=np.abs(X[kmin:kmax+1]); sb=np.arange(kmin,kmax+1)*RATE/nfft*60.0
    keep=(sb>=LO)&(sb<=HI); sub=mags[keep]; bp=sb[keep]
    k=int(np.argmax(sub)); best=float(bp[k]); mag=float(sub[k])
    d=_parabolic(np.log(sub+1e-20),k)
    if d: best+=d*(RATE/nfft*60.0)
    edge=bool(k==0 or k==len(sub)-1)
    peaks=[(float(bp[i]),float(sub[i])) for i in range(1,len(sub)-1) if sub[i]>=sub[i-1] and sub[i]>sub[i+1]]
    best,mag,_=tie_break(best,mag,peaks,3.0,((3,2),))
    runner=0.0
    for bpm,m in peaks:
        if abs(bpm-best)<RAY: continue
        r=max(best/bpm,bpm/best)
        if any(abs(r-p/q)<0.012*(p/q) for (p,q) in ((2,1),(3,1),(4,1),(3,2),(4,3))): continue
        runner=max(runner,m)
    margin = 20*math.log10(mag/runner) if runner>0 else 40.0
    return best, margin, edge

print("=== the confidence margin, RIGHT against WRONG ===")
print("the margin is the winner's power over the tallest NON-metrical rival")
print()
cases=[("clean",None),("jitter 30ms","j"),("noise 0.05","n"),("blank 15%","b15"),("blank 30%","b30"),("blank 45%","b45")]
for label,kind in cases:
    ok=[];bad=[];det=[]
    for style in STYLES:
        for bpm in TRUTHS:
            kw={}
            if kind=="j": kw={"human":30.0}
            elif kind=="n": kw={"noise":0.05}
            odf=odf_of(BS,style,bpm,**kw)
            if kind in ("b15","b30","b45"):
                frac={"b15":0.15,"b30":0.30,"b45":0.45}[kind]
                odf=odf.copy()
                rng=np.random.default_rng(bpm+int(frac*1000))
                odf[rng.random(len(odf))<frac]=0.0
            e,m,edge=spec(odf)
            right = e is not None and abs(e-bpm)<2.0
            (ok if right else bad).append(m)
            if not right: det.append("%s %d->%.1f margin %.1f edge %s"%(style,bpm,e,m,edge))
    ok=np.array(ok); bad=np.array(bad)
    print("%-12s right %2d  margin mean %5.2f min %5.2f   |  wrong %2d  margin mean %5.2f max %5.2f" %
          (label, len(ok), np.mean(ok) if len(ok) else 0, np.min(ok) if len(ok) else 0,
           len(bad), np.mean(bad) if len(bad) else 0, np.max(bad) if len(bad) else 0))
    for d in det[:4]: print("               ", d)
