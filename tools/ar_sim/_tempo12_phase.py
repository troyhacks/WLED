import sys, os, math
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import numpy as np
from _tempo5_lock_sim import RATE, odf_of, STYLES, bin_range, _parabolic, tie_break
from _tempo6_final_sim import spectral

BS = list(range(2,9))

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

def spec_at(odf, bpm, nfft=4096, L=1125):
    x = odf[-L:].astype(np.float64); x -= x.mean()
    X = np.fft.rfft(x, nfft)
    mags = np.abs(X); freqs = np.arange(len(mags))*RATE/nfft*60.0
    k = int(np.argmin(np.abs(freqs-bpm)))
    return mags, freqs, k

def err_ms(phase, P, kicks, centre):
    e=[]
    for t in kicks:
        j = round((t-centre)/P - phase)
        e.append(abs(centre + (phase+j)*P - t)/RATE*1000.0)
    return np.array(e)

def wrap(x): return (x+0.5)%1.0-0.5

def localmax_mask(o, thr):
    n=len(o); m=np.zeros(n,dtype=bool)
    for i in range(1,n-1):
        if o[i]>=o[i-1] and o[i]>o[i+1] and o[i]>thr: m[i]=True
    return m

def comb_from(odf, P, win, mask=None, nbins=64):
    v = odf[-win:].astype(np.float64)
    idx = np.arange(len(v))
    if mask is None: w = v
    else: w = np.where(mask[-win:], v, 0.0)
    if w.sum() <= 0: return None
    ph = (idx % P)/P
    b = (ph*nbins).astype(int) % nbins
    hist = np.bincount(b, weights=w, minlength=nbins)
    k = int(np.argmax(hist))
    # parabolic refinement on the histogram
    if 0 < k < nbins-1:
        a,bb,c = hist[k-1], hist[k], hist[k+1]
        den = a-2*bb+c
        if abs(den) > 1e-12:
            d = 0.5*(a-c)/den
            if abs(d) < 1: k = k + d
    return (k/nbins) % 1.0

def circ_mean(phases, weights):
    a = 2*math.pi*np.array(phases); w=np.array(weights)
    c = float((w*np.cos(a)).sum()); s = float((w*np.sin(a)).sum())
    if c==0 and s==0: return None
    p = math.atan2(s,c)/(2*math.pi); return p % 1.0

print("=== the phase estimators, and the window length, together ===")
print()
# ---- part 1: the window length, on the tempo estimate ----
for L in (750, 1024, 1125):
    errs=[]; wrong=0
    for style in STYLES:
        for bpm in (90,100,110,120,128,140,150,160,174):
            odf = odf_of(BS, style, bpm)
            x = odf[-L:].astype(np.float64); x -= x.mean()
            X = np.fft.rfft(x, 4096)
            kmin,kmax = bin_range(90,175,4096)
            mags=np.abs(X[kmin:kmax+1]); sb=np.arange(kmin,kmax+1)*RATE/4096*60.0
            keep=(sb>=90)&(sb<=175); sub=mags[keep]; bp=sb[keep]
            k=int(np.argmax(sub)); best=float(bp[k]); mag=float(sub[k])
            d=_parabolic(np.log(sub+1e-20),k)
            if d: best += d*(RATE/4096*60.0)
            peaks=[(float(bp[i]),float(sub[i])) for i in range(1,len(sub)-1) if sub[i]>=sub[i-1] and sub[i]>sub[i+1]]
            best,mag,_=tie_break(best,mag,peaks,3.0,((3,2),))
            e=best-bpm; errs.append(e)
            if abs(e)>=2.0: wrong+=1
    errs=np.array(errs)
    print("window L=%4d (%5.2f s)  wrong %2d of 54  max %6.2f  rms %5.2f  Rayleigh %4.2f BPM" %
          (L, L/RATE, wrong, np.max(np.abs(errs)), np.sqrt(np.mean(errs**2)), 60.0*RATE/L))
print()

# ---- part 2: the phase estimators ----
res = {k: [] for k in ("E1","E2","E3","E4","E6")}
for style in STYLES:
    for bpm in (100,128,140,150):
        odf = odf_of(BS, style, bpm)
        est,_,_ = spectral(odf)
        P = 60.0/est*RATE
        kicks = kick_frames(bpm, style)
        W = int(4.0*RATE)
        centre = len(odf)-W
        # E1: the comb over every frame
        p = comb_from(odf, P, W)
        if p is not None: res["E1"].append(np.mean(err_ms(p,P,kicks,centre)))
        # E2/E3: only the local maxima above a threshold
        thr = 0.45*float(np.percentile(odf[-W:], 99.5))
        m = localmax_mask(odf, thr)
        p2 = comb_from(odf, P, W, m)
        if p2 is not None: res["E2"].append(np.mean(err_ms(p2,P,kicks,centre)))
        # E3: the circular mean of the strong onsets' phases
        idx = np.where(m[-W:])[0]
        if len(idx) >= 3:
            phs = ((idx % P)/P)
            wts = odf[-W:][idx]
            pm = circ_mean(phs, wts)
            if pm is not None:
                # the comb origin: the phase of frame n is (n mod P)/P, so the
                # onset phases are absolute already
                e3 = [abs(((t % P)/P - pm + 0.5)%1.0 - 0.5)*P/RATE*1000.0 for t in kicks]
                res["E3"].append(np.mean(e3))
        # E4: the DFT phase at the exact tempo over 2 s
        W4 = int(2.0*RATE)
        v = odf[-W4:].astype(np.float64); v-=v.mean()
        n = len(v); nc = (len(odf)-W4)+(n-1)/2.0
        ang = 2*math.pi*(est/60.0)*(np.arange(n)-nc)/RATE
        re=float(np.dot(v,np.cos(ang))); im=float(np.dot(v,np.sin(ang)))
        if abs(re)>1e-20 or abs(im)>1e-20:
            ph=(math.atan2(im,re)/(2*math.pi))%1.0
            res["E4"].append(np.mean(err_ms(ph,P,kicks,nc)))
        # E6: the comb with a 32-bin comb and the strongest-peak anchoring
        p6 = comb_from(odf, P, W, nbins=32)
        if p6 is not None: res["E6"].append(np.mean(err_ms(p6,P,kicks,centre)))

print("=== the phase estimators, mean error against the generator's kicks ===")
for k in ("E1","E2","E3","E4","E6"):
    v=np.array(res[k])
    if len(v): print("  %s  mean %6.1f ms   median %6.1f ms   max %6.1f ms" % (k, np.mean(v), np.median(v), np.max(v)))
