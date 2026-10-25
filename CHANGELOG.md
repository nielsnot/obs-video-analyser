# Changelog

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
