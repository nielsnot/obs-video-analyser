# Video Analyser Dock

## Introduction

Video Analyser dock shows ROI (region of interest), and all other scopes including vectorscope, waveform, histogram.

## Opening the Video Analyser dock

To open the dock,
Click `Tools` and `New Video Analyser Dock...` in the menu of OBS Studio.
Input the name and clock OK.

## Zooming the ROI view

The ROI view is where you pick the region of interest (ROI): the part of the picture the scopes analyze.
Scroll on it to zoom in, up to 8x. By default it zooms toward face height: the middle of the width, a little above
the middle of the height, where faces sit in a close-up. `Zoom toward` in the ROI options can make it zoom around
the pointer or the middle instead. Drag with the middle mouse button, or with the left
button while no tool is on, to move around. Scroll back to 1x to see the whole picture again.
Shapes and pins can be drawn while zoomed in; they land on the right spot in the picture.

## Selecting areas with the toolbar

A slim toolbar at the top of the dock selects which parts of the picture the scopes analyze.

- `▭ Rectangle` and `◯ Ellipse` switch on drawing. While one of them is active,
  drag on the ROI view to add a shape; you can add as many shapes as you like.
  Hold `Shift` while dragging to draw a square or a circle.
  Click the active button again to stop drawing.
- `↶ Undo` removes the last shape.
- `✕ Reset` removes all shapes, so the scopes use the whole picture again.

The scopes count every pixel that lies inside at least one shape.
Shapes are outlined in green, everything outside them is shown darkened,
and they are saved with the dock.
The toolbar can be hidden from the right-click menu (`Show toolbar`).

## Adding and removing scopes

Click `+ Scope` in the toolbar, or `Add scope` in the right-click menu, and pick a type:
Waveform, Vectorscope, Histogram, Zebra, False Color, Focus Peaking or Diamond.
You can add as many scopes as you like, including several of the same type,
for example a luma waveform next to a YRGB parade.

Right-click a scope to open its options panel:
- `Name` and `Show name on scope` set the label drawn in the scope's corner.
- `Source` picks what the scope analyzes: the dock selection (the ROI with its shapes and qualifier),
  or any scene or source of its own, so one dock can show scopes for several cameras.
- `Type` turns the scope into another kind of scope in the same place.
- `Show graticule` and the scope's own options: lists as dropdowns, values as sliders, on/off options as check boxes.
- `Order` moves the scope earlier or later in the grid.
- `Maximize`, `Open Projector` and `More settings...` (the full settings dialog).
- `Delete` and `Duplicate`.

The `⋯` button in the panel opens the dock menu (add scope, views, layout, toolbar, close).
Right-clicking an empty part of the dock opens that menu directly.

The ROI view can be shown or hidden with `Show ROI`.
A new dock starts with an RGB parade waveform, a luma waveform, a vectorscope and a histogram.
Docks saved by earlier versions keep their scopes.

## Views

A dock can hold several views, each with its own scopes and layout, shown as tabs in the toolbar.
`+ View` adds an empty view. Right-click a tab to rename, duplicate or delete it;
double-click a tab to rename it. The ROI and its shapes are shared by all views.

## Layout

By default the scopes are arranged in a grid that is chosen automatically
to give each scope as much room as possible for the current dock size.
`Layout` in the right-click menu switches between `Grid (automatic)`, `Vertical`, `Horizontal` and `Free`.

**Free layout.** Turn on `⊞ Arrange` in the toolbar (this also switches to `Free`). Every scope gets a green
outline: drag it to move it, drag its bottom right corner to resize it. Positions snap to 1 % of the dock, and
scopes may overlap. Turn `⊞ Arrange` off to use the scopes again. The places are saved with the view.

**Shape.** In a scope's options, `Shape` keeps what suits the scope (`Auto`), fills its tile (`Fill`), or holds
16:9, 4:3 or 1:1. `Tile color` draws a colored frame, useful to group the scopes of one camera.

**Quick controls.** Hover a scope for a bar at its bottom: intensity (drag), grid on/off, color, gain
(1x, 2x, 5x), shape and full size.

Double-click a scope to maximize it in the dock; double-click again to show all scopes.
`Maximize` and `Show all scopes` are also in the right-click menu,
which is the way to maximize the ROI since clicks on the ROI draw the selection.

## ROI Properties

### Source

Selects one of Program, Main view, Preview, Scene, or Source.
Default is Program.

If you select Main view,
the texture before overlaying scenes made by [Downstream Keyer](https://github.com/exeldro/obs-downstream-keyer) plugin.
Without Downstream Keyer, there is no difference between Program and Main view.

### Scale

Scale factor before calculating vectorscope.
The width and height of the source will be scaled by this number.
Main purpose is to shorten the rendering time.
Larger value will degrade the accuracy and intensity.
For example, if you change scale from `1` to `2`, you need to increase intensity of Vectorscope from `1` to `4`, intensity of Waveform from `1` to `2` to get the same intensity. The intensity of Histogram won't be affected.
Default is `2`, which means width and height are both scaled by half. Available range is an integer number beween `1` - `128`.

### Interleave

This property controls whether the calculation will be interleaved or not.
Main purpose is to hide the rendering time.
If set to `0`, interleave won't happen. Every frame will be processed.
If set to `1`, a frame for each 2 frames will be processed.
In this setting, UV channel calculation and staging (sending data from GPU to CPU) will happen at odd frames, counting each color for each scope will happen at even frames.
Default is `1`.

### Qualifier (hue / saturation / luma)

When checked, only pixels whose hue, saturation and luma fall inside the ranges are counted by the scopes.
This works together with the shapes from the toolbar, so you can for example look only at skin tones inside a circle.

- `Hue center` and `Hue width` select a band of hues in degrees (red is 0, yellow 60, green 120, cyan 180, blue 240, magenta 300). A width of 360 passes every hue; grey pixels only pass when the width is 360.
- `Saturation min` / `max` (0 - 100 %) uses HSL saturation.
- `Luma min` / `max` (0 - 100 %) uses Rec. 709 luma.

Pixels that do not pass are shown darkened in the ROI view.

## Diamond

The diamond display plots each pixel twice: the upper diamond shows B − G across and B + G upwards,
the lower diamond R − G across and R + G downwards. Black sits where the diamonds touch and white at
the tips. Anything drawn outside the dashed outlines is outside the legal RGB gamut.
Options: intensity, trace color (green or white), graticule and labels.

## Vectorscope ring scale

`Ring scale with degree ticks` adds rings at 25, 50, 75 and 100 % chroma with a tick every 2 degrees
and a longer one every 10 degrees. New docks start with it on.

## False color table

Set `Show key` to `Table with percentages (right)` to show, beside the picture, every false color band
with its color, luma range, meaning and the share of the picture (or of the selection) in that band.
The table is available with the built-in colors; with a LUT file the regular key applies.

## Color picker

Switch on `⊕ Picker` in the toolbar and click on the ROI view to place a pin; up to 8 pins, the oldest makes room.
Each pin shows a swatch and the color under it, averaged over 3×3 pixels, as RGB (0–255), luma (%) and HSL.
While the picker is on, `Undo` and `Reset` remove pins instead of shapes. Pins are saved with the dock.

## Snapshot and compare

`◉ Snapshot` freezes the current picture and every scope in the view as a reference and turns on `⇆ Compare`.
While comparing, waveforms, vectorscopes and histograms draw the reference as a pink trace over the live one,
and the ROI view shows the reference on the left and live on the right. Turn `⇆ Compare` off to hide it.

## CIE diagram

Plots every pixel at its CIE 1931 xy chromaticity, in its own color, with the spectral locus, the Rec. 709,
DCI-P3 and Rec. 2020 triangles and the D65 white point. The picture is treated as Rec. 709, so it stays inside
the Rec. 709 triangle; the diagram shows how much of that gamut the picture uses.

## Lows / Mids / Highs

A vectorscope's `Part of the picture` option limits it to the dark, middle or bright third of the picture.
Add three vectorscopes and set them to lows, mids and highs to see them side by side.

## Following faces

`☺ Faces` in the toolbar makes the selection follow every face found in the picture: the faces are outlined in
cyan and every scope measures only them. The drawn shapes come back when you turn it off.

The `Faces` scope follows its source (the dock selection by default; switch the camera in OBS). It shows the picture
with the faces marked, the largest face up close, the scene levels and, for that face, its luma, hue against the skin
tone line and saturation, with written advice. Detection runs on the CPU a few times per second on a picture 320
pixels wide.

`◉ Snapshot` also keeps the face. Switch to another camera or angle and, with `Compare` on, the kept face appears
next to the live one with the differences (luma and about how many stops, hue, saturation) and whether it matches.

## Hue and saturation histograms

Set a histogram's components to `Hue` or `Saturation`. Drag across it to set the ROI qualifier to that range;
a plain click clears it. The active range is shown as a light band.
