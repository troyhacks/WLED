# The Beat Sweep's thirty-two sub-effects

One effect id (`Beat Sweep ☾🐺`, fx 241).  The **fifth slider** picks the
sub-effect, 0 to 31.  The sliders below it are the knobs and the checkboxes
are the switches:

| control | what it is |
|---|---|
| Fallback speed | the sweep's period when there is no beat |
| Sensitivity | how deep the spectrum's ribbon goes |
| Width | the size of the light and its tail together |
| Hue | a simple 0..255 hue picker for the light |
| **Effect** | **the sub-effect, 0..31** |
| Trail | the tail behind the light |
| Loop | ON: the light loops and wraps off the end; OFF: it bounces and stops at the edges |
| Cycle | ON: the sub-effect steps on every four bars, so all thirty-two can be watched in order |

The default palette is **Rainbow** (index 11), and the palette and the
colour slots work **together** - the palette gives the spectrum its colours
and the slots stay yours.  The three colour buttons are **Light**, **BG**
(the background) and **Downbeat**.

## The thirty-two

| # | name | what it does |
|---|---|---|
| **0** | **Bass glow** | One colour; the light's brightness is the lowest of the sixteen bands - the pressure point |
| **1** | **Bass colour** | The colour is the palette at the bass level |
| **2** | **Bass width** | The light swells with the bass |
| **3** | **Bass line** | The bass is drawn on the strip as a soft patch at the low end |
| **4** | **Peak colour** | The colour is the palette at the **dominant frequency** |
| **5** | **Peak spot** | A second light sits on the dominant band while the sweep carries on |
| **6** | **Peak glow** | The light's brightness is the dominant band's level |
| **7** | **Peak line** | A mark on the ribbon at the band that is loudest |
| **8** | **Onset flash** | The light flashes on every onset |
| **9** | **Level glow** | The light's brightness is the volume |
| **10** | **Level tail** | The tail's length is the volume |
| **11** | **Breathe** | The whole strip breathes with the level |
| **12** | **Ribbon** | The sixteen bands as a ribbon; the light is a fixed colour |
| **13** | **Inverse** | The plain BG; only the light reacts, taking the colour of the band it is over |
| **14** | **Mirror** | The ribbon, mirrored about the middle |
| **15** | **Steps** | One flat block per band |
| **16** | **Stepped** | The light jumps from pixel to pixel |
| **17** | **Narrow** | The light is a hard dot, no glow |
| **18** | **Wide** | The light's width follows the level |
| **19** | **Lean** | The sweep's direction follows the music: right on the treble, left on the bass. It loops |
| **20** | **Downbeat** | The whole strip flashes on the downbeat |
| **21** | **Swell** | The light's brightness swells through the beat |
| **22** | **Phrase** | The colour walks the sixteen-step phrase |
| **23** | **Drift** | The light slides early or late with the grid's measured error |
| **24** | **One colour** | One colour (the Light slot); the brightness is the band under the light |
| **25** | **Under** | The colour is the palette at the band under the light |
| **26** | **Frequency** | The colour is the palette at the dominant frequency |
| **27** | **Level** | The colour is the palette at the volume |
| **28** | **Full** | The ribbon, the light coloured by the band under it, and the bass on its brightness |
| **29** | **Comet** | The ribbon, and the tail is coloured by the band it passed over |
| **30** | **Rail** | The whole strip lit at the level, with the light on top |
| **31** | **Pressure** | The light's brightness is the **AGC's gain** - how hard the input is being pushed, which goes the other way from the level |

## The data it reads

| from | what |
|---|---|
| the sixteen display bands | `u_data[2]` - the ones the analyser draws, not the thirty-one third-octave bands |
| the volume | `u_data[0]` |
| the onset flag | `u_data[3]` |
| the dominant frequency | `u_data[4]` |
| the AGC's gain | `u_data[10]` |
| the beat and the bar | the published tempo frame - the beat, the progress through it, the place in the bar, the grid's quality and its drift |

## The colour, two ways

| where the light's colour comes from | |
|---|---|
| the palette at the band under it | 0 |
| the palette at the dominant frequency | 1 |
| the palette at the volume | 2 |
| the palette at the band's own value | 3 |
| the palette at the light's position | 4 |
| the palette at the dominant band | 5 |
| **the Hue slider** | 6 |
| **the Light colour slot** | 7 |

## What to rank

The ones most likely to need fixing, and what to look at:

- **the ribbon** (12, 14, 15, 28, 29) - is it too dim, too noisy, too flat?
- **the light** (all of them) - too dim, too smeary, too small?
- **the loop against the bounce** - the Loop checkbox.  A loop WRAPS (the
  light's glow and tail carry on past the end and come back on); a bounce
  does NOT (it stops at the edge).  The wrap is right for a loop and wrong
  for a sweep that goes back and forth.
- **the movement** - one bar a pass, right and then left.  Is that the right
  speed, and should it be per beat instead?
- **the colours** - with the default Rainbow palette, every palette-indexed
  colour is a different hue.  Palette 0 on this board is a BLUE gradient,
  so on 0 the spectrum reads monochrome.
- **the cycle** - four bars (sixteen beats) per sub-effect.
