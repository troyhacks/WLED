import sys, os, math
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import numpy as np
from _tempo5_lock_sim import RATE, odf_of, STYLES, TRUTHS, bin_range, _parabolic, tie_break

BS=list(range(2,9)); LO,HI=90.0,175.0
def spec(odf, band=(LO,HI), nfft=4096, L=1024, ratios=((3,2),), tol=3.0):
    x=odf[-L:].astype(np.float64); x-=x.mean()
    X=np.fft.rfft(x,nfft)
    kmin,kmax=bin_range(band[0],band[1],nfft)
    mags=np.abs(X[kmin:kmax+1]); sb=np.arange(kmin,kmax+1)*RATE/nfft*60.0
    keep=(sb>=band[0])&(sb<=band[1]); sub=mags[keep]; bp=sb[keep]
    k=int(np.argmax(sub)); best=float(bp[k]); mag=float(sub[k])
    d=_parabolic(np.log(sub+1e-20),k)
    if d: best+=d*(RATE/nfft*60.0)
    edge=bool(k==0 or k==len(sub)-1)
    peaks=[(float(bp[i]),float(sub[i])) for i in range(1,len(sub)-1) if sub[i]>=sub[i-1] and sub[i]>sub[i+1]]
    if ratios: best,mag,_=tie_break(best,mag,peaks,tol,ratios)
    return best, edge, mag

def kick_frames(bpm, style, seconds=16.0):
    beat=60.0/bpm; nb=int(seconds/beat); out=[]
    for b in range(nb):
        t=b*beat; ib=b%4
        if style in ("house","hardstyle"): out.append(t)
        elif style=="trap":
            if b%2==0: out.append(t)
        elif style in ("dnb","hiphop"):
            if ib in (0,2): out.append(t)
        elif style=="garage":
            if ib in (0,1): out.append(t)
    return np.array(out)*RATE

def comb(odf, P, win, nbins=32):
    v=odf[-win:].astype(np.float64)
    if v.sum()<=0: return None
    idx=np.arange(len(v)); ph=(idx % P)/P
    b=(ph*nbins).astype(int) % nbins
    hist=np.bincount(b,weights=v,minlength=nbins)
    return int(np.argmax(hist))/nbins

print("=== 1. the grid size N, window L=1024 ===")
for nfft in (1024,2048,4096,8192):
    errs=[]; wrong=0
    for style in STYLES:
        for bpm in TRUTHS:
            odf=odf_of(BS,style,bpm)
            e,edge,_=spec(odf,nfft=nfft)
            err=e-bpm; errs.append(err)
            if abs(err)>=2.0: wrong+=1
    errs=np.array(errs)
    print("  N=%5d  bins %3d  wrong %2d  max %6.2f  rms %5.2f  grid %5.3f BPM" %
          (nfft, len(bin_range(LO,HI,nfft)), wrong, np.max(np.abs(errs)), np.sqrt(np.mean(errs**2)), RATE/nfft*60.0))
print()
print("=== 2. the 3:2 tie-break: is it needed with bands 2-8? ===")
for label,ratios in (("tie-break ON",((3,2),)),("tie-break OFF",None)):
    errs=[]; wrong=0; bad=[]
    for style in STYLES:
        for bpm in TRUTHS:
            odf=odf_of(BS,style,bpm)
            e,edge,_=spec(odf,ratios=ratios)
            err=e-bpm; errs.append(err)
            if abs(err)>=2.0: wrong+=1; bad.append("%s %d->%.1f"%(style,bpm,e))
    errs=np.array(errs)
    print("  %-14s wrong %2d  max %6.2f  rms %5.2f   %s" % (label, wrong, np.max(np.abs(errs)), np.sqrt(np.mean(errs**2)), "; ".join(bad[:4])))
print()
print("=== 3. the tie-break tolerance ===")
for tol in (0.0,1.0,2.0,3.0,4.0,6.0):
    errs=[]; wrong=0
    for style in STYLES:
        for bpm in TRUTHS:
            odf=odf_of(BS,style,bpm)
            e,edge,_=spec(odf,tol=tol)
            err=e-bpm; errs.append(err)
            if abs(err)>=2.0: wrong+=1
    errs=np.array(errs)
    print("  tol %4.1f dB  wrong %2d  max %6.2f  rms %5.2f" % (tol, wrong, np.max(np.abs(errs)), np.sqrt(np.mean(errs**2))))
print()
print("=== 4. the phase: the comb's SIGNED error (is the bias systematic?) ===")
for nbins in (16,32,64):
    signed=[]; absol=[]
    for style in STYLES:
        for bpm in (100,128,140,150):
            odf=odf_of(BS,style,bpm)
            e,_,_=spec(odf); P=60.0/e*RATE
            kicks=kick_frames(bpm,style)
            W=int(4.0*RATE); centre=len(odf)-W
            p=comb(odf,P,W,nbins)
            if p is None: continue
            # signed: the grid line minus the kick, in beats
            ss=[]
            for t in kicks:
                j=round((t-centre)/P - p)
                ss.append(((centre+(p+j)*P - t)/P + 0.5)%1.0 - 0.5)
            signed.append(np.mean(ss)); absol.append(np.mean(np.abs(ss))*P/RATE*1000.0)
    signed=np.array(signed); absol=np.array(absol)
    print("  %2d bins: signed mean %+0.4f beat  sd %0.4f  |  abs mean %5.1f ms  max %6.1f ms" %
          (nbins, np.mean(signed), np.std(signed), np.mean(absol), np.max(absol)))
print()
print("=== 5. the strongest-onset phase anchor ===")
res=[]
for style in STYLES:
    for bpm in (100,128,140,150):
        odf=odf_of(BS,style,bpm)
        e,_,_=spec(odf); P=60.0/e*RATE
        kicks=kick_frames(bpm,style)
        W=int(4.0*RATE)
        v=odf[-W:]
        i=int(np.argmax(v))
        # the strongest frame's phase against the grid at frame 0
        ph=((len(odf)-W+i) % P)/P
        errs=[abs(((t % P)/P - ph + 0.5)%1.0 - 0.5)*P/RATE*1000.0 for t in kicks]
        res.append(np.mean(errs))
res=np.array(res)
print("  strongest onset: mean %5.1f ms  max %6.1f ms" % (np.mean(res), np.max(res)))
