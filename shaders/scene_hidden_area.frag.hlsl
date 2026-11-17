/* The hidden-area mesh only writes depth (scene_hidden_area.vert.hlsl). */
float4 main() : SV_Target0 {
	return float4(0.0, 0.0, 0.0, 0.0);
}
