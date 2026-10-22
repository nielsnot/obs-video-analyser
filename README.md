# Video Analyser plugin for OBS Studio

Video Analyser is based on [Color Monitor](https://github.com/norihiro/obs-color-monitor) by Norihiro Kamae.
It adds a scope dock where you add any number of scopes, a YRGB waveform,
rectangle and ellipse selections drawn from a toolbar, and a hue / saturation / luma qualifier.
It uses its own source ids, so it can be installed next to Color Monitor.

## Features

### Selecting part of the picture
The toolbar at the top of the dock has `▭ Rectangle`, `◯ Ellipse`, `↶ Undo` and `✕ Reset`.
Switch on Rectangle or Ellipse and drag on the ROI view to draw a shape; hold `Shift` for a square or a circle.
You can draw several shapes. From then on every scope in the dock analyzes only the pixels inside the shapes,
so the waveform, vectorscope, histogram, diamond and false color table show just that part of the picture,
for example one face or the sky. Outside the shapes the ROI view is darkened.
`Undo` removes the last shape, `Reset` removes them all so the scopes use the whole picture again.
This selects what is measured; the picture itself is not zoomed.
The vectorscope can be zoomed separately with the mouse wheel.

### Qualifier
In the ROI settings, limit the analysis further to pixels within a hue, saturation and luma range,
for example only skin tones. It works together with the shapes.

### Scopes and views
- Add any number of scopes with `+ Scope`: waveform (RGB, Luma, Chroma, YUV, YRGB), vectorscope with optional
  ring scale, histogram, diamond, zebra, false color (optionally with a table of bands and percentages) and focus peaking.
- Right-click a scope for its options panel: name label, its own source (the dock selection or any camera or scene),
  type, graticule and all other options, order, maximize, projector, delete and duplicate.
- `+ View` adds views as tabs, each with its own scopes and layout.
- The grid arranges the scopes automatically; double-click a scope to maximize it.

See [Dock](doc/dock.md) for the details.

## Introduction

This plugin provides these sources to monitor color balances.

- [Vectorscope](doc/vectorscope.md)
- [Waveform](doc/waveform.md)
- [Histogram](doc/histogram.md)
- [Zebra](doc/zebra.md)
- [False Color](doc/falsecolor.md)
- [Focus Peaking](doc/focuspeaking.md)
- Diamond (see [Dock](doc/dock.md#diamond))

In addition, a dock widget is available.
- [Dock](doc/dock.md)

To hide the source and filter types from the add-source and add-filter menues, refer this document.
- [Global Configuration](doc/global_config.md)

## Quick Usage

### Dock
1. Install the plugin and boot OBS Studio.
1. Click `Tools` and `New Video Analyser Dock...` in the menu of OBS Studio.
1. Input the name, optionally set the source to monitor, and clock OK. You will see a new dock containing vectorscope, waveform, and histogram for the program.

### Projector View
1. Install the plugin and boot OBS Studio.
1. Have your source to see vectorscope, waveform, or histogram, eg. a camera.
1. Create a new scene.
1. Create vectorscope, waveform, or histogram on the scene by clicking `+` button at the bottom of `Source` list.
1. At `Source` combo box, select your source. You can select both scene and source.
   1. If the plugin increases rendering time too much, increase `scale` to scale down the image before processing.
   1. `Bypass` checkbox will show the scaled image to ensure you've select the right source.
1. You may open a windowed projector of the scene so that you can switch preview and program scene while monitoring color.
