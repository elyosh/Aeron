# Aeron Flight Model for Blender

This add-on imports, edits and exports Aeron flight models. A flight model
is a glTF file with the
[`AERON_flight_model`](../../extensions/AERON_flight_model/README.md)
extension, which `aeron_gltf_cook` builds into a `.glb` for the game. The
add-on also imports X-Wing Alliance OPT models, from the original game or
from the X-Wing Alliance Upgrade (XWAU). It needs Blender 5.0 or later.

## Install

Each game release comes with a package for each platform
(`aeron_flight_model-<version>-<platform>.zip`). Install it with Edit >
Preferences > Get Extensions > Install from Disk. Use the add-on that
matches the game version you play, because the `.glb` files it builds must
match the game. The package includes its own `aeron_gltf_cook`, which builds
`.glb` files and reads and writes effect library files, and `opt2gltf`,
which converts OPT models.

To build a package, run `aeron/tools/gltf_cook/package_addon.py --help`.
Build the tools with `AERON_GLTF_COOK_STANDALONE` first, so that they do not
depend on libraries of the build machine.

For development, link this folder as
`<extensions>/user_default/aeron_flight_model`. In the add-on preferences,
set the paths of `aeron_gltf_cook` and `opt2gltf` (built with the Aeron
tools), unless they are on the `PATH`. Paths set in the preferences are
used instead of the packaged tools. Bake Probes also needs
`environment_bake`, which a package includes when it is built with
`--environment-bake`.

If you also set **Game Folder** in the preferences to the game's
`remaster` folder, Export for Game checks every animation frame against its
library in `remaster/effects`. Before that, it rebuilds in that folder the
project libraries that the model uses and that have changed. Bake Probes
also writes to the Game Folder.

Tests (run each one with `--help` for its options):
- `tests/roundtrip.py`: models import and save without changes.
- `tests/animation_editor.py`: the animation editor and the library
  operators.
- `tests/flipbook_preview.py`: the flipbook preview.
- `tests/opt_import.py`: the OPT import.
- `tests/probes.py`: probes authoring, their glTF round trip and the bake.

## Workflow

Models live in a **project**. A project is a folder with each model in
`models/<key>/`, and the effect libraries that the materials play frames
from in `effects/<library>/`. All models of the project share these
libraries. A model folder holds `<key>.gltf`, its `.bin` and its PNG
textures (usually in `textures/`). The `.gltf` holds everything else,
including the material animations ([format](../animated_materials.md)).

1. Import the model's `.gltf` with File > Import > Aeron Flight Model
   (.gltf/.glb), and save your work as a `.blend`. Every 3D view then shows
   the whole model. Import the `.gltf`, not a `.glb` built for the game:
   Blender cannot read the textures of such a `.glb`.
2. Edit the model. The **Aeron** tab of the 3D view sidebar (N, or View >
   Sidebar) adds parts to the active component at the 3D cursor, and checks
   the model. The flight data of the selected object is in Object
   properties > Aeron Flight Model. For the model root, this is the
   component order, the articulations and the sets. For a component, it is
   its fields, the order of its hardpoints and engine glows, its bounds
   and its hinge.
3. Paint textures with any program, in the model folder. Textures that no
   material uses yet can stay there: only the textures that the materials
   use are exported.
4. Save the model with **Save to Project** in the tab (or File > Export >
   Aeron Flight Model (.gltf)). It proposes the imported file, and writes
   the `.gltf` and its `.bin`. Textures stay where they are. Images from
   other folders, or images that exist only in Blender, are copied into
   `textures/` as PNG. Nothing else in the folder is changed or removed.
5. Try the model in the game with **Export for Game** (or File > Export >
   Aeron Flight Model for Game (.glb)). It builds a `.glb` for the game's
   `remaster/models/` folder. When the game effects folder is set, it first
   rebuilds the effect libraries whose frames changed, and lists them.

Both exports check the model first. Aeron > Problems lists every problem,
with a button that selects the object at fault. Save to Project also stops
when:
- an image has unsaved changes. Save the image first, because the model
  uses the file;
- a copied image would replace a different file with the same name;
- the result would lose an extension or extras that the imported file had,
  because Blender or this add-on does not keep them. Turn on **Allow Losing
  Data** to save anyway.

If the `.gltf` changes outside Blender, import it again instead of saving
over it from an older `.blend`.

## Importing an OPT

To convert an OPT into a project and import it as above, use File > Import
> XWA OPT (.opt), the **Import OPT** button in the tab, or drop an OPT on the
3D view. The model goes into `models/<key>/`, where the key is the OPT name
in lower case.

The import goes into the **OPT Project** shown in the tab. Click it to
choose another one. It is the project of the last OPT imported into this
file, or else the project of a model in the scene, or else the last project
used. The file browser's sidebar can change it for one import, and suggests
the known projects.

A dialog asks for the project when there is none yet, and before replacing a
model that the project already has. Replacing it loses the changes made to
that model. A dropped OPT always opens this dialog. Its folder button picks
a folder, and a new folder starts a new project. Under **Source**, the
dialog shows which XWA Upgrade installation the import will use.

**Source** sets the game the model comes from:

- **Original Game**: the OPT alone, as the original game draws it, with the
  game's data for the model (its pilot eye, crew and moving parts).
- **XWA Upgrade**: the OPT with everything XWAU adds to it: materials,
  normal maps and animations, skins (as variants, including the ones that
  missions choose), object profiles, moving parts, pilot eye and cockpit
  displays. The images that its animations play are added to the
  project's effect libraries. Images that the libraries already have are
  kept as they are.
- **Auto** (the default): XWA Upgrade if its data is found, otherwise
  Original Game.

XWAU data is read from the game folder that contains the OPT's
`FlightModels` folder, if that game folder has XWAU's `Materials` folder.
Importing an OPT from such a folder also sets **XWA Upgrade Folder** in
the preferences. That installation is then used for OPTs that are not in a
game folder.

To start from plain meshes, select them and click **Make Flight Model**.
They become the components of a new model root, or of the active one.

## Structure

- **Model root**: an Empty without a parent. Its panels list the components
  in order, and hold the articulations, the articulation settings and the
  component sets. These name a component by its object, or by its number
  for a component outside the model (for example, a cockpit names the
  components of its craft).
- **Components**: meshes directly under the root. Their order in the list is
  the component number that the game uses. Reorder them with the arrows.
- Under a component:
  - **hardpoints**: Empties that have only a position (no rotation or
    scale);
  - **engine glows**: Empties that glow toward their **-Y**. The X and Z
    scale set the width and height of the glow, and the Y scale sets its
    length (it can be negative);
  - one **collision** mesh (never rendered, its materials are ignored);
  - **lights** (see below).
  The list of a component sets the order of its hardpoints and engine
  glows. Hardpoints are numbered per component, and engine glows across the
  whole model.
- Objects without a role are not exported, and a warning lists them.

## Overlays

The **Overlays** of the Aeron tab draw the model of the active object as the
game uses it, with a toggle for each kind. They follow Blender's overlay
toggle and do not change the model.

- **Engine Glows**, as the game draws them from the current view. Each glow
  is a patch the size of its width and height. Seen from the side, it
  stretches along its glow axis by its length. Its core color fades to its
  outer color. **Throttle** sets its size and length. A selected glow is
  outlined. A glow that is not enabled is only outlined, in gray.
- **Hardpoints**, colored by family. The legend lists the families of the
  model with their counts. **Labels** shows the number and type of each
  hardpoint. The pilot eye has a small view frustum.
  With **Projectiles**, each weapon hardpoint shows the projectile it fires,
  at its real size, leaving the hardpoint toward the front of the model.
  The projectile is the project's model with the same name as the game's
  (for example `models/laserrebel/`), drawn unlit. When the project does
  not have it, it is converted once from the game folder set in the
  preferences (**XWA Upgrade Folder**), as the original game draws it.
  Without either, or with **Projectiles** off, an arrow shows the
  direction of fire.
- **Bounds**: the bounds box of each component, aligned with the
  model. A box that the game computes from the collision mesh (or else the
  render mesh) is faint. The box of the active component is highlighted.
- **Targets**: the point where the game puts the targeting box of a
  component. For a component with a target ID, this is its target point,
  with a line to the component. Otherwise it is the center of its
  bounds, drawn fainter.
- **Hinges**: the pivot, the rotation axis (red), the direction axis
  (green), the up axis (blue), and an arc to the open angle of each
  articulation. The arcs are named on the active component.
- **Light Shapes**: the sphere, tube or panel that the game makes of each
  model light (a long panel becomes a tube), and an arrow for one-sided
  lights. **Light Ranges** also draws the range of lights that have one.
- **Component Labels**: the number and name of each component.

**In Front** draws the markers on top of the model instead of hiding them
behind it.

The active component also gets gizmos for what its overlays show:
- drag the target point (when it has a target ID) or the hinge pivot. Ctrl
  snaps, following the snapping settings;
- turn the three hinge axes together with the dial around each axis;
- resize a custom bounds box by its faces, edges and corners. If the
  center and span of the box matched its bounds, they stay matched.

Each drag is one undo step.

## Articulation preview

The **Articulation Preview** panel of the model root turns components
around their hinges as the game does. Each articulation group gets a slider
that goes from the model as built (0) to each member at the angle of its
articulation (1). **Preview Angle** in the Hinge panel of a component turns
that component alone, to try a hinge that no articulation uses yet. The
**Articulations** panel of a component lists the articulations that turn
it, and you can edit, add or remove them there. Hardpoints, engine glows
and lights move with their component. If a component is in several groups,
the last of its articulations whose group is not at 0 is used.

**Play** moves the model as the game does: each articulation group opens
at its **Speed**, stays open for a second, closes at its **Closing Speed**,
stays closed for a second, and starts again. Speeds are in 1/256 turn per
`step_ticks` game ticks (an articulation setting, 15 by default; the game
runs 236 ticks per second). Rotary gun turrets turn on their hinge at the
game's idle speed and change direction about every 2.7 seconds. **Pause**
keeps the components where they are.

When every slider is back at 0, or when you click **Reset Preview**, the
components go back to where they were, and edits made during the preview
are kept. The exports and Check Model do not run while the preview has
turned components.

## Profiles, markings and the pilot eye

A component set named `hidden_by_profile.<profile>` lists the components
that the game hides for that profile. The eye in the row of a set (or
**Hide Members**) hides its members and everything under them. Only one set
can hide its members at a time. When a mission chooses no profile, the game
uses `Default_<markings>`, then `Default`.

The game gives each object a markings value (0 to 255), usually the one of
its flight group. The markings list of the model says which variant
(see [Variants and skins](#variants-and-skins)) each value shows. Edit it
in the **Markings** panel of the model root. Each row is a value, starting
from 0, and names a variant. An empty row shows the materials as modeled.
Values after the last row use the last row. A new row starts with the
variant of the last row, so adding a row doesn't change how any value
looks. The eye on a row shows that value, and **Show As Modeled** shows the
modeled materials again. Check Model reports a row that names a variant
that doesn't exist.

The import adds a camera named "<model> Pilot Eye" for a model with a
pilot eye hardpoint, with the game's cockpit field of view (64 degrees
across). It follows the pilot eye hardpoint. **View From Pilot Eye** (Aeron
tab) makes it the scene camera and looks through it, and creates it again
if it was deleted. It is not part of the model, so the exports leave it
out.

None of these change what the exports write.

## Materials

The animation of a material ([format](../animated_materials.md)) is edited
in Material properties > Aeron Animation. The exports write it to the
`AERON_flight_model` object of the material.

- **Flipbooks**: a list that you can reorder. On each layer, the first
  flipbook whose state is active plays. Each flipbook has a state, a layer,
  a mode, a library, a strength or unlit amount, and frames (image, seconds
  and thumbnail). **Add by Prefix…** adds every image of the library whose
  name starts with a given prefix. **Add Recent…** adds the images last
  added to a library. **Set Duration…** gives all frames the same duration.
- **Hull Damage**: its library, **Pick Group** and **Square Group**, and
  alternatives with three stages each (75%, 50% and 25% hull). Each
  alternative can use a **Random Square** of the texture instead of the
  whole texture, with a **Square Size** and an **Offset**.
- **Energy effects**: by default, the material uses the model's **Default
  Energy Effects** (in Object properties > Material Animations of the model
  root), shown grayed out. With **Own Energy Effects**, it uses its own.

The animation follows the material when you rename or duplicate it, and it
is dropped when the material is deleted. If the animations in the `.gltf`
change outside Blender, click **Revert Animations** in the Material
Animations panel of the model root. Otherwise the next export writes the
animations from Blender back over your changes. The screen of a material
(a cockpit display) is kept as it was imported. Check Model reports frames
without a library. Once the libraries are loaded, it also reports images
that are missing from a library or that are of the wrong type.

**Preview.** Turn on the checkbox of the Aeron Animation > Preview
sub-panel. The viewport (in Material Preview or Rendered shading) then
shows the flipbooks as in the game, at the current frame of the timeline,
with the states checked under Preview States (and Always) active. The
preview follows Blender's timeline: play it (**Play Timeline** in the panel,
or Space) or scrub it to animate the flipbooks. The start frame of the scene
is where the flipbooks start, and its frame range sets how long they play
before the timeline loops back. The preview shows only flipbooks, not hull
damage or energy effects. It needs the model to be in a project, because it
loads the project's libraries. A material is shown if it is a Principled
BSDF whose base color is a texture or a value. For other materials, the
panel says why they are not shown. The preview adds a node group and image
nodes to the materials. It removes them when it stops, during the exports
and while the file is saved, so they never reach your files.

**Effect libraries.** For a model in a project (`<project>/models/<key>/`),
**Load Libraries** in the Effect Libraries sub-panel loads the project's
libraries (`<project>/effects/`). Image fields then suggest the images of the
library that have the right type, frames show thumbnails, and each library
shows how many models use it (libraries are shared). The library operators
write the library files immediately, not at Save to Project, and Blender's
undo does not revert them:

- **Add Images**, and the new library button above it: copy PNG files into
  the `frames/` folder of the library, one image per file, named after the
  file. For emissive images, `<name>_emissive.png` is also named `<name>`,
  and `<name>_base_color.png` gives the base color of `<name>`.
- **Add Blender Image**: saves an image painted in Blender as
  `frames/<name>.png`. Save later changes to it with Image > Save. Export for
  Game refuses library images with unsaved changes.
- **Edit Image**, **Rename Image** and **Remove Image**. Rename Image and
  Remove Image first list the models that use the image. Rename Image also
  renames the image in the frames of this model, but not in the files of other
  models. Remove Image keeps the PNG files in `frames/`.

So to make a new animation: paint the frames, use Add Images, then Add
Recent… on the flipbook of the material. Then use Export for Game (which
builds the changed libraries) to try it, and Save to Project to keep it. If
a library rebuild removes an image that other models use, or changes its
type, Export for Game names those models: their next build fails.

Glass is a transmissive material (`KHR_materials_transmission`). Its base
color tints what shows through it, as in any glTF viewer. Converted glass
keeps XWAU's look in both its base color and transmission textures, so
retouch them together. EEVEE shows what is behind glass when Raytracing is
on (Render properties). The import turns on raytraced transmission in the
materials.

Components and collision meshes need a positive uniform scale, and nothing
may be mirrored or animated. Vectors (target, hinge pivot and axes,
custom bounds) are in the local space of the component. The buttons next to
them set them from the 3D cursor. Without custom bounds, the game computes
the bounds from the collision mesh (or else the render mesh).

## Variants and skins

A variant is a look of the model. Markings values and missions choose
variants by name. The **Variants** panel of the model root lists them, with
the markings values that show each one. The eye on a row shows the variant,
and **Show As Modeled** shows the modeled materials again.

A variant can:
- replace materials. **Replaced materials** lists them, and **+** adds one:
  wherever the model has the first material, the variant shows the second.
- paint skins over materials. **Layers** lists the skins in painting
  order, each with an opacity.
- start from another variant (**Base**). Where this variant doesn't replace
  or paint a material, the base's replacement shows. The base's layers are
  not used.

A skin is a set of images painted over materials. The **Aeron Skins** panel
of a material lists its skins. A skin has a base color image, whose alpha
says where the skin covers the material, and optional emissive and
metallic-roughness images (roughness in G, metallic in B). The images use
the UV layout of the material's base color texture and must have the same
size. A skin can be on several materials, with the same name. A layer
paints the modeled material, even where the variant or its base replaces
that material.

The eye on a skin shows it alone, on every material that has it. **+** next
to a skin image creates an image that changes nothing until you paint it: a
transparent base color, or a copy of the material's emissive or
metallic-roughness. It has the size of the material's texture and is saved
in `textures/skins/<skin>/`.

To paint a skin image, click the brush next to it. This shows the skin
alone, isolates the parts that have the material (local view), and starts
Texture Paint on that image. Painting the base color also paints its alpha,
which is where the skin covers the material: use the Erase Alpha blend to
uncover. The emissive and metallic-roughness images show only where the
base color covers. Save the image with Image > Save, and click the brush
again to stop. After editing an image in another program, reload it
(Image > Reload in the Image Editor). The preview mixes partly covering
skins in linear light, and the game mixes the stored colors, so those
texels can look slightly different.

An OPT can give faces one texture per markings value. The import makes the
first one the material and the others variants named `State 1`, `State 2`,
and so on. Converted XWAU skins become skins and variants.

Renaming a variant updates the markings rows and bases of the model that
use the name. Missions that use the old name break.

The exports write replaced materials as glTF material variants, and layers
and bases as skin variants with the same name. Export for Game composites
the skins into the .glb. Save to Project keeps the skin images under
`textures/skins/<skin>/`. Check Model reports variants that change nothing,
bases that replace nothing, layers whose skin no material has, skins that
no variant uses, and skin images of the wrong size.

## Lights

Every point or area light under a component is a model light, unless its
**Exclude from Model** box is checked. The game draws lights without
shadows. The **Game-Like Lights** toggle turns off the shadows of model
lights.

| Blender light | Game light |
|-|-|
| Point | sphere with the Radius of the light |
| Area, Rectangle or Square | one-sided panel of its size (Disk and Ellipse: a panel with the same area) |
| Area with **Tube** | tube along the X size, with the Y size as thickness. **One-Sided** makes it a strip that emits toward -Z |

The light panel shows what the game draws. A panel at least three times
longer than it is wide becomes a one-sided tube. A squarer panel becomes a
one-sided sphere with the same area. Spot and sun lights are not supported.

Intensity is exported in candela, converted the same way as Blender's glTF
exporter (683 lm/W). A point light or an all-around tube gives its power x
683 / 4pi. A panel or one-sided tube gives its power x 683 / pi, straight
ahead. Exposure, temperature and, with Normalize off, the area are taken
into account. The game converts candela into its own lighting with
`point_lights.model_light_intensity`. With the current value, a Blender sun
of strength 2.5 matches the game's white sun, so preview with such a sun to
keep the game's balance. The **Custom Distance** of a light is its range,
and the light fades out just before it. Without one, the game ends the
light where it becomes negligible.

Each frame, the game ranks all its lights and keeps the 256 that matter
most near the camera. A light on a hidden (destroyed) component is dark.

## Probes

Probes give a model's inside its own light and reflections in the game: a
hangar, or a capital ship's bays and the areas ships fly into. The game
uses them for the model and for every object that reaches them, and
blends them with the scene's environment where they do not apply. They
are Blender light probes under the model root:

- **The light grid** (one Volume light probe): a box around the zone you
  care about, such as a hangar's interior or a bay. Its points hold the
  light that reaches each place, which lights the model and the ships
  inside; the box is what it lights fully, and its light fades out within
  one point spacing outside it. **Add Light Grid** (Aeron tab) puts a
  60 x 60 x 20 m box on the floor of a hangar (its InsideHangar hardpoint,
  type 25), else a 50 m box at the 3D cursor: size it to the zone. Its
  **Spacing** (default 4 m) sets how far apart its points are, within 8192
  points.
- **Reflection probes** (Sphere light probes, up to 42): each reflects the
  area around it. **Fill with Probes** in the grid's panel fills its box
  with probes about 25 m wide, at most 16, each with **Room Parallax**:
  their reflections are projected on the grid's box, so that all of them
  show the room's walls in place. Running it again replaces the probes it
  made (those with **Filled** checked; uncheck it to keep one). **Add
  Reflection Probe** adds a 10 m box, the model's first one 3 m above its
  InsideHangar hardpoint, else at the 3D cursor, for a spot that needs its
  own probe, such as a shiny feature. A probe's influence (box or
  ellipsoid, with its Falloff) is where it applies; without Room
  Parallax, its custom parallax is the volume its reflections are
  projected on. Where probes overlap, the smallest wins; the game uses at
  most 4 in one place.

Probes cannot be rotated relative to the model root: only their position
and size count. The panel of a probe shows Blender's probe settings, which
Blender itself shows only under EEVEE, and an estimate of the bake time.
A probe's Clip End is how far its render sees: anything farther is black in
its reflections. Add Reflection Probe and Fill with Probes set it to reach
the whole model, and validation warns when it doesn't reach the far side of
the probe's parallax volume.

**Bake Probes** renders a panorama from each reflection probe (Samples
per pixel, denoised) and from each grid point (256 x 128 pixels at 16
samples) with Cycles, on the GPU when Cycles has one, and environment_bake
makes the package. It shows its progress; Esc cancels. Grid points inside geometry (most of
their rays hit back faces) are left out and take their neighbours' light.
The bake renders what the scene renders: the World is black, so nothing
comes from outside the model. To leave an object out of the probes or the
grid only, use its Light Probes visibility (Object Properties >
Visibility): Sphere for the reflection probes, Volume for the grid. Turn
off the render visibility of anything to leave out of both.

Bake times, on an Apple M4 Pro: about 3.5 s per reflection probe, and
0.05 s per grid point on the GPU (0.13 s on the CPU).

The model's lights inside the grid's box are baked into it: they are
exported with `baked`, and the game leaves them out while it uses the
probes, so their light is not counted twice. Lights outside it, such as
running lights, stay live.

The bake writes into the game's `remaster` folder (the Game Folder
setting, or else a folder you choose), in
`flight/environments/models/<model key>/`: `probes.ktx2`, `grid.ktx2`,
`package.yaml`, `mapping.yaml` and a `panorama_<n>.hdr` per reflection
probe. Merge `package.yaml` and `mapping.yaml` into the environment
manifest (`flight/environments/environments.yaml`) to use the probes for
that model: `mapping.yaml` adds the model under `xwa.probes`. The
panoramas are the renders the reflections are made from; the game doesn't
use them. Each frame, the game uses the probes of the 8 models nearest to
the camera that have them.

Limits:
- The World is black during the bake, so light from the sky through open
  hangar or bay doors is missing. Add lights near the doors if that area
  needs more light.
- Light can leak through a wall thinner than the point spacing. Reduce
  the Spacing if it does.
- The grid's light is soft: it gives no sharp shadows.
- The probes reflect the Blender scene as it was baked. The ships that the
  game places in the hangar don't show in the reflections.

To review a reflection probe in Blender, click **Preview Lighting** in its
panel. It shows the probe's panorama as the World, turned as in the game,
so a ship placed in the scene shows that probe's light and reflections in
Material Preview or Rendered view. Click it again to put the previous
World back. In the game, the Environment debug tool shows the package
(its probes, grid and memory) and the frame's probe sets, and its Reload
button loads it again after a new bake. Its **Show probes** checkbox draws
each probe's influence, the grid's box and the grid points near the
camera, to check that they sit where they do in Blender. Save to
Project keeps the probes and the grid in the model's `.gltf`.
