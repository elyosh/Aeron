/*
 * The hidden-area mesh of a headset's eye (src/scene/view_area.c): the
 * part of the eye image that the lens never shows. The vertices are already
 * in clip space. They are drawn at the near depth, so everything drawn
 * after them fails the depth test there.
 */
float4 main(float2 position : TEXCOORD0) : SV_Position {
	return float4(position, 1.0, 1.0);
}
