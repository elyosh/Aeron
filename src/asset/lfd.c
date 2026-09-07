#include "aeron/asset/lfd.h"
#include "decode_internal.h"

#include <stdlib.h>
#include <string.h>

void AeronLfd_Free(AeronLfd* panel) {
	if (!panel)
		return;
	free(panel->entries);
	memset(panel, 0, sizeof *panel);
}

bool AeronLfd_Parse(const void* data, size_t size, AeronLfd* out, AeronDecodeError* error) {
	const uint8_t* bytes = data;
	if (out)
		memset(out, 0, sizeof *out);
	if (!bytes || !out || size < 16 || size > AERON_DECODE_MAX_ENTRY_SIZE)
		return decode_error(error, 20, "invalid LFD size");
	size_t   offset = 0;
	uint32_t count  = 0;
	while (offset < size) {
		if (size - offset < 16)
			return decode_error(error, 21, "truncated LFD entry header");
		const uint32_t entry_size = decode_u32(bytes + offset + 12);
		if ((size_t)entry_size > size - offset - 16)
			return decode_error(error, 22, "LFD entry %u is truncated", count);
		offset += 16 + (size_t)entry_size;
		if (++count > 64)
			return decode_error(error, 23, "LFD has too many entries");
	}
	AeronLfdEntry* entries = calloc(count, sizeof *entries);
	if (!entries)
		return decode_error(error, 24, "LFD allocation failed");
	offset = 0;
	for (uint32_t index = 0; index < count; ++index) {
		AeronLfdEntry* entry = &entries[index];
		entry->type =
			AERON_LFD_FOURCC(bytes[offset], bytes[offset + 1], bytes[offset + 2], bytes[offset + 3]);
		memcpy(entry->name, bytes + offset + 4, 8);
		entry->name[8] = '\0';
		entry->size    = decode_u32(bytes + offset + 12);
		entry->data    = bytes + offset + 16;
		offset += 16 + entry->size;
	}
	out->entries = entries;
	out->count   = count;
	return true;
}

const AeronLfdEntry* AeronLfd_Find(const AeronLfd* panel, uint32_t type) {
	if (!panel)
		return NULL;
	const AeronLfdEntry* found = NULL;
	for (uint32_t index = 0; index < panel->count; ++index) {
		if (panel->entries[index].type != type)
			continue;
		if (found)
			return NULL;
		found = &panel->entries[index];
	}
	return found;
}
