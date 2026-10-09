#ifndef AERON_SCENE_MATERIAL_ANIM_LAYOUT_H
#define AERON_SCENE_MATERIAL_ANIM_LAYOUT_H

/*
 * Material animation constants shared between C (aeron/scene/mesh_common.h)
 * and the shaders (material_anim.hlsli). Preprocessor only, like
 * mesh_table_layout.hlsli.
 *
 * Player states, in priority order; a flipbook's state is one of them or
 * AERON_ANIM_STATE_ALWAYS, which is always active.
 *
 * Per-mesh animation buffer (StructuredBuffer<uint4>), records:
 *   header (record 0): reserved, 0, 0, 0
 *   flipbook: x state | layer << 8 | mode << 9 | has_uv << 10,
 *             y first frame record, z frame count, w asuint(total seconds)
 *   flipbook uv (when has_uv, right after it): asuint(scale.x),
 *             asuint(offset.x), asuint(offset.y), clamp
 *   frame:    x atlas layer | lod << 12 (layer AERON_ANIM_NO_LAYER: not in
 *             its library, the static material shows), y rect x | y << 16,
 *             z rect w | h << 16 (texels of an effect page),
 *             w asuint(end time, seconds)
 *   An illumination frame has a second record: the emissive image, same
 *   x, y, z, and w 0.
 * Frames of one flipbook are consecutive, AERON_ANIM_FRAME_RECORDS(layer)
 * records each.
 *   damage alternative (AERON_ANIM_DAMAGE_RECORDS records): three frame
 *             records for the 75, 50 and 25 % stages (layer
 *             AERON_ANIM_NO_LAYER: no damage at that stage), then
 *             x asuint(scale min), y asuint(scale max), z has scale,
 *             w offset mode (AERON_ANIM_OFFSET_*), then
 *             asuint of the offset range x0, y0, x1, y1.
 *   A material's alternatives are consecutive.
 */

#define AERON_ANIM_STATE_THREAT_LASER 0
#define AERON_ANIM_STATE_THREAT_TURRET 1
#define AERON_ANIM_STATE_THREAT_BEAM 2
#define AERON_ANIM_STATE_THREAT_MISSILE_LOCKING 3
#define AERON_ANIM_STATE_THREAT_MISSILE_LOCKED 4
#define AERON_ANIM_STATE_CANNON_1 5 /* cannon.1 .. cannon.8: 5 .. 12 */
#define AERON_ANIM_STATE_BROKEN_CMD 13
#define AERON_ANIM_STATE_BROKEN_LASER_ION 14
#define AERON_ANIM_STATE_BROKEN_BEAM_WEAPON 15
#define AERON_ANIM_STATE_BROKEN_SHIELDS 16
#define AERON_ANIM_STATE_BROKEN_THROTTLE 17
#define AERON_ANIM_STATE_BROKEN_SENSORS 18
#define AERON_ANIM_STATE_BROKEN_LASER_RECHARGE 19
#define AERON_ANIM_STATE_BROKEN_ENGINE_POWER 20
#define AERON_ANIM_STATE_BROKEN_SHIELD_RECHARGE 21
#define AERON_ANIM_STATE_BROKEN_BEAM_RECHARGE 22
#define AERON_ANIM_STATE_HULL_25 23
#define AERON_ANIM_STATE_HULL_50 24
#define AERON_ANIM_STATE_HULL_75 25
#define AERON_ANIM_STATE_LASER_LOCK 26
#define AERON_ANIM_STATE_WARHEAD_LOCKING 27
#define AERON_ANIM_STATE_WARHEAD_LOCKED 28
#define AERON_ANIM_STATE_HANGAR 29
#define AERON_ANIM_STATE_TARGET 30
#define AERON_ANIM_STATE_ALWAYS 31

#define AERON_ANIM_LAYER_COLOUR 0
#define AERON_ANIM_LAYER_ILLUMINATION 1
#define AERON_ANIM_MODE_LOOP 0
#define AERON_ANIM_MODE_ONCE 1

#define AERON_ANIM_FLIPBOOK_LAYER_SHIFT 8
#define AERON_ANIM_FLIPBOOK_MODE_SHIFT 9
#define AERON_ANIM_FLIPBOOK_HAS_UV 0x400u
#define AERON_ANIM_NO_LAYER 0xFFFu
#define AERON_ANIM_FRAME_RECORDS(layer) ((layer) == AERON_ANIM_LAYER_ILLUMINATION ? 2u : 1u)

#define AERON_ANIM_DAMAGE_RECORDS 5u
#define AERON_ANIM_DAMAGE_STAGES 3u
#define AERON_ANIM_OFFSET_FIXED 0u  /* FIXED_LOC */
#define AERON_ANIM_OFFSET_INSIDE 1u /* RAND_LOC */
#define AERON_ANIM_OFFSET_RANGE 2u  /* RAND_LOC=(x0,y0,x1,y1) and the default */

#endif /* AERON_SCENE_MATERIAL_ANIM_LAYOUT_H */
