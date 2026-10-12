# Aeron Flight Model for Blender

Imports, authors and exports Aeron flight models: glTF files with the
[`AERON_flight_model`](../../extensions/AERON_flight_model/README.md)
extension, cooked into the runtime `.glb` by `aeron_gltf_cook`. Blender 5.0
or later.

## Install

Build the extension and install the zip (Edit > Preferences > Get
Extensions > Install from Disk):

```sh
blender --command extension build --source-dir aeron/tools/gltf_cook/blender_addon
```

For development, link this directory as
`<extensions>/user_default/aeron_flight_model` instead. In the add-on's
preferences, set the path of `aeron_gltf_cook` (built with the Aeron tools),
unless it is on the `PATH`: it cooks `.glb` files and reads animation files.
Setting the game's `remaster/effects` directory there makes the `.glb`
export check every animation frame against its library.

`tests/roundtrip.py` checks that models import and export unchanged (run it
with `--help`).

## Workflow

A model's source is a directory: `<model>.gltf`, its `.bin`, its PNG
textures (usually under `textures/`) and, when its materials are animated,
`<model>.anim.yaml` ([format](../../effect_pack/README.md)).

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
   game's `remaster/models/`.

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

## Materials

A material's animation (flipbooks, hull damage, energy effects) from the
model's animation file is kept on the material: Material properties >
Aeron Animation shows it, and Clear Animation removes it. It follows the
material when you rename or duplicate it, and a material that no longer
exists drops it. To change animations, edit the animation file, then use
Reload Animations in the model root's Source panel (otherwise the next
export writes Blender's animations back over your edit).

The model's skins are material variants (Blender's glTF material variants).
The model root's Source panel shows which skin each markings value shows
and how many variants are the model's own paint states; a skin named there
must keep its variant name.

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
