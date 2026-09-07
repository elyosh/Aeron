#include "aeron/asset/decode_types.h"
#include <stdlib.h>
#include <string.h>

void AeronIndexedFrame_Free(AeronIndexedFrame* frame) {
	if (!frame)
		return;
	free(frame->indices);
	free(frame->coverage);
	memset(frame, 0, sizeof *frame);
}

void AeronIndexedFrames_Free(AeronIndexedFrames* frames) {
	if (!frames)
		return;
	for (uint16_t i = 0; i < frames->count; ++i)
		AeronIndexedFrame_Free(&frames->frames[i]);
	free(frames->frames);
	memset(frames, 0, sizeof *frames);
}

void AeronDecodedFont_Free(AeronDecodedFont* font) {
	if (!font)
		return;
	free(font->foreground);
	free(font->shadow);
	free(font->glyphs);
	memset(font, 0, sizeof *font);
}
