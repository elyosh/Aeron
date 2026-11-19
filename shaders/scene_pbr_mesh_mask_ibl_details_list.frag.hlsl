/* scene_pbr_mesh_mask_ibl_details.frag for instances that have a light list: it loops over the
 * list and skips the cluster lookup (AERON_PBR_LIGHT_LIST in
 * scene_pbr_mesh_impl.hlsli). */
#define AERON_PBR_LIGHT_LIST 1
#include "scene_pbr_mesh_mask_ibl_details.frag.hlsl"
