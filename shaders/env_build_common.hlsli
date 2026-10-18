/* These functions give the cube texel directions for the environment
 * builder's filters, as tools/imgbake/envmap.c does. Faces are +X, -X, +Y, -Y, +Z, -Z, and (s, t) run
 * from -1 to 1 across a face, t downwards. */

#ifndef ENV_BUILD_COMMON_HLSLI
#define ENV_BUILD_COMMON_HLSLI

static const float ENV_BUILD_PI = 3.14159265358979323846f;

float3 env_face_direction(int face, float s, float t)
{
    if (face == 0)
        return float3(1.0f, -t, -s);
    if (face == 1)
        return float3(-1.0f, -t, s);
    if (face == 2)
        return float3(s, 1.0f, t);
    if (face == 3)
        return float3(s, -1.0f, -t);
    if (face == 4)
        return float3(s, -t, 1.0f);
    return float3(-s, -t, -1.0f);
}

/* Returns the direction through the centre of texel (x, y) of a `size`
 * face. */
float3 env_texel_direction(int face, int x, int y, int size)
{
    float s = 2.0f * ((float)x + 0.5f) / (float)size - 1.0f;
    float t = 2.0f * ((float)y + 0.5f) / (float)size - 1.0f;
    return normalize(env_face_direction(face, s, t));
}

#endif
