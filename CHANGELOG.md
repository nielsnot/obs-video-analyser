# Changelog

## 0.7.1

- **Faces:** one source instead of up to four cameras; switch the camera in OBS. The largest face is shown up close.
- **Face snapshot:** `◉ Snapshot` also keeps the face and its measurements. With `Compare` on, the kept face is shown
  next to the live one with the difference in luma (and about how many stops), hue and saturation, and whether it
  matches, so you can check a face from another camera or angle.

## 0.7.0

- **Faces:** a new scope that finds and follows faces in up to four cameras (the first is the reference). Per camera:
  a thumbnail with the faces marked, scene luma (median, P10-P90, P99, near black, near clipping), and for the
  largest face its luma, hue against the skin tone line and saturation, with written advice and the difference to
  camera 1. Face detection by libfacedetection (BSD), on the CPU, no extra installs.
- **☺ Faces** in the toolbar: the dock selection follows every face found, so all scopes measure the faces.
- **Quick controls:** hover a scope for a bar with intensity, grid, color, gain (1x, 2x, 5x), shape and full size.
- **Waveform:** levels at the limits drawn orange (adjustable), an optional histogram along the right edge and
  pixel positions along the top.
- **Audio:** loudness timeline (30 s to 1.5 h) with short-term loudness and momentary peaks against the target,
  momentary and short-term maximum, and a room meter for 5.1 and 7.1.
- **Tile color:** a colored frame per scope, to group the scopes of one camera.
- Vectorscope shows which matrix it uses (Rec. 601, 709 or 2020).
- macOS: the analysis no longer keeps a graphics buffer mapped while the scopes compute, which made the OpenGL
  driver report "glUnmapBuffer failed" now and then.
- **Export screenshot:** `⤓ Export` (or the dock menu) saves the whole dock as it is now as a PNG.

## 0.6.0

- **Free layout:** `⊞ Arrange` lets you drag scopes anywhere in the dock and resize them at their bottom right
  corner; they may overlap. Also under Layout → Free. The places are saved per view.
- **Shape per scope:** Auto, Fill, 16:9, 4:3 or 1:1, in the scope's options.
- **Vectorscope:** the Lows / Mids / Highs preset is gone from `+ Scope`; every vectorscope has
  "Part of the picture" (all, lows, mids or highs) in its options.
- **ROI zoom** goes toward face height (the middle of the width, a little above the middle) by default;
  or toward the pointer or the middle.
- **QC timeline:** luma maximum, APL and minimum over 1 min to 4 h, with the stretches where a check failed in red,
  and for HDR the brightest pixel against 1000 nits.

## 0.5.0

- **Audio meter:** peak per channel with hold, EBU R128 loudness (momentary, short-term, integrated) against a
  target (-23, -24, -16 or -14 LUFS), phase correlation and a goniometer. Program mix or one source.
- **3D color cube:** the pixels in the RGB cube with the grey axis upright. Turns by itself; drag to turn, wheel to zoom.
- Options panel: text dropdowns, such as the audio source.

## 0.4.0

- **3D LUT:** load a `.cube` file (3D, or 1D) in the ROI options and apply it to the ROI view only, or to the view and
  everything the scopes measure. A Browse button picks the file.
- **Rec. 2100 PQ** next to HLG, picked automatically when the OBS project is PQ: nits scale, 58 % reference white line,
  Rec. 2020 vectorscope and CIE, MaxCLL and MaxFALL.
- QC: optional EBU R103 low-pass, so short overshoots do not count.

## 0.3.0

- **QC and MinMax:** minimum and maximum of R, G, B and luma, APL, share of clipped whites and crushed blacks, black
  borders, black and frozen picture; for HDR MaxCLL, MaxFALL and colors outside Rec. 709. Problems that last are
  logged with time and duration, in the scope and optionally in a CSV file.
- **Sat / Lum** plot, **line plot** (one picture line) and an **info tile** (timecode, clock, date, video format, text).
- Waveform: **colorize** the luma trace with the picture colors.
- Options panel: text, file and button options.

## 0.2.0

- **HDR HLG:** captured in linear float and encoded like the HLG output of OBS. Waveform reference white line and nits
  scale, Rec. 2020 vectorscope, false color preset for HLG levels, CIE in Rec. 2020.
- **Markers** on the waveform (lines at a level) and vectorscope (points or lines at a hue and saturation, also from a
  color picker pin), each with a name, color, style and width.
- Works with **OBS 33** (earlier versions do not load there).

## 0.1.5

- macOS: dropdowns in the options panel open instead of closing the panel.

## 0.1.4

- macOS: right-click, Control-click and two-finger tap open the scope options.

## 0.1.3

- Fixed a crash in Direct3D when a media source plays next to scopes with text or the hue ring.

## 0.1.0 – 0.1.2

- First versions: composable scope docks with views, options panel, waveform, vectorscope, histogram, CIE diagram,
  diamond, false color, zebra, focus peaking, selections, color picker, snapshot and compare, zoom in the ROI view.
