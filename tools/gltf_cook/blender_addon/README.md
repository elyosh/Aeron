# Aeron Flight Model for Blender

Imports, authors and exports Aeron flight models: glTF files with the
[`AERON_flight_model`](../../extensions/AERON_flight_model/README.md)
extension, cooked into the runtime `.glb` by `aeron_gltf_cook`. Blender 5.0
or later.

## Install

Install the package for your platform, published with the game's
releases (`aeron_flight_model-<version>-<platform>.zip`), with Edit >
Preferences > Get Extensions > Install from Disk. Use the add-on of the
game version you play: the `.glb` it cooks must suit the game. The package
holds its own `aeron_gltf_cook`, which cooks `.glb` files and reads
animation and library files.

To build a package (with a cooker built with `AERON_GLTF_COOK_STANDALONE`,
which needs no library of the build machine), run
`aeron/tools/gltf_cook/package_addon.py --help`.

For development, link this directory as
`<extensions>/user_default/aeron_flight_model`, and set the path of
`aeron_gltf_cook` (built with the Aeron tools) in the add-on's preferences,
unless it is on the `PATH`. The path set there is used over the packaged
cooker.

Setting the game's `remaster/effects` directory there makes the `.glb`
export check every animation frame against its library, after building
there the libraries of the model's source tree (`<tree>/effects`, beside
the `models/` directory holding the model) that the model names and that
changed.

`tests/roundtrip.py` checks that models import and export unchanged,
`tests/animation_editor.py` the animation editor and the library operators,
and `tests/flipbook_preview.py` the flipbook preview (run them with `--help`).

## Workflow

A model's source is a directory: `<model>.gltf`, its `.bin`, its PNG
textures (usually under `textures/`) and, when its materials are animated,
`<model>.anim.yaml` ([format](../animated_materials.md)).

1. Import the model's `.gltf` with File > Import > Aeron Flight Model, and
   save your work as a `.blend`. Blender cannot read a cooked `.glb`'s
   textures: import the source.
2. Edit it. The **Aeron** tab of the 3D view's sidebar (N, or View >
   Sidebar) adds parts to the active component at the 3D cursor and checks
   the model. The selected object's flight data is in Object properties >
   Aeron Flight Model: the model root's component order, articulations and
   sets; a component's fields, hardpoint and glow order, descriptor and
   hinge.
3. Paint textures with any program, in the model's directory. Textures
   you have not used yet can stay there: only what the materials use is
   exported.
4. Export with File > Export > Aeron Flight Model Source (or the tab's
   Export Source). It offers the imported file, and writes the `.gltf`, its
   `.bin` and, when the animations changed, the animation file. Textures
   stay where they are; images from elsewhere, or that exist only in
   Blender, are copied into `textures/` as PNG. Nothing else in the
   directory is changed or removed.
5. Build the game's model from the source, or try it at once with File >
   Export > Aeron Flight Model (.glb), which cooks it into a `.glb` for the
   game's `remaster/models/`. With the effects directory set, it first
   rebuilds the effect libraries whose frames changed, and says which.

Both exports check the model first: Aeron > Problems lists every problem
with a button that selects its object. The source export also stops when:
- an image has unsaved changes (save it first: the export uses the file);
- a copied image would replace a different file of the same name;
- the result would lack an extension or extras the imported file had,
  which Blender or this add-on does not carry (Allow Losing Data exports
  anyway).

When the source file changes outside Blender, import it again rather than
export over it from an older `.blend`.

To start from plain meshes, select them and use **Make Flight Model**: they
become the components of a new model root (or of the active one).

## Structure

- **Model root**: an Empty without a parent. Its panels list the components
  in order, and hold the articulations, articulation settings and component
  sets. Those name components by object, or by a raw ordinal for one
  outside the model (a cockpit names its craft's components).
- **Components**: meshes directly under the root. Their order in the list is
  the ordinal the game uses; reorder them with the arrows.
- Under a component:
  - **hardpoints**: Empties, a position only (no rotation or scale);
  - **engine glows**: Empties looking toward their **-Y**; X and Z scale
    are the glow's width and height, Y scale its depth (negative is
    allowed);
  - one **collision** mesh (never rendered, materials ignored);
  - **lights** (below).
  The component's list orders its hardpoints and glows: hardpoints count
  per component, glows across the whole model.
- Objects without a role are not exported (a warning lists them).

## Overlays

The Aeron tab's **Overlays** draw the active object's model as the game
uses it, with a toggle per kind. They follow Blender's overlay toggle and
leave the model unchanged.

- **Engine glows** as the game draws them from the current view: a patch
  of the glow's width and height that stretches along the look axis by its
  depth when seen from the side, the core colour fading to the outer one.
  Its size and length follow **Throttle**. A selected glow is outlined; a
  disabled one is only outlined, in grey.
- **Hardpoints** coloured by family (the legend lists the model's, with
  their counts), with optional labels giving their ordinal and type. The
  pilot eye has a small view frustum.
- **Descriptors**: each component's descriptor box, faint where the game
  computes it from the collision (or render) mesh, aligned with the model;
  the active component's is highlighted.
- **Targets**: the point the game aims a component's targeting box at:
  its target, joined to the component, with a target ID; otherwise,
  fainter, its descriptor's center.
- **Hinges**: the pivot, the rotation axis (red), the direction (green)
  and up (blue) axes, and an arc to each articulation's opened angle,
  named on the active component.
- **Light shapes**: the sphere, tube or panel the game makes of each model
  light (a long panel becomes a tube), an arrow for one-sided lights, and
  optionally the range of lights that have one.
- **Component labels**: ordinal and name.

**In front** draws the markers over the model instead of hidden by it.

The active component also gets gizmos for what its overlays show: drag the
target point (with a target ID) or the hinge's pivot (Ctrl snaps, following the snapping
settings), turn the hinge's three axes together with the dial around
each one, and resize an authored descriptor box by its faces, edges and
corners. A box whose center and span matched its bounds keeps them
matched. Each drag is one undo step.

## Articulation preview

The model root's **Articulation Preview** panel turns components about their
hinges as the game does: a slider per articulation group goes from the
model as built (0) to each member at its articulation's angle (1). A
component's **Try angle** (Hinge panel) turns it alone, to try a hinge no
articulation names yet. A component's **Articulations** panel lists the
articulations that turn it, to edit, add or remove them there. Hardpoints,
glows and lights follow. A component in
several groups takes the last of its articulations whose group is not at 0.

When every slider is back at 0, or with **Reset Preview**, the components
are as before, keeping any edit made meanwhile. The exports and Check Model
refuse to run while a slider is not at 0.

## Profiles, markings and the pilot eye

A component set named `hidden_by_profile.<profile>` lists the components
the game hides for that profile. The eye in a set's row (or **Hide
Members**) hides its members and what is under them, one set at a time.
When a mission chooses no profile, the game uses `Default_<markings>`,
then `Default`.

The game gives each object a markings value (0 to 255), usually its flight
group's, and the model's markings list says which material variant each
value shows. Edit it in the model root's **Markings** panel: a row per
value, from 0, naming a variant, or empty for the materials as modelled.
A markings value without a row of its own shows the last row's variant. A
new row starts with the last row's variant, so adding one changes no look.
The eye in a row shows that value's look; **Show As Modelled** shows the
materials back. Check Model reports a row naming a variant that is missing
or that no part maps (Blender's glTF exporter would leave it out).

**View From Pilot Eye** (Aeron tab) looks from the pilot eye hardpoint
toward the model's forward.

None of these change what the exports write.

## Materials

A material's animation ([format](../animated_materials.md)) is edited in
Material properties > Aeron Animation, and written to the model's animation
file by the source export:

- **Flipbooks**: a reorderable list (per layer, the first whose state is
  active plays), each with its state, layer, mode, library, emissive
  strength or unlit part, and frames (key, seconds, thumbnail). By Prefix
  adds every image of the library whose key starts with a prefix, From
  Added the images last added to a library, and Set Duration gives all
  frames one duration.
- **Hull Damage**: its library, choice and placement, and alternatives of
  three stages each, with an optional scale and offset.
- **Energy effects**: the model's Energy Default (the model root's Object
  properties > Material Animations), shown greyed, or with Own Energy
  Effects the material's.

The animation follows the material when you rename or duplicate it, and a
material that no longer exists drops it. When the animation file changes
outside Blender, use Reload Animations in the model root's Material
Animations panel (otherwise the next export writes Blender's animations back
over the edit). Check Model reports frames without a library and, once the libraries are
loaded, keys a library lacks or of the wrong kind.

**Preview.** With the checkbox of the Aeron Animation > Preview sub-panel,
the viewport (Material Preview or Rendered shading) shows the flipbooks as
in the game at the timeline's frame, with the checked states active (and
`always`). The preview follows Blender's timeline: play it (the panel's
Play Timeline button, or Space) or scrub it to animate them; the scene's
start frame is the flipbooks' start, and its frame range bounds how long
they play before looping back. It shows
flipbooks only, not hull damage or energy effects, and needs the model in a
source tree (it loads the tree's libraries). A material is shown when it is
a Principled BSDF whose base colour is a texture or a value; the panel says
why another is not. The preview adds a node group and image nodes to the
materials, which it removes when it stops, around the exports and while the
file is saved: they never reach your files.

**Effect libraries.** For a model in a source tree (`<tree>/models/<key>/`),
the Effect Libraries sub-panel loads the tree's libraries
(`<tree>/effects/`): key fields then offer the library's images of the
right kind, frames show thumbnails, and each library shows how many models
use it (libraries are shared). Library operators write the library's files
at once, not at the source export, and Blender's undo does not revert them:

- New Library and Add Images: PNG files copied into the library's
  `frames/`, one image per file, keyed by file name. For emissive images,
  `<key>_base_color.png` gives `<key>`'s base colour.
- Add Blender Image: an image painted in Blender, saved as
  `frames/<key>.png`; save later repaints with Image > Save (the `.glb`
  export refuses unsaved library images).
- Edit, Rename and Remove Image. Rename and Remove first list the models
  that use the key; Rename also renames it in this model's frames, not in
  other models' files.

So a new animation is: paint the frames, Add Images, From Added on the
material's flipbook, then the `.glb` export (which builds the changed libraries) to
try it, and the source export to keep it. When a library rebuild removes a
key, or changes an image's kind, that other models use, the `.glb` export
names them: their next cook fails.

The model's looks (converted paint states and skins) are material variants
(Blender's glTF material variants). The markings list and missions name
them, so renaming a variant breaks those names.

Glass is a transmissive material (`KHR_materials_transmission`): its base
colour tints what shows through it, as in any glTF viewer. Converted glass
keeps XWAU's look in both its base colour and transmission textures, so
retouch them together. EEVEE shows what is behind glass with Raytracing on
(Render properties); the import turns on the materials' raytraced
transmission.

Components and collision meshes need a positive uniform scale, and nothing
may be mirrored or animated. Vectors (target, hinge pivot and axes,
descriptor) are in the component's local space; the buttons next to them
take the 3D cursor. Without an authored descriptor, the game computes it
from the collision (or render) mesh's bounds.

## Lights

Every point or area light under a component is a model light, unless its
**Not a model light** box is checked. The game has no light shadows; the
**Game-like light preview** toggle turns the model lights' shadows off.

| Blender light | Game light |
|-|-|
| Point | sphere of the light's Radius |
| Area, Rectangle or Square | one-sided panel of its size (Disk and Ellipse: a panel of the same area) |
| Area with **Tube** | tube along the X size, the Y size its thickness; **One-sided** makes it a strip emitting toward -Z |

The light panel shows what the game draws: a panel at least three times
longer than wide becomes a one-sided tube, a squarer one a one-sided sphere
of its area. Spot and sun lights are not supported.

Intensity is exported in candela, converted as Blender's glTF exporter does
(683 lm/W): a point light or an all-around tube gives its power x 683 / 4pi,
a panel or one-sided tube its power x 683 / pi, straight ahead. Exposure,
temperature and, with Normalize off, the area are included. The game turns
candela into its lighting with `point_lights.model_light_intensity`; the
current value makes a Blender sun of strength 2.5 about the game's white
sun, so preview with such a sun to keep the game's balance. A light's
**Custom Distance** is its range (it fades out just before it); without one,
the game ends the light where it becomes negligible.

The game ranks all its lights each frame and keeps the 256 that matter most
near the camera. A light on a hidden (destroyed) component is dark.
