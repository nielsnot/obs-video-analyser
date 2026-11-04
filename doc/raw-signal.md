# Raw signal: quick setup guide

Since version 0.8.1 Video Analyser can measure the signal of a video source **as it arrives**, before OBS
converts it to 8-bit R'G'B'. In that conversion OBS clips everything below 0 % and above 100 %, so superwhite
(above 100 %, code 235 in 8-bit or 940 in 10-bit) and sub-black (below 0 %, code 16 or 64) disappear. With the raw
signal you see them again, from -7 % to 109 %.

## In short

1. Measure a **video source** (a capture card, camera, webcam or media file), not a scene or the program.
2. That's it: the plugin puts the filter **Video Analyser input (raw signal)** on that source by itself.
3. The waveform shows **RAW ...** at the bottom right, for example `RAW 10-bit V210`, and its scale now runs from
   -7 % to 109 %.

## Step by step

### 1. Point the dock at the source

Right-click the ROI view of the dock, open the options and set **Source** to your camera or capture card
(for example *Blackmagic Device*), not *Program*, *Preview* or a scene. A scene is composed in OBS as 8-bit
R'G'B', so there is no raw signal to measure there.

Scopes that measure their own source instead of the dock selection: set their **Source** the same way.

### 2. Check that it is on

- The waveform shows `RAW 8-bit ...` or `RAW 10-bit ...` at the bottom right.
- In OBS, right-click the source → **Filters**: the list has **Video Analyser input (raw signal)**.
  You do not have to change anything there. The filter passes every frame through as it is.

### 3. Blackmagic (UltraStudio, DeckLink, Intensity)

Install **Blackmagic Desktop Video** and add a **Blackmagic Device** source in OBS. In its properties:

| Setting | Set to | Why |
|---|---|---|
| Device | your UltraStudio / DeckLink | |
| Input | SDI or HDMI, the one you use | |
| Mode | the format of your signal, or Auto | |
| **Pixel format** | **10-bit YUV** | measures the 10-bit codes (shown as `RAW 10-bit V210`); 8-bit YUV works too, at 8 bits |
| Color space | Rec. 709 (or what your signal is) | the matrix used for R, G and B |
| Color range | Limited (Partial) | SDI and HDMI video are limited range: 0 % is code 64 (10-bit), 100 % is 940 |

8-bit BGRA and 10-bit RGB (R'G'B' 4:4:4) work too: they show as `RAW 8-bit BGRA` or `RAW 10-bit RGB`. When the
R'G'B' signal is video range (0 % at 64, 100 % at 940), set the filter's *Signal range* to *Video* (see below).

### 3b. From DaVinci Resolve through an UltraStudio or DeckLink

To see in OBS what leaves Resolve, including what goes beyond 0 % and 100 %:

1. In Resolve: **Project Settings → Master Settings → Video Monitoring**: set **Data levels** to **Video** and
   turn on **Retain sub-black and super-white data**.
2. In OBS, in the Blackmagic Device source: *Color range* **Limited (Partial)**, *Pixel format* **10-bit YUV**
   (or 10-bit RGB when Resolve outputs RGB 4:4:4).
3. In the source's **Filters**, select **Video Analyser input** and set **Signal range** to **Video** when the label
   says `full` while the signal is video range (often with RGB 4:4:4).

With *Data levels* set to *Full* in Resolve, nothing goes beyond 0 % and 100 %: the whole code range is used.

### 3c. The filter's settings

| Setting | What it does |
|---|---|
| Signal range | *As the source reports it* (default), *Video (legal) range* or *Full range*: how the codes are read. Set it when the source reports the wrong range |
| Also measure the raw signal when the source is SDR in an HDR project | see *HDR* below |

### 4. Other sources

| Source | Raw signal |
|---|---|
| Video Capture Device (webcam, USB capture) | yes (YUY2, NV12, ... and RGB); a webcam is usually full range, so nothing goes beyond 0 % and 100 % |
| Media Source (files) | yes, for YUV video files, 8- and 10-bit |
| NDI | yes (UYVY or BGRA) |
| Scenes, Program, Preview, browser, game or window capture | no: these are R'G'B' only |

## What uses the raw signal

| Scope | With the raw signal |
|---|---|
| Waveform | scale from -7 % to 109 %, graticule and labels continue above 100 %; `RAW ...` label |
| QC | check *Outside the preferred range (raw signal, EBU R103)*: luma or R, G, B below -5 % or above 105 % (10-bit codes 20 to 984, EBU R103 v3, the same for SDR, HLG and PQ); the timeline shows -7 % to 109 % |
| False color | two more colors: **Sub-black** (cyan, below 0 %) and **Superwhite** (white, above 100 %), also in the key and the table with percentages |
| Zebra | the stripes follow the raw luma; a high threshold of 100 % has no upper limit, so it also stripes superwhite |
| Histogram | the levels beyond 0 % and 100 % have their own bins; the graticule marks 0 % and 100 % |
| Line plot, Sat / Lum | the luma (and R, G, B) beyond 0 % and 100 % |
| Gamut diamond | colors beyond the legal range land outside the diamonds |
| Vectorscope, CIE, color chart, faces | unchanged: they measure color, not level |

## HDR (HLG and PQ)

The raw signal is used when the source has the **same transfer as the OBS project**:

| Project | Source | What the scopes show |
|---|---|---|
| SDR (Rec. 709) | SDR camera | the raw signal, `RAW ... SDR` |
| HLG (Rec. 2100) | HLG camera, for example a Blackmagic with an HLG signal | the raw HLG signal, `RAW ... HLG`: 75 % is reference white (BT.2408), superwhite above 100 % shows |
| HLG | SDR camera | the picture as OBS converts it to HLG (SDR white near 75 %), no `RAW` label |
| SDR | HLG camera | the picture as OBS converts it to SDR, no `RAW` label |

When the transfers differ, OBS converts the source before it reaches the project, so the raw signal would not be
what goes out. To see the source itself anyway, open the source's **Filters**, select **Video Analyser input** and
turn on *Also measure the raw signal when the source is SDR in an HDR project*.

The transfer comes from the source: a Blackmagic Device reports HLG or PQ when the signal carries that metadata,
a media file from its tags. A source that reports nothing counts as SDR.

## Good to know

- **Resolution:** the scopes measure at the resolution set by *Scale* in the scope options (by default half the
  picture), the same as without the raw signal.
- **Filters that crop or scale the source** (Crop/Pad, Scaling/Aspect Ratio) change the picture after the raw
  input filter, so the raw signal no longer lines up with it. Put those filters on a scene item instead, or measure
  without them.
- **A LUT** set in the ROI options is not applied to the raw signal: you see the signal as it arrives.
- **Turning it off:** remove the filter from the source (right-click → Filters). The plugin does not add it again
  while OBS keeps running; after a restart it is added again when a scope measures that source.
- **Cost:** the filter only copies frames while a scope uses them; otherwise it does nothing.

## Troubleshooting

| You see | Do this |
|---|---|
| No `RAW` label | Is the dock measuring the source itself (step 1)? In an HDR project: see *HDR* |
| `RAW 8-bit UYVY` from a Blackmagic | Set *Pixel format* to *10-bit YUV* in the Blackmagic source |
| No `RAW` label in an HLG project | The source is SDR (or reports no transfer): see *HDR* above |
| Everything is shifted up or down by about 7 % | Check *Color range* in the source: limited for normal video |
| The filter is missing | It is added a second after a scope starts measuring the source; if you removed it, restart OBS |
