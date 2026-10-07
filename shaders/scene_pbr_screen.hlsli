/* Screen surfaces of the PBR mesh shader (gltf_mesh.h, "Screen surfaces";
 * docs: dynamic_cockpit.md, 2.5).
 *
 * A screen material shows a game-provided texture pair (graphics and text,
 * premultiplied) through rectangles of its TEXCOORD_0 space. Each rectangle
 * maps its part of the surface onto a per-frame source rectangle of the
 * textures. Holograms port PixelShaderDCHolo.hlsl of xwa_ddraw_d3d11. */

#ifndef SCENE_PBR_SCREEN_HLSLI
#define SCENE_PBR_SCREEN_HLSLI

static const uint SCREEN_COVER = 0x1u;
static const uint SCREEN_TRANSPARENT = 0x2u;
static const uint SCREEN_ALWAYS_VISIBLE = 0x4u;
static const uint SCREEN_HOLOGRAM = 0x8u;
static const uint SCREEN_NOISY = 0x10u;

static const uint SCREEN_LAYER_GRAPHICS = 0x1u;
static const uint SCREEN_LAYER_TEXT = 0x2u;
static const uint SCREEN_LAYER_BAR = 0x4u;

static const uint SCREEN_FRAME_HIDE_TRANSPARENT = 0x1u;
static const uint SCREEN_FRAME_BAR_VISIBLE = 0x2u;

/* XWAU's fill-bar background, 0x101010, linear. */
static const float3 SCREEN_BAR_BACKGROUND = float3(0.00518f, 0.00518f, 0.00518f);

struct ScreenRect
{
    float4 dst;       /* u0, v0, u1, v1 */
    float4 bg_gain;   /* background RGB, content gain */
    float4 highlight; /* background RGB while the reticle is highlighted */
    float4 lock;      /* background RGB while a warhead lock is solid */
    float4 bar;       /* fill-bar RGB */
    uint4  info;      /* layers, source */
};

StructuredBuffer<ScreenRect> g_screen_rects : register(AERON_PBR_SCREEN_RECT_REGISTER, space2);
/* AeronSceneScreenFrame: rows 0-2 header, then one row per source. */
StructuredBuffer<float4> g_screen_frame : register(AERON_PBR_SCREEN_FRAME_REGISTER, space2);
#ifndef AERON_PBR_SCREEN_GRAPHICS_TEXTURE_REGISTER
#define AERON_PBR_SCREEN_GRAPHICS_TEXTURE_REGISTER t10
#define AERON_PBR_SCREEN_GRAPHICS_SAMPLER_REGISTER s10
#define AERON_PBR_SCREEN_TEXT_TEXTURE_REGISTER t11
#define AERON_PBR_SCREEN_TEXT_SAMPLER_REGISTER s11
#endif
Texture2D<float4> g_screen_graphics : register(AERON_PBR_SCREEN_GRAPHICS_TEXTURE_REGISTER, space2);
SamplerState g_screen_graphics_sampler : register(AERON_PBR_SCREEN_GRAPHICS_SAMPLER_REGISTER, space2);
Texture2D<float4> g_screen_text : register(AERON_PBR_SCREEN_TEXT_TEXTURE_REGISTER, space2);
SamplerState g_screen_text_sampler : register(AERON_PBR_SCREEN_TEXT_SAMPLER_REGISTER, space2);

struct ScreenFrame
{
    uint2  valid;
    uint   color_state;
    uint   flags;
    float  display_strength;
    float  bar_level;
    float  holo_time;
    float  holo_fade;
    float4 holo_color; /* linear RGB, emissive multiplier */
};

ScreenFrame screen_load_frame()
{
    float4 row0 = g_screen_frame[0];
    float4 row1 = g_screen_frame[1];
    ScreenFrame f;
    f.valid = asuint(row0.xy);
    f.color_state = asuint(row0.z);
    f.flags = asuint(row0.w);
    f.display_strength = row1.x;
    f.bar_level = row1.y;
    f.holo_time = row1.z;
    f.holo_fade = row1.w;
    f.holo_color = g_screen_frame[2];
    return f;
}

bool screen_source_valid(ScreenFrame f, uint source)
{
    uint word = source < 32u ? f.valid.x : f.valid.y;
    return ((word >> (source & 31u)) & 1u) != 0u;
}

float4 screen_source(uint source)
{
    return g_screen_frame[3u + source];
}

/* The rectangle-local coordinate of `uv`, and the source UV it shows when
 * inside the source rectangle. */
bool screen_rect_lookup(ScreenRect r, float2 uv, out float2 st, out float2 source_uv)
{
    st = (uv - r.dst.xy) / (r.dst.zw - r.dst.xy);
    float4 s = screen_source(r.info.y);
    source_uv = lerp(s.xy, s.zw, st);
    return all(source_uv >= min(s.xy, s.zw)) && all(source_uv <= max(s.xy, s.zw));
}

/* Linear screen radiance of a screen material (PixelShaderDC.hlsl's HUD
 * part): the first rectangle's background outside every rectangle; inside
 * one, its content (text over graphics, layers per rectangle, scaled by the
 * gain) over its background. */
float3 screen_evaluate(GltfMaterial m, float2 uv_in, ScreenFrame f)
{
    float2 uv = frac(uv_in);
    float3 screen = float3(0.0f, 0.0f, 0.0f);
    [loop]
    for (uint k = 0u; k < m.screen_count; ++k) {
        ScreenRect r = g_screen_rects[m.screen_first + k];
        float3 bg = f.color_state == 2u ? r.lock.rgb
                  : f.color_state == 1u ? r.highlight.rgb
                                        : r.bg_gain.rgb;
        if (k == 0u)
            screen = bg;
        uint layers = r.info.x;
        if ((layers & SCREEN_LAYER_BAR) != 0u) {
            float2 st = (uv - r.dst.xy) / (r.dst.zw - r.dst.xy);
            if ((f.flags & SCREEN_FRAME_BAR_VISIBLE) == 0u || any(st < 0.0f) || any(st > 1.0f))
                continue;
            screen = (st.y >= 1.0f - f.bar_level ? r.bar.rgb : SCREEN_BAR_BACKGROUND) * r.bg_gain.w;
            continue;
        }
        if (!screen_source_valid(f, r.info.y))
            continue;
        float2 st, source_uv;
        if (!screen_rect_lookup(r, uv, st, source_uv))
            continue;
        float4 g = (layers & SCREEN_LAYER_GRAPHICS) != 0u
                       ? g_screen_graphics.SampleLevel(g_screen_graphics_sampler, source_uv, 0.0f)
                       : float4(0.0f, 0.0f, 0.0f, 0.0f);
        float4 t = (layers & SCREEN_LAYER_TEXT) != 0u
                       ? g_screen_text.SampleLevel(g_screen_text_sampler, source_uv, 0.0f)
                       : float4(0.0f, 0.0f, 0.0f, 0.0f);
        float4 c = t + g * (1.0f - t.a);
        screen = bg * (1.0f - c.a) + c.rgb * r.bg_gain.w;
    }
    return screen * f.display_strength;
}

/* ===== Hologram (PixelShaderDCHolo.hlsl) ============================ */

/* Value noise of shadertoy 4sfGzS. */
float screen_hash(float3 p)
{
    p = frac(p * 0.3183099f + 0.1f);
    p *= 17.0f;
    return frac(p.x * p.y * p.z * (p.x + p.y + p.z));
}

float screen_noise(float3 x)
{
    float3 i = floor(x);
    float3 f = frac(x);
    f = f * f * (3.0f - 2.0f * f);
    return lerp(lerp(lerp(screen_hash(i + float3(0, 0, 0)), screen_hash(i + float3(1, 0, 0)), f.x),
                     lerp(screen_hash(i + float3(0, 1, 0)), screen_hash(i + float3(1, 1, 0)), f.x), f.y),
                lerp(lerp(screen_hash(i + float3(0, 0, 1)), screen_hash(i + float3(1, 0, 1)), f.x),
                     lerp(screen_hash(i + float3(0, 1, 1)), screen_hash(i + float3(1, 1, 1)), f.x), f.y),
                f.z);
}

float screen_sd_box(float2 p, float2 b)
{
    float2 d = abs(p) - b;
    return length(max(d, 0.0f)) + min(max(d.x, d.y), 0.0f);
}

/* Straight sRGB colour of a premultiplied linear sample. */
float3 screen_straight_srgb(float4 c)
{
    return c.a > 1.0e-4f ? AeronLinearToSrgb(saturate(c.rgb / c.a)) : float3(0.0f, 0.0f, 0.0f);
}

/* Linear colour and straight alpha of a hologram. XWAU computes it in
 * display (sRGB) values; so does this, converting once at the end. */
float4 screen_hologram(GltfMaterial m, float2 uv_in, ScreenFrame f)
{
    float2 uv = frac(uv_in);
    float  t = f.holo_time;
    float  noisy = (m.screen_flags & SCREEN_NOISY) != 0u ? 1.0f : 0.0f;

    /* VCR shake: a horizontal wobble inside a band scrolling down every 4 s. */
    float2 look = uv;
    float  band = look.y - frac(t / 4.0f);
    float  window = 1.0f / (1.0f + 20.0f * band * band);
    float  on_off = step(0.3f, sin(t + 4.0f * cos(t * 4.0f)));
    look.x += (0.1f + 0.2f * noisy) * sin(look.y * 10.0f + t) / 50.0f * on_off * (1.0f + cos(t * 80.0f)) *
              window;

    /* Panel: HUD colour with a rounded falloff, optional scanlines. */
    float4 bg = float4(AeronLinearToSrgb(saturate(f.holo_color.rgb)), 0.9f);
    float  d = screen_sd_box(look - 0.5f, float2(0.32f, 0.32f));
    float  dout = smoothstep(0.0f, 0.1f, d - 0.07f);
    if (noisy != 0.0f)
        bg *= 0.8f + 0.2f * sin(25.0f * t + look.y * 250.0f);
    bg.a = 0.9f * lerp(bg.a, 0.0f, dout);
    bg.rgb *= 0.2f * (0.5f + 0.5f * screen_noise(150.0f * float3(uv, t)));

    /* Content at the shaken UV, text boosted. */
    float4 hud = bg;
    float  content = 0.0f;
    [loop]
    for (uint k = 0u; k < m.screen_count; ++k) {
        ScreenRect r = g_screen_rects[m.screen_first + k];
        if ((r.info.x & SCREEN_LAYER_BAR) != 0u || !screen_source_valid(f, r.info.y))
            continue;
        float2 st, source_uv;
        if (!screen_rect_lookup(r, look, st, source_uv))
            continue;
        float4 g = g_screen_graphics.SampleLevel(g_screen_graphics_sampler, source_uv, 0.0f);
        float4 tx = g_screen_text.SampleLevel(g_screen_text_sampler, source_uv, 0.0f);
        float  text_alpha = saturate(2.0f * tx.a);
        float4 c = float4(lerp(screen_straight_srgb(g), screen_straight_srgb(tx), text_alpha),
                          saturate(max(g.a, text_alpha)));
        content = c.a;
        hud = lerp(bg, c, c.a);
    }

    /* Fade: black speckle noise resolving into the image. */
    float fade = saturate(f.holo_fade);
    if (fade < 1.0f) {
        float  size = 10.0f + 30.0f * fade;
        float  speckle = 3.0f * (screen_noise(size * float3(uv, 20.0f * fade)) - 0.5f);
        float4 video = hud * lerp(speckle, 1.0f, fade);
        hud = lerp(float4(0.0f, 0.0f, 0.0f, 0.0f), video, saturate(fade * 3.0f));
    }

    float gain = (content > 0.5f ? 1.7f : 1.0f) * f.holo_color.a;
    return float4(AeronSrgbToLinear(saturate(hud.rgb)) * gain, saturate(hud.a));
}

#endif /* SCENE_PBR_SCREEN_HLSLI */
