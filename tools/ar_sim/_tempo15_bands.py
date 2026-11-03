import sys, os, math
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import numpy as np
from _tempo5_lock_sim import RATE, odf_of, STYLES, TRUTHS, bin_range, _parabolic, tie_break
BS=list(range(2,9))
def est(odf, band, nfft=4096, L=1125):
    x=odf[-L:].astype(np.float64); x-=x.mean()
    X=np.fft.rfft(x,nfft)
    kmin,kmax=bin_range(band[0],band[1],nfft)
    mags=np.abs(X[kmin:kmax+1]); sb=np.arange(kmin,kmax+1)*RATE/nfft*60.0
    keep=(sb>=band[0])&(sb<=band[1]); sub=mags[keep]; bp=sb[keep]
    k=int(np.argmax(sub)); best=float(bp[k]); mag=float(sub[k])
    d=_parabolic(np.log(sub+1e-20),k)
    if d: best+= d*(RATE/nfft*60.0)
    edge = bool(k==0 or k==len(sub)-1)
    peaks=[(float(bp[i]),float(sub[i])) for i in range(1,len(sub)-1) if sub[i]>=sub[i-1] and sub[i]>sub[i+1]]
    best,mag,_=tie_break(best,mag,peaks,3.0,((3,2),))
    return best, edge
print("=== which tempi fail, per band (bands 2-8) ===")
for band in [(90,150),(90,160),(100,175),(120,176),(90,175),(90,180)]:
    print("--- band %g-%g ---" % band)
    for style in STYLES:
        row=[]
        for bpm in TRUTHS:
            odf=odf_of(BS,style,bpm)
            e,edge=est(odf,band)
            inside = band[0] <= bpm <= band[1]
            bad = (not inside) or abs(e-bpm)>=2.0
            row.append("%d%s%s" % (bpm, "" if not bad else ("->%.0f"%e), "E" if edge else ""))
        print("  %-9s %s" % (style, "  ".join(row)))
