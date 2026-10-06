/*
 * Back-face fill fragment shader for the glTF (PBR) mesh path.
 *
 * Paired with scene_pbr_backface.vert. Drawn after the opaque meshes with
 * their visible faces culled instead of their back faces, and a strict,
 * biased depth test, so a fragment survives only
 * where no front face covers it: through hairline cracks between authored
 * surfaces and into open shells. There it shows a dark interior instead of
 * the background behind the model.
 */

float4 main() : SV_Target0
{
	return float4(0.004f, 0.004f, 0.0045f, 1.0f);
}
