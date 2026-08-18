#include "vfs_iso9660.h"

#include "internal.h"

#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
	ISO_LOGICAL_SECTOR_SIZE  = 2048,
	ISO_RAW_SECTOR_SIZE      = 2352,
	ISO_RAW_PAYLOAD_OFFSET   = 16,
	ISO_READ_BATCH_SECTORS   = 16,
	ISO_MAX_ENTRIES          = 8192,
	ISO_MAX_DIRECTORY_BYTES  = 32 * 1024 * 1024,
	ISO_MAX_DIRECTORY_DEPTH  = 32,
	ISO_MAX_DESCRIPTOR_BYTES = 64 * 1024
};

typedef struct AeronIsoSource {
	char     image_path[AERON_MAX_PATH];
	uint64_t image_size;
	uint64_t track_offset;
	uint32_t physical_sector_size;
	uint32_t payload_offset;
} AeronIsoSource;

typedef struct AeronIsoEntry {
	char*    path;
	uint32_t extent;
	uint32_t size;
	int      is_directory;
} AeronIsoEntry;

struct AeronIso9660 {
	AeronIsoSource source;
	AeronIsoEntry* entries;
	size_t         entry_count;
	size_t         entry_capacity;
	uint32_t       volume_sectors;
	uint32_t*      visited_directories;
	size_t         visited_count;
	size_t         visited_capacity;
};

typedef struct AeronIsoFile {
	AeronIsoSource source;
	SDL_IOStream*  image;
	uint64_t       logical_offset;
	uint64_t       size;
	uint64_t       position;
	uint8_t*       scratch;
	uint64_t       cached_first_sector;
	size_t         cached_sector_count;
} AeronIsoFile;

static uint16_t iso_le16(const uint8_t* value) {
	return (uint16_t)value[0] | (uint16_t)((uint16_t)value[1] << 8);
}

static uint16_t iso_be16(const uint8_t* value) {
	return (uint16_t)((uint16_t)value[0] << 8) | (uint16_t)value[1];
}

static uint32_t iso_le32(const uint8_t* value) {
	return (uint32_t)value[0] | ((uint32_t)value[1] << 8) | ((uint32_t)value[2] << 16) |
		   ((uint32_t)value[3] << 24);
}

static uint32_t iso_be32(const uint8_t* value) {
	return ((uint32_t)value[0] << 24) | ((uint32_t)value[1] << 16) | ((uint32_t)value[2] << 8) |
		   (uint32_t)value[3];
}

static int iso_seek_and_read(SDL_IOStream* stream, uint64_t offset, void* destination, size_t size) {
	if (!stream || offset > INT64_MAX || SDL_SeekIO(stream, (Sint64)offset, SDL_IO_SEEK_SET) < 0)
		return 0;
	return SDL_ReadIO(stream, destination, size) == size;
}

static int iso_read_logical(const AeronIsoSource* source, SDL_IOStream* stream, uint64_t offset,
							void* destination, size_t size, uint8_t* scratch) {
	uint8_t* output = (uint8_t*)destination;
	if (!source || !stream || (!destination && size != 0))
		return 0;
	if (source->physical_sector_size == ISO_LOGICAL_SECTOR_SIZE) {
		if (offset > UINT64_MAX - source->track_offset ||
			source->track_offset + offset > source->image_size ||
			size > source->image_size - (source->track_offset + offset))
			return 0;
		return iso_seek_and_read(stream, source->track_offset + offset, destination, size);
	}

	/* MODE1/2352 stores each 2048-byte logical block between a physical
	 * sector header and its error-correction tail. */
	while (size != 0) {
		const uint64_t sector = offset / ISO_LOGICAL_SECTOR_SIZE;
		const size_t   inside = (size_t)(offset % ISO_LOGICAL_SECTOR_SIZE);
		if (inside == 0 && size >= ISO_LOGICAL_SECTOR_SIZE && scratch) {
			size_t sectors = size / ISO_LOGICAL_SECTOR_SIZE;
			if (sectors > ISO_READ_BATCH_SECTORS)
				sectors = ISO_READ_BATCH_SECTORS;
			const size_t   physical_size = sectors * ISO_RAW_SECTOR_SIZE;
			const uint64_t physical      = source->track_offset + sector * ISO_RAW_SECTOR_SIZE;
			if (physical > source->image_size || physical_size > source->image_size - physical ||
				!iso_seek_and_read(stream, physical, scratch, physical_size))
				return 0;
			for (size_t index = 0; index < sectors; ++index) {
				memcpy(output + index * ISO_LOGICAL_SECTOR_SIZE,
					   scratch + index * ISO_RAW_SECTOR_SIZE + ISO_RAW_PAYLOAD_OFFSET,
					   ISO_LOGICAL_SECTOR_SIZE);
			}
			const size_t logical_size = sectors * ISO_LOGICAL_SECTOR_SIZE;
			offset += logical_size;
			output += logical_size;
			size -= logical_size;
			continue;
		}

		const size_t   available = ISO_LOGICAL_SECTOR_SIZE - inside;
		const size_t   chunk     = size < available ? size : available;
		const uint64_t physical =
			source->track_offset + sector * ISO_RAW_SECTOR_SIZE + ISO_RAW_PAYLOAD_OFFSET + inside;
		if (physical > source->image_size || chunk > source->image_size - physical ||
			!iso_seek_and_read(stream, physical, output, chunk))
			return 0;
		offset += chunk;
		output += chunk;
		size -= chunk;
	}
	return 1;
}

static int iso_path_extension_is(const char* path, const char* extension) {
	const char* actual = path ? strrchr(path, '.') : NULL;
	return actual && SDL_strcasecmp(actual, extension) == 0;
}

static int iso_read_small_file(const char* path, uint8_t** out_data, size_t* out_size) {
	SDL_IOStream* file = SDL_IOFromFile(path, "rb");
	if (!file)
		return 0;
	const Sint64 size = SDL_GetIOSize(file);
	if (size <= 0 || size > ISO_MAX_DESCRIPTOR_BYTES) {
		SDL_CloseIO(file);
		SDL_SetError("disc descriptor has an invalid size");
		return 0;
	}
	uint8_t* data = (uint8_t*)SDL_malloc((size_t)size + 1u);
	if (!data) {
		SDL_CloseIO(file);
		return 0;
	}
	if (SDL_ReadIO(file, data, (size_t)size) != (size_t)size || !SDL_CloseIO(file)) {
		SDL_free(data);
		return 0;
	}
	data[size] = 0;
	*out_data  = data;
	*out_size  = (size_t)size;
	return 1;
}

static char* iso_trim_line(char* line) {
	while (*line && isspace((unsigned char)*line))
		++line;
	char* end = line + strlen(line);
	while (end > line && isspace((unsigned char)end[-1]))
		*--end = '\0';
	return line;
}

static int iso_join_descriptor_file(const char* descriptor, const char* filename, char* destination,
									size_t capacity) {
	const char* slash     = strrchr(descriptor, '/');
	const char* backslash = strrchr(descriptor, '\\');
	if (backslash && (!slash || backslash > slash))
		slash = backslash;
	if (!filename[0] || strchr(filename, '/') || strchr(filename, '\\') || strcmp(filename, ".") == 0 ||
		strcmp(filename, "..") == 0) {
		SDL_SetError("disc descriptor references an unsupported image path");
		return 0;
	}
	const size_t prefix = slash ? (size_t)(slash - descriptor + 1) : 0;
	const int    length = SDL_snprintf(destination, capacity, "%.*s%s", (int)prefix, descriptor, filename);
	if (length < 0 || (size_t)length >= capacity) {
		SDL_SetError("disc image path is too long");
		return 0;
	}
	return 1;
}

static int iso_parse_descriptor(const char* descriptor, AeronIsoSource* source) {
	uint8_t* bytes                      = NULL;
	size_t   size                       = 0;
	char     image_name[AERON_MAX_PATH] = { 0 };
	char     mode[32]                   = { 0 };
	unsigned index_minutes              = 0;
	unsigned index_seconds              = 0;
	unsigned index_frames               = 0;
	int      file_count                 = 0;
	int      track_count                = 0;
	int      have_index                 = 0;
	if (!iso_read_small_file(descriptor, &bytes, &size))
		return 0;

	char* cursor = (char*)bytes;
	char* limit  = (char*)bytes + size;
	while (cursor < limit) {
		char* line = cursor;
		while (cursor < limit && *cursor != '\n' && *cursor != '\r')
			++cursor;
		if (cursor < limit)
			*cursor++ = '\0';
		while (cursor < limit && (*cursor == '\n' || *cursor == '\r'))
			++cursor;
		line                     = iso_trim_line(line);
		const size_t line_length = strlen(line);
		if (line_length > 4 && SDL_strncasecmp(line, "FILE", 4) == 0 && isspace((unsigned char)line[4])) {
			char* quote     = strchr(line + 4, '"');
			char* end_quote = quote ? strchr(quote + 1, '"') : NULL;
			if (!quote || !end_quote || (size_t)(end_quote - quote - 1) >= sizeof image_name) {
				SDL_SetError("disc descriptor has an invalid FILE statement");
				goto fail;
			}
			memcpy(image_name, quote + 1, (size_t)(end_quote - quote - 1));
			image_name[end_quote - quote - 1] = '\0';
			++file_count;
		} else if (line_length > 5 && SDL_strncasecmp(line, "TRACK", 5) == 0 &&
				   isspace((unsigned char)line[5])) {
			unsigned track_number;
			if (sscanf(line + 5, "%u %31s", &track_number, mode) != 2 || track_number == 0) {
				SDL_SetError("disc descriptor has an invalid TRACK statement");
				goto fail;
			}
			++track_count;
		} else if (line_length > 5 && SDL_strncasecmp(line, "INDEX", 5) == 0 &&
				   isspace((unsigned char)line[5])) {
			unsigned index_number;
			if (sscanf(line + 5, "%u %u:%u:%u", &index_number, &index_minutes, &index_seconds,
					   &index_frames) == 4 &&
				index_number == 1)
				have_index = 1;
		}
	}
	/* The storefront descriptors describe one data-only track. Deliberately
	 * reject audio and multi-file layouts rather than guessing their offsets. */
	if (file_count != 1 || track_count != 1 || !have_index || index_seconds >= 60 || index_frames >= 75) {
		SDL_SetError("only single-track MODE1 disc descriptors are supported");
		goto fail;
	}
	if (SDL_strcasecmp(mode, "MODE1/2352") == 0) {
		source->physical_sector_size = ISO_RAW_SECTOR_SIZE;
		source->payload_offset       = ISO_RAW_PAYLOAD_OFFSET;
	} else if (SDL_strcasecmp(mode, "MODE1/2048") == 0) {
		source->physical_sector_size = ISO_LOGICAL_SECTOR_SIZE;
		source->payload_offset       = 0;
	} else {
		SDL_SetError("disc descriptor track is not MODE1/2048 or MODE1/2352");
		goto fail;
	}
	if (!iso_join_descriptor_file(descriptor, image_name, source->image_path, sizeof source->image_path))
		goto fail;
	const uint64_t index_sector = ((uint64_t)index_minutes * 60u + index_seconds) * 75u + index_frames;
	source->track_offset        = index_sector * source->physical_sector_size;
	SDL_free(bytes);
	return 1;

fail:
	SDL_free(bytes);
	return 0;
}

static int iso_open_source(const char* path, AeronIsoSource* source, SDL_IOStream** out_stream) {
	memset(source, 0, sizeof *source);
	if (iso_path_extension_is(path, ".cue") || iso_path_extension_is(path, ".ins")) {
		if (!iso_parse_descriptor(path, source))
			return 0;
	} else {
		if (strlen(path) >= sizeof source->image_path) {
			SDL_SetError("disc image path is too long");
			return 0;
		}
		Aeron_CopyString(source->image_path, sizeof source->image_path, path);
	}

	SDL_IOStream* stream = SDL_IOFromFile(source->image_path, "rb");
	if (!stream)
		return 0;
	const Sint64 image_size = SDL_GetIOSize(stream);
	if (image_size <= 0) {
		SDL_CloseIO(stream);
		SDL_SetError("disc image has an invalid size");
		return 0;
	}
	source->image_size = (uint64_t)image_size;
	if (source->track_offset >= source->image_size) {
		SDL_CloseIO(stream);
		SDL_SetError("disc data track begins beyond the image");
		return 0;
	}
	*out_stream = stream;
	return 1;
}

static int iso_probe_layout(AeronIsoSource* source, SDL_IOStream* stream) {
	uint8_t        descriptor[7];
	const uint32_t layouts[][2] = {
		{ ISO_LOGICAL_SECTOR_SIZE, 0 },
		{ ISO_RAW_SECTOR_SIZE, ISO_RAW_PAYLOAD_OFFSET },
	};
	if (source->physical_sector_size != 0) {
		const uint64_t offset =
			source->track_offset + 16u * source->physical_sector_size + source->payload_offset;
		if (!iso_seek_and_read(stream, offset, descriptor, sizeof descriptor) || descriptor[0] != 1 ||
			memcmp(descriptor + 1, "CD001", 5) != 0 || descriptor[6] != 1) {
			SDL_SetError("disc data track does not contain an ISO9660 primary volume descriptor");
			return 0;
		}
		return 1;
	}

	for (size_t index = 0; index < sizeof layouts / sizeof layouts[0]; ++index) {
		const uint64_t offset = 16u * layouts[index][0] + layouts[index][1];
		if (offset <= source->image_size && sizeof descriptor <= source->image_size - offset &&
			iso_seek_and_read(stream, offset, descriptor, sizeof descriptor) && descriptor[0] == 1 &&
			memcmp(descriptor + 1, "CD001", 5) == 0 && descriptor[6] == 1) {
			source->physical_sector_size = layouts[index][0];
			source->payload_offset       = layouts[index][1];
			return 1;
		}
	}
	SDL_SetError("disc image is not a supported ISO9660 MODE1 image");
	return 0;
}

static int iso_normalize_path(const char* path, char* destination, size_t capacity) {
	size_t      length = 0;
	const char* cursor = path;
	if (!path || !destination || capacity == 0)
		return 0;
	while (*cursor == '/' || *cursor == '\\')
		++cursor;
	while (*cursor) {
		const char* component = cursor;
		while (*cursor && *cursor != '/' && *cursor != '\\')
			++cursor;
		const size_t component_length = (size_t)(cursor - component);
		while (*cursor == '/' || *cursor == '\\')
			++cursor;
		if (component_length == 0 || (component_length == 1 && component[0] == '.'))
			continue;
		if (component_length == 2 && component[0] == '.' && component[1] == '.')
			return 0;
		if (length != 0) {
			if (length + 1 >= capacity)
				return 0;
			destination[length++] = '/';
		}
		if (component_length >= capacity - length)
			return 0;
		memcpy(destination + length, component, component_length);
		length += component_length;
	}
	destination[length] = '\0';
	return 1;
}

static int iso_add_entry(AeronIso9660* iso, const char* path, uint32_t extent, uint32_t size,
						 int is_directory) {
	if (iso->entry_count >= ISO_MAX_ENTRIES) {
		SDL_SetError("ISO9660 image contains too many entries");
		return 0;
	}
	if (iso->entry_count == iso->entry_capacity) {
		size_t         capacity = iso->entry_capacity ? iso->entry_capacity * 2u : 256u;
		AeronIsoEntry* grown    = (AeronIsoEntry*)SDL_realloc(iso->entries, capacity * sizeof *grown);
		if (!grown)
			return 0;
		iso->entries        = grown;
		iso->entry_capacity = capacity;
	}
	char* copy = SDL_strdup(path);
	if (!copy)
		return 0;
	iso->entries[iso->entry_count++] = (AeronIsoEntry) {
		.path         = copy,
		.extent       = extent,
		.size         = size,
		.is_directory = is_directory,
	};
	return 1;
}

static int iso_mark_directory_visited(AeronIso9660* iso, uint32_t extent) {
	for (size_t index = 0; index < iso->visited_count; ++index) {
		if (iso->visited_directories[index] == extent) {
			SDL_SetError("ISO9660 directory cycle or alias detected");
			return 0;
		}
	}
	if (iso->visited_count == iso->visited_capacity) {
		size_t    capacity = iso->visited_capacity ? iso->visited_capacity * 2u : 64u;
		uint32_t* grown    = (uint32_t*)SDL_realloc(iso->visited_directories, capacity * sizeof *grown);
		if (!grown)
			return 0;
		iso->visited_directories = grown;
		iso->visited_capacity    = capacity;
	}
	iso->visited_directories[iso->visited_count++] = extent;
	return 1;
}

static int iso_extent_valid(const AeronIso9660* iso, uint32_t extent, uint32_t size) {
	const uint64_t sectors = ((uint64_t)size + ISO_LOGICAL_SECTOR_SIZE - 1u) / ISO_LOGICAL_SECTOR_SIZE;
	return extent <= iso->volume_sectors && sectors <= (uint64_t)iso->volume_sectors - extent;
}

static int iso_record_values(const uint8_t* record, size_t record_size, uint32_t* extent, uint32_t* size) {
	if (record_size < 34 || record[1] != 0 || iso_le32(record + 2) != iso_be32(record + 6) ||
		iso_le32(record + 10) != iso_be32(record + 14))
		return 0;
	*extent = iso_le32(record + 2);
	*size   = iso_le32(record + 10);
	return 1;
}

static int iso_entry_name(const uint8_t* record, size_t record_size, char* name, size_t capacity) {
	const size_t name_length = record[32];
	if (name_length == 0 || 33u + name_length > record_size || name_length >= capacity)
		return 0;
	memcpy(name, record + 33, name_length);
	name[name_length] = '\0';
	char* version     = strchr(name, ';');
	if (version)
		*version = '\0';
	const size_t length = strlen(name);
	if (length != 0 && name[length - 1] == '.')
		name[length - 1] = '\0';
	if (!name[0] || strchr(name, '/') || strchr(name, '\\'))
		return 0;
	for (const unsigned char* cursor = (const unsigned char*)name; *cursor; ++cursor) {
		if (*cursor < 0x20 || *cursor == 0x7f)
			return 0;
	}
	return 1;
}

static int iso_parse_directory(AeronIso9660* iso, SDL_IOStream* stream, const char* parent, uint32_t extent,
							   uint32_t directory_size, unsigned depth) {
	if (depth > ISO_MAX_DIRECTORY_DEPTH || directory_size == 0 || directory_size > ISO_MAX_DIRECTORY_BYTES ||
		!iso_extent_valid(iso, extent, directory_size) || !iso_mark_directory_visited(iso, extent))
		return 0;
	uint8_t* data    = (uint8_t*)SDL_malloc(directory_size);
	uint8_t* scratch = (uint8_t*)SDL_malloc(ISO_READ_BATCH_SECTORS * ISO_RAW_SECTOR_SIZE);
	if (!data || !scratch ||
		!iso_read_logical(&iso->source, stream, (uint64_t)extent * ISO_LOGICAL_SECTOR_SIZE, data,
						  directory_size, scratch)) {
		SDL_free(scratch);
		SDL_free(data);
		return 0;
	}
	SDL_free(scratch);

	/* Directory records never span logical-sector padding. Recursing here
	 * builds the immutable lookup table used by open, stat, and glob. */
	size_t offset = 0;
	while (offset < directory_size) {
		const size_t record_size = data[offset];
		if (record_size == 0) {
			offset = ((offset / ISO_LOGICAL_SECTOR_SIZE) + 1u) * ISO_LOGICAL_SECTOR_SIZE;
			continue;
		}
		if (record_size > directory_size - offset ||
			record_size > ISO_LOGICAL_SECTOR_SIZE - offset % ISO_LOGICAL_SECTOR_SIZE) {
			SDL_SetError("ISO9660 directory contains an invalid record");
			SDL_free(data);
			return 0;
		}
		const uint8_t* record = data + offset;
		if (record_size < 34 || 33u + record[32] > record_size) {
			SDL_SetError("ISO9660 directory contains a truncated record");
			SDL_free(data);
			return 0;
		}
		if (record[32] == 1 && (record[33] == 0 || record[33] == 1)) {
			offset += record_size;
			continue;
		}
		if ((record[25] & 0x80u) != 0 || record[26] != 0 || record[27] != 0) {
			SDL_SetError("ISO9660 image uses unsupported multi-extent or interleaved files");
			SDL_free(data);
			return 0;
		}
		uint32_t child_extent;
		uint32_t child_size;
		char     name[256];
		char     path[AERON_MAX_PATH];
		if (!iso_record_values(record, record_size, &child_extent, &child_size) ||
			!iso_entry_name(record, record_size, name, sizeof name) ||
			!iso_extent_valid(iso, child_extent, child_size)) {
			SDL_SetError("ISO9660 image contains invalid entry metadata");
			SDL_free(data);
			return 0;
		}
		const int path_length = SDL_snprintf(path, sizeof path, "%s%s%s", parent, parent[0] ? "/" : "", name);
		if (path_length < 0 || (size_t)path_length >= sizeof path) {
			SDL_SetError("ISO9660 entry path is too long");
			SDL_free(data);
			return 0;
		}
		const int is_directory = (record[25] & 0x02u) != 0;
		if (!iso_add_entry(iso, path, child_extent, child_size, is_directory) ||
			(is_directory && !iso_parse_directory(iso, stream, path, child_extent, child_size, depth + 1u))) {
			SDL_free(data);
			return 0;
		}
		offset += record_size;
	}
	SDL_free(data);
	return 1;
}

static int iso_compare_entries(const void* left, const void* right) {
	const AeronIsoEntry* a      = (const AeronIsoEntry*)left;
	const AeronIsoEntry* b      = (const AeronIsoEntry*)right;
	const int            folded = SDL_strcasecmp(a->path, b->path);
	return folded != 0 ? folded : strcmp(a->path, b->path);
}

static int iso_parse_volume(AeronIso9660* iso, SDL_IOStream* stream) {
	uint8_t descriptor[ISO_LOGICAL_SECTOR_SIZE];
	uint8_t scratch[ISO_READ_BATCH_SECTORS * ISO_RAW_SECTOR_SIZE];
	int     found_primary = 0;
	for (uint32_t sector = 16; sector < 80; ++sector) {
		if (!iso_read_logical(&iso->source, stream, (uint64_t)sector * ISO_LOGICAL_SECTOR_SIZE, descriptor,
							  sizeof descriptor, scratch))
			return 0;
		if (memcmp(descriptor + 1, "CD001", 5) != 0 || descriptor[6] != 1) {
			SDL_SetError("disc image has an invalid ISO9660 volume descriptor");
			return 0;
		}
		if (descriptor[0] == 1) {
			found_primary = 1;
			break;
		}
		if (descriptor[0] == 255)
			break;
	}
	if (!found_primary || iso_le32(descriptor + 80) != iso_be32(descriptor + 84) ||
		iso_le16(descriptor + 128) != iso_be16(descriptor + 130) ||
		iso_le16(descriptor + 128) != ISO_LOGICAL_SECTOR_SIZE) {
		SDL_SetError("disc image has an unsupported ISO9660 primary volume descriptor");
		return 0;
	}
	iso->volume_sectors = iso_le32(descriptor + 80);
	const uint64_t available_sectors =
		(iso->source.image_size - iso->source.track_offset) / iso->source.physical_sector_size;
	if (iso->volume_sectors == 0 || iso->volume_sectors > available_sectors) {
		SDL_SetError("ISO9660 volume extends beyond the disc image");
		return 0;
	}
	uint32_t     root_extent;
	uint32_t     root_size;
	const size_t root_record_size = descriptor[156];
	if (!iso_record_values(descriptor + 156, root_record_size, &root_extent, &root_size) ||
		!iso_extent_valid(iso, root_extent, root_size) ||
		!iso_add_entry(iso, "", root_extent, root_size, 1) ||
		!iso_parse_directory(iso, stream, "", root_extent, root_size, 0))
		return 0;

	qsort(iso->entries, iso->entry_count, sizeof *iso->entries, iso_compare_entries);
	for (size_t index = 1; index < iso->entry_count; ++index) {
		if (SDL_strcasecmp(iso->entries[index - 1].path, iso->entries[index].path) == 0) {
			SDL_SetError("ISO9660 image contains ambiguous case-insensitive paths");
			return 0;
		}
	}
	return 1;
}

AeronIso9660* AeronIso9660_Open(const char* source_path) {
	if (!source_path || !source_path[0]) {
		SDL_SetError("disc image path is empty");
		return NULL;
	}
	AeronIso9660* iso    = (AeronIso9660*)SDL_calloc(1, sizeof *iso);
	SDL_IOStream* stream = NULL;
	if (!iso || !iso_open_source(source_path, &iso->source, &stream) ||
		!iso_probe_layout(&iso->source, stream) || !iso_parse_volume(iso, stream)) {
		if (stream)
			SDL_CloseIO(stream);
		AeronIso9660_Destroy(iso);
		return NULL;
	}
	if (!SDL_CloseIO(stream)) {
		AeronIso9660_Destroy(iso);
		return NULL;
	}
	SDL_free(iso->visited_directories);
	iso->visited_directories = NULL;
	iso->visited_count       = 0;
	iso->visited_capacity    = 0;
	return iso;
}

void AeronIso9660_Destroy(AeronIso9660* iso) {
	if (!iso)
		return;
	for (size_t index = 0; index < iso->entry_count; ++index)
		SDL_free(iso->entries[index].path);
	SDL_free(iso->entries);
	SDL_free(iso->visited_directories);
	SDL_free(iso);
}

static const AeronIsoEntry* iso_find_entry(const AeronIso9660* iso, const char* path, int case_insensitive) {
	char normalized[AERON_MAX_PATH];
	if (!iso || !iso_normalize_path(path, normalized, sizeof normalized))
		return NULL;
	size_t low  = 0;
	size_t high = iso->entry_count;
	while (low < high) {
		const size_t middle = low + (high - low) / 2u;
		if (SDL_strcasecmp(iso->entries[middle].path, normalized) < 0)
			low = middle + 1u;
		else
			high = middle;
	}
	for (size_t index = low;
		 index < iso->entry_count && SDL_strcasecmp(iso->entries[index].path, normalized) == 0; ++index) {
		if (case_insensitive || strcmp(iso->entries[index].path, normalized) == 0)
			return &iso->entries[index];
	}
	return NULL;
}

static Sint64 SDLCALL iso_file_size(void* userdata) {
	const AeronIsoFile* file = (const AeronIsoFile*)userdata;
	return file && file->size <= INT64_MAX ? (Sint64)file->size : -1;
}

static Sint64 SDLCALL iso_file_seek(void* userdata, Sint64 offset, SDL_IOWhence whence) {
	AeronIsoFile* file = (AeronIsoFile*)userdata;
	if (!file)
		return -1;
	Sint64 base;
	switch (whence) {
		case SDL_IO_SEEK_SET:
			base = 0;
			break;
		case SDL_IO_SEEK_CUR:
			base = (Sint64)file->position;
			break;
		case SDL_IO_SEEK_END:
			base = (Sint64)file->size;
			break;
		default:
			return -1;
	}
	if (offset > 0) {
		if (base > INT64_MAX - offset)
			return -1;
	} else if (offset < 0) {
		if (offset == INT64_MIN || base < -offset)
			return -1;
	}
	const Sint64 position = base + offset;
	if (position < 0 || (uint64_t)position > file->size)
		return -1;
	file->position = (uint64_t)position;
	return position;
}

static int iso_file_read_raw(AeronIsoFile* file, uint64_t offset, void* destination, size_t size) {
	uint8_t* output = (uint8_t*)destination;
	/* Small recovered-code reads commonly stay within the same archive area.
	 * Cache a short physical-sector window per file handle. */
	while (size != 0) {
		const uint64_t sector = offset / ISO_LOGICAL_SECTOR_SIZE;
		const size_t   inside = (size_t)(offset % ISO_LOGICAL_SECTOR_SIZE);
		if (file->cached_sector_count == 0 || sector < file->cached_first_sector ||
			sector - file->cached_first_sector >= file->cached_sector_count) {
			const uint64_t total_sectors =
				(file->source.image_size - file->source.track_offset) / ISO_RAW_SECTOR_SIZE;
			if (sector >= total_sectors)
				return 0;
			size_t sectors = (size_t)(total_sectors - sector);
			if (sectors > ISO_READ_BATCH_SECTORS)
				sectors = ISO_READ_BATCH_SECTORS;
			const uint64_t physical      = file->source.track_offset + sector * ISO_RAW_SECTOR_SIZE;
			const size_t   physical_size = sectors * ISO_RAW_SECTOR_SIZE;
			if (!iso_seek_and_read(file->image, physical, file->scratch, physical_size))
				return 0;
			file->cached_first_sector = sector;
			file->cached_sector_count = sectors;
		}
		const size_t cached_index = (size_t)(sector - file->cached_first_sector);
		const size_t chunk =
			size < ISO_LOGICAL_SECTOR_SIZE - inside ? size : ISO_LOGICAL_SECTOR_SIZE - inside;
		memcpy(output, file->scratch + cached_index * ISO_RAW_SECTOR_SIZE + ISO_RAW_PAYLOAD_OFFSET + inside,
			   chunk);
		offset += chunk;
		output += chunk;
		size -= chunk;
	}
	return 1;
}

static size_t SDLCALL iso_file_read(void* userdata, void* destination, size_t size, SDL_IOStatus* status) {
	AeronIsoFile* file = (AeronIsoFile*)userdata;
	if (!file || !destination) {
		if (status)
			*status = SDL_IO_STATUS_ERROR;
		return 0;
	}
	const uint64_t available = file->size - file->position;
	const size_t   count     = available < size ? (size_t)available : size;
	const uint64_t offset    = file->logical_offset + file->position;
	const int read_ok = file->source.physical_sector_size == ISO_RAW_SECTOR_SIZE
							? iso_file_read_raw(file, offset, destination, count)
							: iso_read_logical(&file->source, file->image, offset, destination, count, NULL);
	if (count != 0 && !read_ok) {
		if (status)
			*status = SDL_IO_STATUS_ERROR;
		return 0;
	}
	file->position += count;
	if (count < size && status)
		*status = SDL_IO_STATUS_EOF;
	return count;
}

static bool SDLCALL iso_file_close(void* userdata) {
	AeronIsoFile* file = (AeronIsoFile*)userdata;
	if (!file)
		return true;
	const bool result = !file->image || SDL_CloseIO(file->image);
	SDL_free(file->scratch);
	SDL_free(file);
	return result;
}

SDL_IOStream* AeronIso9660_OpenFile(const AeronIso9660* iso, const char* path, int case_insensitive) {
	const AeronIsoEntry* entry = iso_find_entry(iso, path, case_insensitive);
	if (!entry || entry->is_directory)
		return NULL;
	AeronIsoFile* file = (AeronIsoFile*)SDL_calloc(1, sizeof *file);
	if (!file)
		return NULL;
	file->source         = iso->source;
	file->logical_offset = (uint64_t)entry->extent * ISO_LOGICAL_SECTOR_SIZE;
	file->size           = entry->size;
	file->image          = SDL_IOFromFile(file->source.image_path, "rb");
	if (file->source.physical_sector_size == ISO_RAW_SECTOR_SIZE)
		file->scratch = (uint8_t*)SDL_malloc(ISO_READ_BATCH_SECTORS * ISO_RAW_SECTOR_SIZE);
	if (!file->image || (file->source.physical_sector_size == ISO_RAW_SECTOR_SIZE && !file->scratch)) {
		iso_file_close(file);
		return NULL;
	}
	SDL_IOStreamInterface interface;
	SDL_INIT_INTERFACE(&interface);
	interface.size       = iso_file_size;
	interface.seek       = iso_file_seek;
	interface.read       = iso_file_read;
	interface.close      = iso_file_close;
	SDL_IOStream* stream = SDL_OpenIO(&interface, file);
	if (!stream)
		iso_file_close(file);
	return stream;
}

int AeronIso9660_Stat(const AeronIso9660* iso, const char* path, int case_insensitive,
					  AeronFileInfo* out_info) {
	if (!out_info)
		return 0;
	memset(out_info, 0, sizeof *out_info);
	const AeronIsoEntry* entry = iso_find_entry(iso, path, case_insensitive);
	if (!entry)
		return 0;
	out_info->exists       = 1;
	out_info->is_directory = entry->is_directory;
	out_info->size         = entry->size;
	return 1;
}

static int iso_wildcard_match(const char* pattern, const char* value, int case_insensitive) {
	while (*pattern) {
		if (*pattern == '*') {
			while (*pattern == '*')
				++pattern;
			if (!*pattern)
				return 1;
			for (; *value; ++value) {
				if (iso_wildcard_match(pattern, value, case_insensitive))
					return 1;
			}
			return 0;
		}
		if (!*value)
			return 0;
		const unsigned char p = (unsigned char)*pattern;
		const unsigned char v = (unsigned char)*value;
		if (*pattern != '?' && (case_insensitive ? tolower(p) != tolower(v) : p != v))
			return 0;
		++pattern;
		++value;
	}
	return *value == '\0';
}

int AeronIso9660_Glob(const AeronIso9660* iso, const char* directory, const char* pattern, uint32_t flags,
					  int root_case_insensitive, AeronVfsGlobCallback callback, void* userdata) {
	if (!iso || !callback)
		return 0;
	if (!directory || !directory[0])
		directory = ".";
	if (!pattern || !pattern[0])
		pattern = "*";
	const AeronIsoEntry* parent = iso_find_entry(iso, directory, root_case_insensitive);
	if (!parent || !parent->is_directory)
		return 0;
	const size_t prefix_length          = parent->path[0] ? strlen(parent->path) + 1u : 0;
	const int    match_case_insensitive = (flags & AERON_VFS_GLOB_CASE_INSENSITIVE) != 0;
	int          want_files             = (flags & AERON_VFS_GLOB_FILES) != 0;
	int          want_directories       = (flags & AERON_VFS_GLOB_DIRECTORIES) != 0;
	if (!want_files && !want_directories)
		want_files = want_directories = 1;

	for (size_t index = 0; index < iso->entry_count; ++index) {
		const AeronIsoEntry* entry = &iso->entries[index];
		if (entry == parent || strlen(entry->path) <= prefix_length)
			continue;
		if (prefix_length != 0 && (SDL_strncasecmp(entry->path, parent->path, prefix_length - 1u) != 0 ||
								   entry->path[prefix_length - 1u] != '/'))
			continue;
		const char* name = entry->path + prefix_length;
		if (strchr(name, '/') || !iso_wildcard_match(pattern, name, match_case_insensitive) ||
			(entry->is_directory ? !want_directories : !want_files))
			continue;
		const AeronVfsEntry public_entry = {
			.name         = name,
			.size         = entry->size,
			.is_directory = entry->is_directory,
		};
		if (!callback(userdata, &public_entry))
			return 0;
	}
	return 1;
}
