# The AudioReactive simulation

These are the instruments the AudioReactive work was measured with.  They are
not a test suite: they are a way of asking the same question of a hundred real
tracks without a board, so that a constant in the firmware is chosen from a
measurement instead of from a guess.

Nothing here is needed to build or run WLED.  It needs Python, numpy, scipy,
librosa and PyAV.

## The corpus

`_cache.py` builds the corpus: the 100 house MP3s in `~/Downloads/house_2026`,
decimated to 6 kHz, cut into 40-second windows, three windows per track at
different offsets - 300 windows in all.  The cache is 276 MB and is NOT in git;
it is rebuilt by `_cache.py`.

The MP3s carry no BPM tag - mutagen found no `TBPM` and no `TXXX:BPM` in any of
them - so the corpus has no ground truth of its own.  Without one, an estimator
can be tuned into agreement with itself and with nothing else.

So there are two independent references:

* `_oracle_librosa.py` - librosa's beat tracker, run over the same windows.
  Its tempo, its beat times and how peaked its local tempo distribution was.
  Not truth either: it reports the half-time rate on some tracks and the
  double-time on others.  But it is somebody else's implementation, with
  different assumptions.
* `_diag_phase.py` - two onset detectors, one narrow (40-160 Hz, the kick) and
  one wide (20-2000 Hz, the snare and the hats too).  These are NOT the beat:
  a house track's hats land on the off-beats.  They are what a beat grid can be
  scored against, up to a ceiling that `_fw_ceiling.py` measures.

## The tracker twin

`_fw_port.py` is a transliteration of `usermods/audioreactive/ar_tempo.h`: the
same comb, the same kernel, the same search, the same veto, the same PLL.  It is
what the firmware's constants are chosen against.

`_fw_band.py` builds the firmware's ODF in the firmware's order - anti-alias
filter, decimate by sixteen, band at 3 kHz - because the order matters.  A
digital filter built for a corner `f` at rate `fs` has that corner at
`f * (the rate it runs at / fs)` when it runs at a different rate, so a band
designed at 3 kHz and run on a 6 kHz stream is really an 80-320 Hz band.  The
corpus was first measured on exactly that.

## The measurements

| script | what it answers |
|---|---|
| `_fw_final.py` | the band, the veto's margin and the PLL's gain, against both ground truths |
| `_fw_gain_librosa.py` | the PLL's gain, measured against somebody ELSE's beat tracker - the raw onsets reward a tracker that follows the comb, and the rate is not supposed to |
| `_fw_sweep.py` | the grid: prior weight x prior-in-narrow x margin x gain |
| `_fw_ratio.py` | how much better the far candidate is, when the music has NOT changed - the veto's margin |
| `_fw_cutscore2.py` | the same, across a cut between two different tracks |
| `_fw_relock3.py` | what the tracker actually DOES after a track change |
| `_fw_ceiling.py` | how many raw onsets a beat grid can hit at all - librosa measures the ceiling |
| `_fw_old_vs_new.py` | the old tracker and the new one, on the same windows, against the same references |
| `_fw_floor2.py` | the comb score's floor, before and after a cut |
| `_watch.py` | the live board: the published rate, its score and the cost, over `/json/ar` |

## Running one

    python tools/ar_sim/_fw_final.py --windows 120 --margins 0.70 --gains 0.20 --bands 90:320,60:240,80:320,40:160

`_watch.py` polls the board, so do not run it often: the web server's TCP task
wedges after about sixty cumulative requests.

    python tools/ar_sim/_watch.py --samples 26 --interval 5
