/* A flat rectangle placed in 3D, for the virtual screen and the panel drawn
 * into a headset's eye image. The four corners are given in clip space, in
 * the order top left, top right, bottom left, bottom right, and drawn as a
 * triangle strip. Their w makes the texture coordinates right in
 * perspective. */
cbuffer VertexUniform : register(b0, space1) {
	float4 g_corners[4];
};

struct VSOut {
	float4 position : SV_Position;
	float2 uv : TEXCOORD0;
};

VSOut main(uint vertexId : SV_VertexID) {
	VSOut output;
	output.position = g_corners[vertexId];
	output.uv       = float2((float)(vertexId & 1u), (float)(vertexId >> 1u));
	return output;
}
