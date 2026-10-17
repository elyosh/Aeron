# AERON_flight_model_cooked

`AERON_flight_model_cooked` holds what `aeron_gltf_cook` adds to an
[AERON_flight_model](../AERON_flight_model/README.md) asset for the game:
where its textures are, precomputed simulation and render data, and its
baked probes. Only the cooker writes it, and only cooked files carry it. A
cooked file lists it in `extensionsRequired` (its textures are found only
through it) and both extensions in `extensionsUsed`; a source file has
neither this extension nor its fields. Re-cooking replaces it whole.

It extends:
- the model-root node: `renderData` and `probeData`;
- component nodes: `simulationData`;
- textures: their atlas image and page.

Each object has its schema, with a description of every field:
[node](node.AERON_flight_model_cooked.schema.json) and
[texture](texture.AERON_flight_model_cooked.schema.json).

## Textures and images

Each glTF image is a KTX2 array of atlas pages in a buffer view, for one
channel (base colour, normal, metallic-roughness or emissive: the material
slot that references it). A texture names its image and page through its
`AERON_flight_model_cooked` object, and has no `source` of its own:

- `source`: the image;
- `layer`: the page of it, 0 to 4095;
- `maxLod`: optional, the mip levels the texture may sample (0 to 14): a
  texture packed on a shared page stops there, before its neighbours bleed
  into it. Without it, no limit.

`KHR_texture_transform` on the material's texture info places the texture
within its page.

A model loads an image when a primitive's own material references it, or
when a material variant it shows does (through `KHR_materials_variants`,
for the markings values in use). In a channel no primitive's own material
uses, every image is loaded. The cooker packs the pages that only some
variants use into images of their own, one per set of variants, so that a
model loads only the skins it shows.

## Simulation and render data

Precomputed acceleration data. Loaders verify it in
linear time and build it themselves, with a warning, when it is absent.
Malformed or stale data fails the load.

- Component `simulationData`: `{ "version": 1, "bufferView": N,
  "topologyHash": "<16 lower-case hex digits>" }`. It covers the
  component's simulation topology (the collision child's mesh, otherwise the
  render mesh) as the loader builds it: positions in metres in the Aeron
  frame, degenerate triangles dropped, triangle order preserved.
- Model-root `renderData`: `{ "version": 1, "bufferView": N, "meshHash":
  "<16 lower-case hex digits>" }`. It covers the merged render triangles in
  final index order (opaque, mask, blend).

`topologyHash` is XXH64 (seed 0) of these little-endian values:
`position_count` (u32), every position (3 x f32), `triangle_count` (u32),
then every triangle's three position indices (3 x u32). `meshHash` hashes
the render vertex positions the same way, with the index count (u32) in place
of the triangle count.

The buffer view holds, little-endian and exactly this size:

```text
u32 version = 1, u32 face_count, u32 unique_vertex_count, u32 node_count, u32 sample_count
node[node_count]: f32 min[3], f32 max[3], f32 max_inv_dominant, u32 first_or_child, u32 count
u32 face_order[face_count]
u32 sample[sample_count]
```

Nodes form a bounding volume hierarchy over the triangles. Node 0 is the
root. An internal node (`count == 0`) has its children at `first_or_child`
and `first_or_child + 1`, both after it. A leaf lists `count` triangles of
`face_order` starting at `first_or_child`. Bounds are the raw triangle
bounds. `max_inv_dominant` is the largest `1 / max(|nx|, |ny|, |nz|)` of the
node's triangles: games expand nodes by their own plane tolerance times this
value. Render data stores 0 there, `sample_count = 0`, and the render vertex
count as `unique_vertex_count`.

For simulation data, unique vertices are the topology positions
deduplicated by bitwise equality, first occurrence kept. The sample is up to
256 unique-vertex indices in farthest-point order: the seed is the
lexicographically smallest position, and each step takes the largest minimum
squared distance, lowest index on ties. A game needing fewer samples takes a
prefix.

## probeData

`probeData` holds the probes an authoring tool baked, with the layout they
were baked for, so the images and the layout always agree. `aeron_gltf_cook
--probes <dir>` writes it from a probes bake folder (`bake.json`, the layout;
`probes.ktx2` and `grid.ktx2`, the images).

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

Positions and sizes are in the Aeron frame (the model frame: x, y, z of
the game's model axes) and metres, axis-aligned with it:

- `cubes`: the buffer view of a KTX2 array of square BC6H or RGBA16F cube
  maps, one per probe (layer k: probe k), with mips for roughness;
- `grid`: the buffer view of a KTX2 image of one RGBA16F level, 6 rx by
  ry rz texels: point (x, y, z) at column 6 x + f, row y + ry z holds the
  irradiance / pi toward +x, -x, +y, -y, +z, -z (f = 0 to 5) in RGB and the
  luminance of the mean radiance in A;
- `intensity` (0 to 16), `diffuseStrength` and `specularStrength` (0 to
  4): optional, default 1, the probes' light;
- `gridBox`: `center` and `size` (full extent) of the box the grid lights
  fully; its light fades out within one point spacing outside;
- `gridResolution`: points per axis, 2 to 64, placed at (i + 1) /
  (resolution + 1) of the box;
- `keyLightShadow`: optional, 0 to 1, default 0.7: when the scene's key
  light is in the bake (a hangar's ceiling light), the part of the probe
  light its shadow removes;
- `probes`: 1 to 42 probes: `center`, `size` (full extent; an ellipsoid's
  axes), `shape` (`box` or `ellipsoid`), `falloff` (above 0, at most 1: the
  outer fraction of the influence over which its weight falls to 0),
  `parallaxSize` and `parallaxShape` (the volume its reflections are
  projected on) and optional `parallaxCenter` (default `center`);
- `reflectionPlane`: optional, a floor facing up (a hangar's) that
  reflects what stands on it: `center`, `size` (along x and z) and
  `distance` (0 to 10, how far from it a surface takes its reflection).

Images that do not match the layout fail the load.

Unknown fields are ignored.
