# Everything Orio adds

Orio keeps everything that makes Bambu Studio strong (Bambu Lab printers in cloud and LAN, profiles, interface) and adds the tools below.

**69 additions** · 52 original creations · 11 brought in from OrcaSlicer / PrusaSlicer · 6 fixes of Bambu Studio behavior

● Available · β Beta (usable, still being polished)

**Contents:** [Slicing & print quality](#slicing--print-quality) · [Supports](#supports) · [Multi-material](#multi-material) · [Prepare & plate](#prepare--plate) · [Preview & analysis](#preview--analysis) · [Modeling tools](#modeling-tools) · [Printer & connection](#printer--connection) · [Interface & comfort](#interface--comfort) · [Lighter and quieter](#lighter-and-quieter)

---

## Slicing & print quality

### β Tilted layers
On the chosen zone, layers leave the horizontal: they lean a few degrees toward the overhangs, then flatten again smoothly. Overhangs that needed supports print without them.
- Dedicated 3D tool (key **I**): it proposes the zone, the direction and the transitions by itself
- Angle matched to the real clearance of the print head, side by side; the limiting side is named
- In the tool: overhangs saved in green, overhangs still needing supports in red
- The nozzle lifts above tilted layers before each layer change: the timelapse no longer grazes the part
- Warning at slicing time if the head or the gantry would hit a neighbouring object
- Combines with painted speed, density and walls, height ranges, variable layer height, color painting, fuzzy skin and "Make overhangs printable"

### ● Brick layers
Every other inner wall is printed half a layer higher: walls interlock like the bricks of a wall, and the part resists better between layers. The infill can follow the same alternation.
- Built into the slicer: exact preview, no external script
- Per object, part, modifier or height range
- Compatible with the prime tower (AMS) and tilted layers

### ● Variable speed
Paint the speed along the height of the object, with a curve per line type: on the same layer, walls and infill can run at different speeds.
- Each line type starts from the speed it is really printed at: the print setting, capped by the filament's maximum flow (a gray line shows it)
- Also across X and Y: the speed changes along the layer; where several axes are painted, the slowest wins
- Paint on the column or directly on the part; stops (right-click on the scale) end the paint sharply at their level
- Ctrl + wheel sets the brush size; option to hide the other objects
- Maximum scale computed from the filament flow and the nozzle type
- Ctrl+Z / Ctrl+Y, curves saved in the project

### ● Variable infill and walls
The same brush for infill density and wall count: 2 walls and 10 % at the bottom, 5 walls and 40 % at the top, without splitting the object.
- The object is colored by the painted value; one-click smoothing
- A modifier keeps its own density (a 100 % local reinforcement, for instance)
- Also across X and Y; where several axes are painted, the strongest value wins
- Shell thickness marked above and below on the column

### ● Variable layer height, redesigned
Bambu Studio's layer-height painting, rebuilt like variable speed: thin layers where there is detail, thick ones elsewhere.
- Column with its mm scale, painting on the part, stops that also block smoothing
- The first layer height can be changed without losing the painted profile

### ● Walls that run through modifiers
Everywhere else, a modifier that changes the wall count, wall speed or almost any wall setting cuts the part and creates internal seams, which are classic weak points. In Orio, the common walls go round the whole part; only the extra walls stay inside the modifier zone.
- Fixes a behavior shared by Bambu Studio, OrcaSlicer and PrusaSlicer
- Extra walls follow the outline and stop cleanly at the modifier edge, anchored by an infill line
- Whatever the setting (speed, acceleration, flow, filament, brick layers…), walls cross the modifier without being cut

### ● Alternate extra wall *(from OrcaSlicer)*
One layer out of two gets an extra wall; the infill, clamped between the walls above and below, strengthens the part. Reads OrcaSlicer profiles and projects as they are.

### ● Make overhangs printable *(from OrcaSlicer)*
Under overhangs that are too steep, the material widens into a cone at an adjustable angle: the part prints without supports, at the cost of a little material.

### β Wave overhangs *(from Unlayered Slicer)*
Under a flat overhang, the material starts from the edge of the layer below and spreads in concentric waves, each one hanging on the previous one: wide overhangs print without supports and without bridges.
- In the Quality tab, per object, part or modifier
- An illustrated window: drag handles on drawings of an overhang, seen from above and in section, with a wave-by-wave animation
- Monotonic, zigzag or smart pattern; width, overlap, flow and speeds adjustable
- "Arcs" shape: fans of concentric arcs leaning on each other like scales, for even wider overhangs
- No support under the waves; a solid floor is added above

### ● Infill patterns from PrusaSlicer and OrcaSlicer
- Quarter cubic, lateral honeycomb, TPMS-D and TPMS-FK from OrcaSlicer
- PrusaSlicer's 3D honeycomb
- Multi-line infill on quarter cubic, TPMS-D and TPMS-FK

### ● 16 more settings per object
First-layer speeds, accelerations, jerk and "infill first" can be set per object: a delicate part gently and another at full speed on the same plate.

### ● Infill first, on the first layer only
Bambu Studio never applies "infill first" to the first layer. Orio adds "First layer only".

### ● Scarf seam, set with the mouse
Ten obscure settings become an illustrated window: the wall drawn bead by bead in the filament color, adjusted by dragging the nozzle.
- Available in Quality, in the filament settings and in the seam painting tool
- "Watch the nozzle print": the seam animated step by step
- A filament can carry its own scarf settings for every object it prints

### ● Flow ratios by line type *(from OrcaSlicer)*
A flow ratio for each line type (outer, inner and overhang walls, infill, surfaces, gap fill, supports), also per object and per modifier, with the volumetric speed in mm³/s next to it.

### ● Real speed of the settings
When the filament's maximum flow caps a speed, the speed really printed appears in gray next to it: "mm/s > 184". The tooltip names the limiting filament.

### ● Ironing painting
Paint the top surfaces to iron: only those areas are ironed (key **J**). Only flat, upward faces can be painted.

### ● Fuzzy skin in layers
Fuzzy skin painting works in layers, like a drawing app: each layer has its color and its own thickness, point distance and noise type. Up to 8 layers per object, plus an airbrush that speckles the part with random dots.

### ● Shorter re-slicing after a change
Changing ironing, wall order, scarf seam or flow ratios of an object no longer re-slices everything: only the affected step is redone. Changing a speed no longer recomputes supports.

### ● "Slice all" faster: a repeated part is sliced once
When the same part sits on several plates, it is sliced once and the other plates reuse the result. Each plate keeps its own G-code.

## Supports

### ● Supports adjustable zone by zone
Drop a box on a zone and change there only the threshold angle, distances, interface, walls… or, for organic trees, the branch diameter and angle.
- "Sliced supports" (Ctrl+U) shows the supports of the last slice in Prepare, to place the zones exactly
- Overhang highlighting (Ctrl+L) respects each zone's own angle

### ● Painted supports, classic or tree
The support brush has two colors: one for classic supports, one for trees. Both live on the same object, in the same slice, and the trees avoid the classic towers. A "Style" menu paints a given style (Snug, Grid, Default, Thin, Strong, Hybrid, Organic).

### ● Tree supports, all the settings
The tree settings of Bambu Studio, OrcaSlicer and PrusaSlicer in one group of 9, including the new "Branch walls". Default values are unchanged: old projects do not move.

### ● Style of classic supports
"Snug" towers that follow the overhangs, or a "Grid" that is stronger and easier to remove. It's independent of the tree style, and per object.

## Multi-material

### ● Multi-material tab *(from OrcaSlicer)*
All multicolor settings in one tab, and a different filament for each feature of the part: outer walls, inner walls, infill, solid infill, top, bottom. Cyclic or free filament order; lighter prime tower on layers without a change.

### ● Change filament on a painted object
"Change filament" at a given height finally works on a painted multicolor plate (Bambu Studio grays it out): unpainted areas change filament, painted ones keep their color.

### ● Smoother, more precise color painting
The painting tool no longer recomputes the whole part on every mouse move, and only redraws the touched area: responsive even on heavy models.
- Paints open or spiky models (scans) that Bambu Studio ignores
- Height range up to the top of the object; paint a band at a typed height
- "Paint depth" in mm or layers, one setting instead of four

### ● Prime tower with different layer heights
A fine-layer figurine next to a thick-layer box on the same multicolor plate: Bambu Studio refuses the prime tower, Orio accepts it. The tower climbs at the pace of the parts.

## Prepare & plate

### ● PrusaSlicer's arrange engine
"Arrange" can use the real shape of the parts to nest them (L, U, rings) and align the group in the center, a corner or randomly. Respects excluded bed areas and the prime tower; options are remembered.

### ● Hatched section view
In Prepare, before slicing, a slider cuts the models along X, Y or Z. Cut faces are shown solid and hatched, like a technical drawing; modifiers get their own color and hatching.

### ● Excluded bed area, clearly visible
The forbidden zone of the plate (cutter corner on X1 and P1) appears in red, and any part over it is darkened. A part sunk below the plate gets a bright outline.

### ● Thick brim ears
Each brim ear has its own thickness; ears and a regular brim can print together; the mouse snaps to vertices, midpoints and edges.

### ● Duplicate or instance
"Clone" offers two choices: Duplicate (Ctrl+D) makes independent copies, Instance (Ctrl+K) makes linked copies, so an edit on one applies to all. "Detach instance" frees one at any time.

### ● "Not printed" area
Unticking "Printable" moves the object to a clearly marked area next to the plates; ticking it puts it back. "Already printed" and "Do not print" sections on right-click.

### ● Settings per plate
Each plate can get its own settings sheet, opened like a spreadsheet tab, and even another printer. One plate for an X1C, another for an A1 mini, in the same project. "Arrange" spreads parts between machines.

## Preview & analysis

### ● X / Y / Z cuts and the real bead cross-section
The preview cuts the slice vertically along X and Y. On the cut, every bead shows its true width and height. Cuts combine and are kept per plate.

### ● Compare with the previous slice
A legend button, held down, shows the time, filament and cost of the previous slice with the changed values highlighted.

### ● A legend that talks
The first slice shows line types, each with its share of print time and material. A "Show G-code" box, a G-code window that remembers its place, no more daily tips covering the legend.

### ● Scarf seams in color
The "Line type" view has its own "Scarf seam" line: ramps and overlaps in cyan, with their share of time and material.

### ● An always up-to-date preview
Changing a setting while in the preview no longer makes the part "disappear": a card says the preview is out of date and offers to re-slice (Ctrl+R). An "Auto-slice" box re-slices one second after the last change. Designed for beginners.

### ● A card for every line type
A small "i" in front of each line of the legend opens its card: what this line is, in one sentence, and the settings that make it, with their current values. Clicking a setting opens it in the settings column.

### ● Hide an object in the preview
Hover a sliced object: an eye appears; one click hides it, leaving only a pale silhouette.

### ● Layer fusion view
Each line is colored by the share of its width resting on the layer below: red on air, blue fully supported. Under the legend, for the clicked part: bonded area and layer section, in mm² and %, at the chosen layer, between two handles and over the whole part. Accurate with brick layers and variable layer height.

## Modeling tools

### β G-code Drawing
A tab to draw print or travel paths in 3D, inside the printer volume: lines, arcs, circles, polygons, splines, text… Tools and dimensions like a CAD sketch, automatic constraints, fillets, chamfers, offsets, mirrors, patterns, 100 undo levels. Length, filament and time computed.

### ● Converter: get the part back from a G-code, make a STEP
A tab that rebuilds a mesh from a G-code (Bambu Studio, Orio, OrcaSlicer, PrusaSlicer): the outer wall of each layer gives the outline, compensations are undone, slopes are smoothed. Export as STL or re-slice with other settings.
- β Mesh to STEP: planes, cylinders, cones, spheres and tori recognized, the rest as extrusions, revolutions, helices (threads) and free-form surfaces, with no facets

### ● Emboss text *(from OrcaSlicer)*
OrcaSlicer's text tool: raised text on the object (Ctrl+T) that can follow a curved surface. Old projects with Bambu text still open.

### ● Precise cut
The cut tool (key **C**) gets a position in millimeters at any angle, a plane placed on a face, through 3 points or an edge and a point, and a precise tilt around an edge or a circle axis.

### ● Precise cut connectors
Connectors snap to the notable points of the cut face (corners, midpoints, radii, ellipse foci), at least 5 mm from the edge, in circle, grid or edge patterns. ISO threads M3 to M600, 0.15 mm clearance by default.

### ● Cut on several planes
Several planes at once, parallel or crossed: slices or quarters in one click, numbered pieces, one color per slice, connectors on every plane, one Ctrl+Z gives the whole part back.

### ● Modifiers placed to the millimeter
A toolbar tool to add material, a hole, a settings modifier, a support blocker or enforcer: the shape follows the mouse on the part and snaps to its notable points.
- Dimensions on a ruler or typed in mm; "Join" a point or face of the shape to the part, like a CAD joint
- References: dimensions to the part that fix the shape to the millimeter
- "From a face": a modifier shaped like a flat face of the part, e.g. to orient a single bridge at 45°
- A color per modifier, a lock, a table of the object's modifiers with only their changed settings

## Printer & connection

### ● G-code console
Type G-code commands and send them directly to a Bambu printer, in the cloud or on the local network. 100-command history, live printer state, a "Park" button, confirmation during a print.

### ● Exclude a part during a print
A top view of the parts in the Printer tab: click a failed part to exclude it, the printer goes on with the others. Works in LAN mode and on P1S, two cases where Bambu Studio does not offer it.

### ● Background sending
After "Send", the window closes at once and the transfer continues in the background: start the next print on another machine without waiting. Several transfers tracked at once, each cancelable; a LAN printer that drops is reconnected.

### ● Plain G-code for Bambu printers too
"Export G-code file" gives a pure `.gcode` of the plate, where Bambu Studio only gives the `.gcode.3mf`.

### ● Print and Export menus in one click
Three buttons: Slice, Print, Export. "Export all plates" writes one `.gcode.3mf` per plate in a folder named after the project. "Send all" and "Print all" drop that folder on every LAN printer, one card per machine, with each printer's AMS slots editable in place.

### ● One printer and filament catalog
All the printers and filaments of Bambu Studio and OrcaSlicer in one app: 61 brands, about 350 printer models and 5,800 filament profiles, from the setup wizard.
- Bambu Lab printers up to date (Bambu Studio 2.8 profiles, A2L included)
- OrcaSlicer's filament library (Polymaker, eSUN, Sunlu…) for every non-Bambu printer
- Marlin 2, RepRapFirmware and Repetier; adaptive bed mesh on Klipper printers
- Prusa MINI, MK4S, XL, CORE One…: binary G-code (`.bgcode`) export

### ● Create a material from scratch
"Create material" opens the filament editor on a blank sheet: no brand, no starting profile. Recognized by the AMS like any custom filament.

## Interface & comfort

### ● Cleaner tab bar
Centered, lighter tabs; less-used pages move to a menu; the home button joins the title bar.

### ● Light or dark theme in one click
A button in the title bar, left of Minimize, switches instantly without restarting.

### ● A more readable settings column
Global and Objects become two big buttons; nozzle diameter and flow in a "Nozzle" box; "Sync info" in the title bar; collapsible sections that remember their state.

### ● Comma or point
"0,4" and "0.4" are both understood, typed or pasted.

### ● Floating tool windows
Like in CAD software: tool windows move by their title bar, fold to that bar, remember their place, and can be detached into their own Windows window, on a second screen if you like.

### ● A toolbar you place anywhere
Grab the 3D-view toolbar by its handle and drop it anywhere; against the left or right edge it becomes a vertical column. Reorder tools with a long press, resize them from 50 to 200 %.

### ● Painting tools that keep the part upright
In the painting tools, the view rotates as in Prepare: the part stays upright on screen, the rotation centers on the painted object, down to the bottom view to paint the base.

### ● Zoom to the mouse, without a gray screen
Zooming between beads in the preview no longer sends the view to infinity.

### ● Faster start
Orio opens about three seconds sooner, and the home page no longer freezes after appearing. A progress bar runs under the splash logo.

## Lighter and quieter

- **No telemetry**: no usage data is sent to anyone.
- **No store, no MakerLab, no MakerWorld upload, no Helio Additive** online simulation (your G-code is never sent to its servers).
- **Readable logs**: plain text, to diagnose a problem (Bambu Studio encrypts them). A crash leaves a diagnostic file; nothing is sent.
- **About 225 MB lighter**: languages other than English and French download on demand.
- **Its own app**: own name, logo and data folder; it lives next to Bambu Studio without conflict, and 3MF projects stay readable by Bambu Studio.
