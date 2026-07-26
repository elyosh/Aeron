struct VertexOutput {
	float4 position : SV_Position;
	float2 texcoord : TEXCOORD0;
};

VertexOutput main(uint vertexId : SV_VertexID) {
	VertexOutput output;
	float2       positions[3] = {
		float2(-1.0, -1.0),
		float2(-1.0, 3.0),
		float2(3.0, -1.0),
	};
	float2 texcoords[3] = {
		float2(0.0, 1.0),
		float2(0.0, -1.0),
		float2(2.0, 1.0),
	};

	output.position = float4(positions[vertexId], 0.0, 1.0);
	output.texcoord = texcoords[vertexId];
	return output;
}
