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
unless it is on the `PATH`.

## Workflow

1. Import a model with File > Import > Aeron Flight Model. Import the
   **uncooked** `.gltf` (with its PNG textures) that the conversion keeps:
   Blender cannot read a cooked `.glb`'s textures.
2. Edit it. The **Aeron** tab of the 3D view's sidebar (N, or View >
   Sidebar) adds parts to the active component at the 3D cursor and checks
   the model. The selected object's flight data is in Object properties >
   Aeron Flight Model: the model root's component order, articulations and
   sets; a component's fields, hardpoint and glow order, descriptor and
   hinge.
3. Export with File > Export > Aeron Flight Model (or the tab's Export).
   The export checks the model first: Aeron > Problems lists every problem
   with a button that selects its object.

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
