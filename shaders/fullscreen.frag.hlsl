#include "srgb.hlsli"

Texture2D<float4> g_frameTexture : register(t0, space2);
SamplerState      g_frameSampler : register(s0, space2);

cbuffer FragmentUniform : register(b0, space3) {
	float4 params; /* x: decode sRGB source to linear; y: sharp sampling; z: output RGB scale */
	float4 tint;   /* RGBA multiplier applied after decode (1,1,1,1 = untinted) */
	float4 bias;   /* additive RGB weighted by sample alpha (fade-to-color) */
};

float2 AeronSharpBilinearTexcoord(float2 texcoord) {
	uint texWidth;
	uint texHeight;
	g_frameTexture.GetDimensions(texWidth, texHeight);

	float2 texSize           = float2((float)texWidth, (float)texHeight);
	float2 texelsPerDstPixel = abs(float2(ddx(texcoord.x), ddy(texcoord.y))) * texSize;
	float2 dstPixelsPerTexel = 1.0 / max(texelsPerDstPixel, float2(0.000001, 0.000001));
	float2 integerScale      = round(dstPixelsPerTexel);
	float2 integerScaleError = abs(dstPixelsPerTexel - integerScale);
	float2 texelCoord        = texcoord * texSize;
	float2 texelBase         = floor(texelCoord);
	float2 texelFract        = frac(texelCoord);
	float2 blendRegion       = max(float2(0.0, 0.0), 0.5 - 0.5 / max(dstPixelsPerTexel, float2(1.0, 1.0)));
	float2 centerDistance    = texelFract - 0.5;
	float2 sharpenedFract    = (centerDistance - clamp(centerDistance, -blendRegion, blendRegion)) *
								   max(dstPixelsPerTexel, float2(1.0, 1.0)) +
							   0.5;

	if (integerScale.x >= 1.0 && integerScale.y >= 1.0 && integerScaleError.x < 0.001 &&
		integerScaleError.y < 0.001) {
		return (texelBase + 0.5) / texSize;
	}

	return dstPixelsPerTexel.x > 1.0 && dstPixelsPerTexel.y > 1.0 ? (texelBase + sharpenedFract) / texSize
																  : texcoord;
}

float4 main(float4 position : SV_Position, float2 texcoord : TEXCOORD0) : SV_Target0 {
	float  unusedValue    = position.x * 0.0;
	float2 sampleTexcoord = params.y != 0.0 ? AeronSharpBilinearTexcoord(texcoord) : texcoord;
	float4 color          = g_frameTexture.Sample(g_frameSampler, sampleTexcoord);

	if (params.x != 0.0) {
		/* Decode the sRGB-encoded (display-space) source to linear using the exact
		 * piecewise sRGB curve, so it is the precise inverse of the sRGB swapchain's
		 * hardware encode. A pow(x, 2.2) approximation crushes shadows (its inverse
		 * differs from the sRGB encode in the low range), raising contrast. */
		color.rgb = AeronSrgbToLinear(saturate(color.rgb));
	}

	float4 outputColor = color * tint + float4(bias.rgb * color.a, 0.0);
	outputColor.rgb *= params.z;
	return outputColor + float4(unusedValue, 0.0, 0.0, 0.0);
}
