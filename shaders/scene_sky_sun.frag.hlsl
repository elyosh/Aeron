/*
 * Scene sky-sun fragment shader (AeronScene_AddSkySun, disc visual).
 *
 * A limb-darkened disc of angular radius R with an antialiased edge, plus
 * an optional glow falling off as (1 - t)^2 from R to the glow radius G.
 * Coordinates are tangents of the angle from the sun's centre. Radiances
 * are prepared host-side: the disc's mean radiance equals the light's
 * irradiance over the disc's solid angle, and the glow integrates to the
 * requested fraction of it. Drawn additively.
 */

cbuffer SkySunFS : register(b0, space3)
{
    float4 disc;   /* rgb = centre radiance, a = limb-darkening coefficient */
    float4 glow;   /* rgb = peak glow radiance */
    float4 radii;  /* x = disc radius R, y = glow radius G (tan units) */
};

float4 main(float4 pos : SV_Position, float2 tangent_coords : TEXCOORD0) : SV_Target
{
    float r     = length(tangent_coords);
    float width = max(fwidth(r), 1.0e-7f);
    float cover = saturate((radii.x - r) / width + 0.5f);

    float rel  = saturate(r / radii.x);
    float mu   = sqrt(saturate(1.0f - rel * rel));
    float limb = 1.0f - disc.a * (1.0f - mu);
    float3 color = disc.rgb * limb * cover;

    if (radii.y > radii.x) {
        float t = saturate((r - radii.x) / (radii.y - radii.x));
        color += glow.rgb * (1.0f - t) * (1.0f - t) * (1.0f - cover);
    }
    return float4(color, 1.0f);
}
