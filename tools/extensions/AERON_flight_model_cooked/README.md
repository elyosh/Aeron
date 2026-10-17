# AERON_flight_model_cooked

`AERON_flight_model_cooked` holds the data that `aeron_gltf_cook` adds to an
[AERON_flight_model](../AERON_flight_model/README.md) asset for the game:
texture locations, precomputed simulation and render data, and baked
probes. Only the cooker writes it, and only cooked files contain it. A
cooked file lists it in `extensionsRequired`, because its textures can only
be found through it, and lists both extensions in `extensionsUsed`. Source
files contain neither this extension nor its fields.

It extends:
- the model root node: `renderData` and `probeData`;
- component nodes: `simulationData`;
- textures: their atlas image and page.

Each object has a schema that describes every field:
[node](node.AERON_flight_model_cooked.schema.json) and
[texture](texture.AERON_flight_model_cooked.schema.json).

## Textures and images

Each glTF image is a KTX2 array of atlas pages, stored in a buffer view.
An image holds one channel: base colour, normal, metallic-roughness or
emissive, depending on the material slot that references it. A texture
has no `source` of its own. Its `AERON_flight_model_cooked` object gives
its image and page:

- `source`: the image index;
- `layer`: the page in that image, 0 to 4095;
- `maxLod`: optional, the deepest mip level the texture may sample (0 to
  14). A texture packed on a shared page stops at this level, so its
  neighbours do not bleed into it. Without it, there is no limit.

`KHR_texture_transform` on the material's texture info places the texture
within its page.

A model loads an image when the material of one of its primitives
references it, or when a material variant that the model shows references
it (through `KHR_materials_variants`, for the markings values in use). In
a channel that no primitive's own material uses, every image is loaded.
The cooker packs the pages that only some variants use into separate
images, one per set of variants, so a model loads only the skins it shows.

## Simulation and render data

This is precomputed acceleration data. Loaders verify it in linear time.
When it is missing, they build it themselves and print a warning.
Malformed or stale data fails the load.

- Component `simulationData`: `{ "version": 1, "bufferView": N,
  "topologyHash": "<16 lower-case hex digits>" }`. It covers the
  component's simulation topology (the collision child's mesh, otherwise the
  render mesh) as the loader builds it: positions in metres in the Aeron
  frame, degenerate triangles dropped, triangle order preserved.
- Model root `renderData`: `{ "version": 1, "bufferView": N, "meshHash":
  "<16 lower-case hex digits>" }`. It covers the merged render triangles in
  final index order (opaque, mask, blend).

`topologyHash` is XXH64 (seed 0) of these little-endian values:
`position_count` (u32), every position (3 x f32), `triangle_count` (u32),
then the three position indices of every triangle (3 x u32). `meshHash`
hashes the render vertex positions the same way, with the index count (u32)
in place of the triangle count.

The buffer view contains exactly the following, little-endian:

```text
u32 version = 1, u32 face_count, u32 unique_vertex_count, u32 node_count, u32 sample_count
node[node_count]: f32 min[3], f32 max[3], f32 max_inv_dominant, u32 first_or_child, u32 count
u32 face_order[face_count]
u32 sample[sample_count]
```

The nodes form a bounding volume hierarchy over the triangles. Node 0 is
the root. An internal node (`count == 0`) has its two children at
`first_or_child` and `first_or_child + 1`, both stored after it. A leaf
covers `count` entries of `face_order`, starting at `first_or_child`.
Bounds are the raw triangle bounds. `max_inv_dominant` is the largest
`1 / max(|nx|, |ny|, |nz|)` over the node's triangles. Games expand a node
by their own plane tolerance times this value. Render data stores 0 in
`max_inv_dominant`, sets `sample_count = 0`, and stores the render vertex
count in `unique_vertex_count`.

For simulation data, the unique vertices are the topology positions with
bitwise-equal duplicates removed, keeping the first occurrence. The sample
holds up to 256 unique-vertex indices in farthest-point order. The first
one is the lexicographically smallest position. Each next one is the
vertex with the largest minimum squared distance to the vertices already
picked, with the lowest index winning ties. A game that needs fewer
samples uses a prefix.

## probeData

`probeData` holds the probes that an authoring tool baked, together with
the layout used for the bake, so the images and the layout always match.
`aeron_gltf_cook --probes <dir>` writes it from a probes bake folder:
`bake.json` holds the layout, and `probes.ktx2` and `grid.ktx2` hold the
images.

```json
"probeData": {
  "version": 1, "cubes": 12, "grid": 13,
  "intensity": 1, "diffuseStrength": 1, "specularStrength": 1,
  "gridBox": { "center": [0, 173.7, -115.3], "size": [133.3, 86.3, 21.3] },
  "gridResolution": [32, 21, 4],
  "keyLightShadow": 0.7,
  "probes": [
    { "center": [53.4, 144.9, -115.3], "size": [33.3, 36.0, 26.6], "shape": "box", "falloff": 0.2,
      "parallaxCenter": [0.1, 173.7, -115.3], "parallaxSize": [133.3, 86.3, 21.3], "parallaxShape": "box" }
  ],
  "reflectionPlane": { "center": [0, 173.5, -125.9], "size": [130.8, 86.0], "distance": 0.05 }
}
```

Positions and sizes are in metres, in the Aeron frame (the model frame:
x, y, z along the game's model axes). Boxes are aligned with the frame's axes.

- `cubes`: the buffer view of a KTX2 array of square BC6H or RGBA16F cube
  maps, one per probe (layer k is probe k), with mips for roughness;
- `grid`: the buffer view of a KTX2 image with one RGBA16F level of
  6 rx by ry rz texels. Point (x, y, z) uses column 6 x + f and row
  y + ry z, for f = 0 to 5. RGB is the irradiance / pi toward +x, -x, +y,
  -y, +z, -z (in that order of f), and A is the luminance of the mean
  radiance;
- `intensity` (0 to 16), `diffuseStrength` and `specularStrength` (0 to
  4): optional, default 1, scale the probes' light;
- `gridBox`: `center` and `size` (full extent) of the box that the grid
  lights fully. Outside the box, the grid's light fades out over one point
  spacing;
- `gridResolution`: points per axis, 2 to 64, placed at (i + 1) /
  (resolution + 1) of the box;
- `keyLightShadow`: optional, 0 to 1, default 0.7. When the scene's key
  light (such as a hangar's ceiling light) is in the bake, this is the part
  of the probe light that the key light's shadow removes;
- `probes`: 1 to 42 probes. Each has `center`, `size` (full extent, or the
  axes of an ellipsoid), `shape` (`box` or `ellipsoid`), `falloff` (above
  0, at most 1: the outer fraction of the influence volume over which the
  probe's weight falls to 0), `parallaxSize` and `parallaxShape` (the
  volume that its reflections are projected on), and optional
  `parallaxCenter` (default `center`);
- `reflectionPlane`: optional, an upward-facing floor (such as a hangar
  floor) that reflects the objects standing on it. It has `center`, `size`
  (along x and y) and `distance` (0 to 10, how far from the plane a surface
  can be and still get its reflection).

Images that do not match the layout fail the load.

Unknown fields are ignored.
