<p align="center">
  <img src="media/logo-tile.svg" alt="Orio" width="560">
</p>

<h1 align="center">Every layer, your way.</h1>

<h3 align="center">60+ tools you won't find anywhere else, in a slicer that's a pleasure to use.</h3>

<p align="center">
  Orio is a free, open-source 3D-printing slicer built on Bambu Studio.<br>
  Same printers, same profiles, same projects. Everyday work just gets a lot smoother,<br>
  and when you want to go further, you can tune your print part by part, zone by zone, millimeter by millimeter.
</p>

<p align="center">
  <a href="https://github.com/OrioSlicer/Orio/releases/latest"><img alt="Download for Windows" src="https://img.shields.io/badge/Download-Windows%2064--bit-00C2FF?style=for-the-badge&logo=windows&logoColor=white&labelColor=111316"></a>
  &nbsp;
  <a href="https://github.com/OrioSlicer/Orio/discussions"><img alt="Discussions" src="https://img.shields.io/badge/Ideas%20%26%20questions-Discussions-8A9099?style=for-the-badge&logo=github&logoColor=white&labelColor=111316"></a>
</p>

<p align="center">
  <img alt="Latest release" src="https://img.shields.io/github/v/release/OrioSlicer/Orio?color=00C2FF&labelColor=111316">
  <img alt="Downloads" src="https://img.shields.io/github/downloads/OrioSlicer/Orio/total?color=00C2FF&labelColor=111316">
  <img alt="License: AGPL-3.0" src="https://img.shields.io/badge/license-AGPL--3.0-8A9099?labelColor=111316">
  <img alt="Languages" src="https://img.shields.io/badge/languages-18-8A9099?labelColor=111316">
</p>

<p align="center"><img src="media/hero.gif" alt="A sliced vase in the Orio preview, cut open by a vertical section plane" width="900"></p>

---

## Why Orio?

<img src="media/icons/comfort.svg" width="28" height="28" align="left"> **Far more comfortable day to day.** Tool windows float, move, and go to a second screen. The toolbar sits on whichever edge you like. Slicing can run on its own, printing runs in the background, and the preview tells you when it's out of date instead of making your part "disappear".
<br clear="left">

<img src="media/icons/settings.svg" width="28" height="28" align="left"> **Settings nobody else gives you.** Speed, infill, walls, layer height and supports can change along the height of a part or zone by zone. Settings go per plate, per object, per modifier. Every tricky option has an illustrated window that shows what it does.
<br clear="left">

<img src="media/icons/overhangs.svg" width="28" height="28" align="left"> **Supports only where they help.** Paint classic and tree supports on the same part, and give each zone its own angle, distances and interface.
<br clear="left">

<img src="media/icons/bricks.svg" width="28" height="28" align="left"> **Stronger parts, same printer.** Brick layers interlock the walls. Walls run straight through modifiers instead of being cut in two.
<br clear="left">

<img src="media/icons/cut.svg" width="28" height="28" align="left"> **See inside before you print.** Cut the slice along X, Y or Z and look at the real cross-section of every bead. A layer fusion view shows where a part might split.
<br clear="left">

<img src="media/icons/three.svg" width="28" height="28" align="left"> **The best of three slicers.** OrcaSlicer's multi-material tab, flow ratios and infill patterns, PrusaSlicer's arrange engine, and the whole Bambu Studio and OrcaSlicer printer catalog, all in one app.
<br clear="left">

<img src="media/icons/quiet.svg" width="28" height="28" align="left"> **Light and quiet.** No telemetry, no store, no MakerWorld upload. Logs are plain readable text. Nothing leaves your computer.
<br clear="left">

## Highlights

### A workspace you arrange yourself
Floating tool windows you can detach to a second screen, a side panel and a toolbar you drop on any edge, collapsible settings sections, a light / dark switch in the title bar. Tricky settings, like the scarf seam, open in an illustrated window that shows what each value does.

<p align="center"><img src="media/workspace.gif" alt="The illustrated scarf seam window, then the settings panel and the toolbar moved to other edges of the window" width="820"></p>

### It keeps up with you
A card tells you plainly when the preview needs a re-slice, and Auto-slice can do it for you. Printing runs in the background. Hold the "previous slice" button to bring back the last result: every value that changed is highlighted.

<p align="center"><img src="media/previous-slice.png" alt="Slicing result legend comparing the current slice with the previous one, changed values highlighted in amber" width="820"></p>

### Settings per plate, per object, per zone
Each plate can have its own printer and settings sheet, linked to others or not. Speeds, accelerations and first-layer settings can be changed for one object only. Supports, walls and infill can change inside a single part, from a box you drop on it or a height range you pick.

<p align="center"><img src="media/plate-sheets.png" alt="Two plates in one project, the second one with its own sheet for a Bambu Lab H2S" width="820"></p>

### Speed, infill and walls along the height
A slow, careful bottom and a fast top. Two walls and 10 % infill at the bottom, five walls and 40 % at the top, without splitting the object. Paint the values on a ruler along Z, X or Y, and see them on the part before you slice. Each line type has its own speed curve, and Orio shows the speed your filament can actually reach.

<p align="center"><img src="media/variable-infill.gif" alt="Infill density painted along the height of a lemon, then the sliced result cut open" width="820"></p>
<p align="center">
  <img src="media/variable-speed.gif" alt="Speed painted along the X axis of a lemon, then the sliced result colored by speed" width="49%">
  <img src="media/variable-walls.gif" alt="Wall count painted in bands along the height of a lemon, then the sliced result" width="49%">
</p>

### Supports, zone by zone
Classic and tree supports on the same part, in the same slice: the brush has a color for each style. Drop a box on one area and change the threshold angle, distances, interface or branch diameter there only.

<p align="center"><img src="media/painted-supports.gif" alt="Classic and tree supports painted on a lemon in several styles, then sliced" width="820"></p>

### Brick layers
Every other inner wall is printed half a layer higher, so the walls lock together like bricks and the part holds better between layers. It's built into the slicer, so the preview is exact and there's no post-processing script. You can set it per object, part, modifier or height range.

<p align="center"><img src="media/brick-layers.png" alt="Cross-section of the walls: the inner walls are offset by half a layer, like bricks" width="520"></p>

### X / Y / Z cuts and the real bead cross-section
Besides the layer slider, the preview can cut the slice vertically. On the cut, each bead shows its true width and height, so overlaps and gaps between walls become visible. Cuts can be combined and are remembered per plate.

<p align="center"><img src="media/section-cut.gif" alt="A sliced lemon cut along X and Y in the preview, then a close-up of the beads on the cut" width="820"></p>

### Layer fusion view
Each line is colored by how much of its width rests on the layer below (red on air, blue fully supported), with the bonded area of the clicked part, layer by layer.

<p align="center"><img src="media/layer-fusion.png" alt="Layer fusion view of a lemon: blue where lines rest on the layer below, red where they hang over air" width="820"></p>

### Precise cutting and modifiers
Cut on several planes at once, with numbered pieces and connectors that snap to the notable points of the cut face (ISO threads from M3 to M600). Place modifiers, holes and support blockers to the millimeter, snapped to the part.

<p align="center"><img src="media/multi-plane-cut.gif" alt="A lemon cut on three planes at once, the four pieces then laid out on the plate" width="820"></p>
<p align="center"><img src="media/modifiers.gif" alt="A cylinder modifier snapped to a hole, given four walls, then the sliced result with the reinforced hole" width="820"></p>

**[See all 60+ features](docs/FEATURES.md)**

## Orio and Bambu Studio

<p align="center"><img src="media/comparison.png" alt="Comparison of Orio and Bambu Studio. Only Orio has detachable tool windows, printing in the background, settings per plate, speed, infill and walls along the height, supports per zone, brick layers, X/Y/Z cuts, and no telemetry." width="820"></p>

<details>
<summary>Same comparison as text</summary>

| | Bambu Studio | **Orio** |
|---|:---:|:---:|
| Bambu Lab printers, profiles and 3MF projects | Yes | Yes |
| Detachable tool windows, toolbar on any edge | No | Yes |
| Printing in the background while you keep working, optional auto-slice | No | Yes |
| Printer and settings per plate | No | Yes |
| Brick layers, built in | No | Yes |
| Speed, infill and walls changing along the height | No | Yes |
| Walls not cut by modifiers | No | Yes |
| Classic and tree supports on the same part, settings per zone | No | Yes |
| X / Y / Z cuts of the slice, layer fusion view | No | Yes |
| Prime tower with objects at different layer heights | No | Yes |
| Exclude a part during a LAN print, also on P1S | No | Yes |
| Plain `.gcode` export for Bambu printers | No | Yes |
| Multi-material tab, flow ratios per line type, extra infill patterns (from OrcaSlicer) | No | Yes |
| PrusaSlicer's arrange engine | No | Yes |
| 61 brands, about 350 printers, 5,800 filament profiles | Bambu Lab and a few others | Yes |
| Telemetry, store and MakerWorld upload | Yes | **None** |

</details>

## Download and install

<img src="media/icons/download.svg" width="28" height="28" align="left">

1. Download `Orio-<version>-win64.zip` from the **[latest release](https://github.com/OrioSlicer/Orio/releases/latest)**.
2. Optional: check the file against the SHA-256 checksum published with the release (`Get-FileHash Orio-*.zip` in PowerShell).
3. Unzip it anywhere and run `orio.exe`. No installer, nothing written outside `%APPDATA%\Orio`. Orio sits next to Bambu Studio without getting in its way.

**Windows says "Windows protected your PC"?** Orio isn't code-signed (a certificate costs several hundred dollars a year). Click **More info**, then **Run anyway**. The source of every release is right here, and the release notes give the checksum of the zip.

**Requirements:** Windows 10 or 11, 64-bit, and a graphics card with OpenGL 3.2 or later. macOS and Linux builds aren't available yet.

**Languages:** English and French are built in. Czech, Dutch, German, Hungarian, Italian, Japanese, Korean, Polish, Portuguese (Brazil), Russian, Spanish, Swedish, Turkish, Ukrainian and Chinese (simplified and traditional) download on demand from *Preferences > Language*.

## Printing with a Bambu Lab printer

<img src="media/icons/lan.svg" width="28" height="28" align="left"> Orio is meant to be used with your printer in **LAN mode** (local network). The step-by-step guide is here: **[docs/LAN-MODE.md](docs/LAN-MODE.md)**.
<br clear="left">

## Ideas, questions, bugs

<img src="media/icons/ideas.svg" width="24" height="24" align="top"> **Ideas and suggestions:** [Discussions > Ideas](https://github.com/OrioSlicer/Orio/discussions/categories/ideas). Upvote the ones you'd like to see.<br>
<img src="media/icons/qa.svg" width="24" height="24" align="top"> **Questions:** [Discussions > Q&A](https://github.com/OrioSlicer/Orio/discussions/categories/q-a)<br>
<img src="media/icons/prints.svg" width="24" height="24" align="top"> **Show your prints:** [Discussions > Show and tell](https://github.com/OrioSlicer/Orio/discussions/categories/show-and-tell)<br>
<img src="media/icons/bug.svg" width="24" height="24" align="top"> **Bugs:** [open an issue](https://github.com/OrioSlicer/Orio/issues/new/choose) (also from Orio: *Help > Report issue*). Please attach the 3MF project.

What's coming next: **[ROADMAP](docs/ROADMAP.md)**. Common questions: **[FAQ](docs/FAQ.md)**.

## Support Orio

Orio is free and will stay free. The best ways to help for now:

<img src="media/icons/star.svg" width="24" height="24" align="top"> **Star** the repository, it helps other makers find it.<br>
<img src="media/icons/share.svg" width="24" height="24" align="top"> **Share** your prints and Orio with your printing friends.<br>
<img src="media/icons/translate.svg" width="24" height="24" align="top"> **Translate** Orio into your language, see [CONTRIBUTING](CONTRIBUTING.md).<br>
<img src="media/icons/sponsor.svg" width="24" height="24" align="top"> **Sponsor:** a GitHub Sponsors page is on the way.

## Credits and license

Orio stands on the shoulders of giants. It's a fork of **[Bambu Studio](https://github.com/bambulab/BambuStudio)** by Bambu Lab, itself based on **[PrusaSlicer](https://github.com/prusa3d/PrusaSlicer)** by Prusa Research, which comes from **[Slic3r](https://github.com/slic3r/Slic3r)** by Alessandro Ranellucci and the RepRap community. Several features come from **[OrcaSlicer](https://github.com/SoftFever/OrcaSlicer)** by SoftFever and its contributors, and wave overhangs from **[Unlayered Slicer](https://github.com/Unlayered3D/Unlayered-Slicer)**. Details in **[CREDITS](CREDITS.md)**.

Orio is licensed under the **GNU Affero General Public License v3.0**, see [LICENSE](LICENSE).

> Orio is an independent project. It isn't affiliated with, endorsed or supported by Bambu Lab, Prusa Research or the OrcaSlicer project. Bambu Lab and all printer and filament names are trademarks of their respective owners, used only to describe compatibility. Orio is provided as is, without warranty: always keep an eye on your first prints with a new feature.
