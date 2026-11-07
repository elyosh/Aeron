#include "xwau_config.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "util.h"

/* ---- Paths ---------------------------------------------------------------- */

/* The components already found: "<folder>/<name in lower case>" gives the
 * path with the name's case on disk. A conversion looks up many files in the
 * same folders, and reading a folder such as FlightModels for each lookup
 * takes most of the time. A path is used again only while it still exists,
 * and names that weren't found aren't kept, so files that come or go are
 * still found. Threads share the cache, so `g_found_mutex` guards it. */
typedef struct FoundPath {
	char* key;
	char* path;
} FoundPath;

static FoundPath*      g_found;
static size_t          g_found_cap, g_found_count;
static pthread_mutex_t g_found_mutex = PTHREAD_MUTEX_INITIALIZER;

static size_t found_hash(const char* key) {
	size_t hash = 14695981039346656037u;
	for (; *key; ++key)
		hash = (hash ^ (unsigned char)*key) * 1099511628211u;
	return hash;
}

/* Copies the path found for `key` into `out`. Returns false when there is
 * none, or when it doesn't fit. */
static bool found_get(const char* key, char* out, size_t cap) {
	bool known = false;
	pthread_mutex_lock(&g_found_mutex);
	if (g_found_cap) {
		size_t i = found_hash(key) & (g_found_cap - 1);
		while (g_found[i].key && strcmp(g_found[i].key, key) != 0)
			i = (i + 1) & (g_found_cap - 1);
		if (g_found[i].key && strlen(g_found[i].path) < cap) {
			strcpy(out, g_found[i].path);
			known = true;
		}
	}
	pthread_mutex_unlock(&g_found_mutex);
	return known;
}

static void found_put_locked(const char* key, const char* path) {
	if (g_found_count * 2 >= g_found_cap) {
		const size_t cap   = g_found_cap ? g_found_cap * 2 : 1024;
		FoundPath*   table = (FoundPath*)calloc(cap, sizeof *table);
		if (!table)
			return;
		for (size_t i = 0; i < g_found_cap; ++i) {
			if (!g_found[i].key)
				continue;
			size_t j = found_hash(g_found[i].key) & (cap - 1);
			while (table[j].key)
				j = (j + 1) & (cap - 1);
			table[j] = g_found[i];
		}
		free(g_found);
		g_found     = table;
		g_found_cap = cap;
	}
	size_t i = found_hash(key) & (g_found_cap - 1);
	while (g_found[i].key && strcmp(g_found[i].key, key) != 0)
		i = (i + 1) & (g_found_cap - 1);
	char* new_path = strdup(path);
	if (!new_path)
		return;
	if (g_found[i].key) {
		free(g_found[i].path);
		g_found[i].path = new_path;
		return;
	}
	g_found[i].key = strdup(key);
	if (!g_found[i].key) {
		free(new_path);
		return;
	}
	g_found[i].path = new_path;
	g_found_count++;
}

static void found_put(const char* key, const char* path) {
	pthread_mutex_lock(&g_found_mutex);
	found_put_locked(key, path);
	pthread_mutex_unlock(&g_found_mutex);
}

/* A folder read while the folder cache is on. Its names are sorted without
 * case. Names that differ only in case keep their order in the folder, so a
 * lookup finds the same name as a scan of the folder. A folder that can't be
 * opened has no names. */
typedef struct CachedFolder {
	char*  path;
	char** names;
	size_t count;
	bool   opened;
} CachedFolder;

typedef struct FolderEntry {
	char*  name;
	size_t order;
} FolderEntry;

static bool            g_folder_cache;
static CachedFolder*   g_folders;
static size_t          g_folder_cap, g_folder_count;
static pthread_mutex_t g_folder_mutex = PTHREAD_MUTEX_INITIALIZER;

/* A file that Xwau_FileHasText read while the folder cache is on, such as a
 * hook DLL. The import looks for many texts in the same DLLs. */
typedef struct CachedFile {
	char*  path;
	char*  data;
	size_t size;
} CachedFile;

static CachedFile* g_files;
static size_t      g_file_count;

static int compare_folder_entries(const void* a, const void* b) {
	const FolderEntry* ea    = (const FolderEntry*)a;
	const FolderEntry* eb    = (const FolderEntry*)b;
	const int          order = strcasecmp(ea->name, eb->name);
	return order ? order : (ea->order > eb->order) - (ea->order < eb->order);
}

/* Reads the names of `folder->path`. */
static void read_folder(CachedFolder* folder) {
	DIR* d = opendir(folder->path);
	if (!d)
		return;
	folder->opened      = true;
	FolderEntry*   list = NULL;
	size_t         cap = 0, count = 0;
	struct dirent* de;
	while ((de = readdir(d))) {
		if (count == cap) {
			FolderEntry* grown = (FolderEntry*)realloc(list, (cap ? cap * 2 : 64) * sizeof *grown);
			if (!grown)
				break;
			list = grown;
			cap  = cap ? cap * 2 : 64;
		}
		if (!(list[count].name = strdup(de->d_name)))
			break;
		list[count].order = count;
		++count;
	}
	closedir(d);
	qsort(list, count, sizeof *list, compare_folder_entries);
	folder->names = (char**)malloc((count ? count : 1) * sizeof *folder->names);
	for (size_t i = 0; i < count; ++i) {
		if (folder->names)
			folder->names[i] = list[i].name;
		else
			free(list[i].name);
	}
	folder->count = folder->names ? count : 0;
	free(list);
}

/* Returns the cached folder `path`, and reads the folder the first time.
 * Returns NULL when out of memory. The caller must hold `g_folder_mutex`. */
static CachedFolder* cached_folder_locked(const char* path) {
	if (g_folder_count * 2 >= g_folder_cap) {
		const size_t  cap   = g_folder_cap ? g_folder_cap * 2 : 256;
		CachedFolder* table = (CachedFolder*)calloc(cap, sizeof *table);
		if (!table)
			return NULL;
		for (size_t i = 0; i < g_folder_cap; ++i) {
			if (!g_folders[i].path)
				continue;
			size_t j = found_hash(g_folders[i].path) & (cap - 1);
			while (table[j].path)
				j = (j + 1) & (cap - 1);
			table[j] = g_folders[i];
		}
		free(g_folders);
		g_folders    = table;
		g_folder_cap = cap;
	}
	size_t i = found_hash(path) & (g_folder_cap - 1);
	while (g_folders[i].path && strcmp(g_folders[i].path, path) != 0)
		i = (i + 1) & (g_folder_cap - 1);
	if (!g_folders[i].path) {
		if (!(g_folders[i].path = strdup(path)))
			return NULL;
		read_folder(&g_folders[i]);
		g_folder_count++;
	}
	return &g_folders[i];
}

/* Looks for `name` in the folder `path`, in any case, and copies the name as it
 * is written in the folder into `out`. Returns 1 if it is found, 0 if it isn't,
 * and -1 if the cache can't be used. */
static int find_in_cached_folder(const char* path, const char* name, char* out, size_t cap) {
	pthread_mutex_lock(&g_folder_mutex);
	const CachedFolder* folder = cached_folder_locked(path);
	int                 found  = folder ? 0 : -1;
	if (folder && folder->opened) {
		size_t low = 0, high = folder->count;
		while (low < high) {
			const size_t middle = low + (high - low) / 2;
			if (strcasecmp(folder->names[middle], name) < 0)
				low = middle + 1;
			else
				high = middle;
		}
		if (low < folder->count && strcasecmp(folder->names[low], name) == 0)
			found = PathFormat(out, cap, "%s", folder->names[low]) ? 1 : 0;
	}
	pthread_mutex_unlock(&g_folder_mutex);
	return found;
}

void Xwau_SetFolderCache(bool on) {
	pthread_mutex_lock(&g_folder_mutex);
	g_folder_cache = on;
	if (!on) {
		for (size_t i = 0; i < g_folder_cap; ++i) {
			for (size_t k = 0; k < g_folders[i].count; ++k)
				free(g_folders[i].names[k]);
			free(g_folders[i].names);
			free(g_folders[i].path);
		}
		free(g_folders);
		g_folders    = NULL;
		g_folder_cap = g_folder_count = 0;
		for (size_t i = 0; i < g_file_count; ++i) {
			free(g_files[i].path);
			free(g_files[i].data);
		}
		free(g_files);
		g_files      = NULL;
		g_file_count = 0;
	}
	pthread_mutex_unlock(&g_folder_mutex);
}

/* Writes `base`/`rel` into `out`, matching each component of `rel` in any
 * case. Returns false when a component is missing. */
static bool resolve(const char* base, const char* rel, char* out, size_t cap) {
	char parts[1024];
	if (!PathFormat(out, cap, "%s", base) || !PathFormat(parts, sizeof parts, "%s", rel))
		return false;
	for (char* c = parts; *c; ++c)
		if (*c == '\\')
			*c = '/';
	for (char *part = parts, *next; part; part = next) {
		next = strchr(part, '/');
		if (next)
			*next++ = 0;
		if (!*part)
			continue;
		if (g_folder_cache) {
			char      name[1024];
			const int found = find_in_cached_folder(out, part, name, sizeof name);
			if (found == 0)
				return false;
			if (found == 1) {
				const size_t len = strlen(out);
				if (!PathFormat(out + len, cap - len, "/%s", name))
					return false;
				continue;
			}
		}
		char key[2048];
		if (!PathFormat(key, sizeof key, "%s/%s", out, part))
			return false;
		LowerInPlace(key + strlen(out) + 1);
		char        known[1024];
		struct stat st;
		if (found_get(key, known, sizeof known) && stat(known, &st) == 0) {
			if (!PathFormat(out, cap, "%s", known))
				return false;
			continue;
		}
		DIR* d = opendir(out);
		if (!d)
			return false;
		bool           found = false, fits = true;
		struct dirent* de;
		while (!found && (de = readdir(d))) {
			if (strcasecmp(de->d_name, part) == 0) {
				const size_t len = strlen(out);
				fits             = PathFormat(out + len, cap - len, "/%s", de->d_name);
				found            = true;
			}
		}
		closedir(d);
		if (!found || !fits)
			return false;
		found_put(key, out);
	}
	return true;
}

bool Xwau_FindFile(const char* game, const char* rel, char* out, size_t cap) {
	char path[1024];
	return resolve(game, rel, path, sizeof path) && IsFile(path) && PathFormat(out, cap, "%s", path);
}

bool Xwau_FindDir(const char* game, const char* rel, char* out, size_t cap) {
	return resolve(game, rel, out, cap) && IsDir(out);
}

/* Whether the `size` bytes of `data` hold `text`. */
static bool has_text(const char* data, size_t size, const char* text) {
	const size_t length = strlen(text);
	for (size_t i = 0; data && length && i + length <= size; ++i)
		if (memcmp(data + i, text, length) == 0)
			return true;
	return false;
}

bool Xwau_FileHasText(const char* path, const char* text) {
	pthread_mutex_lock(&g_folder_mutex);
	if (g_folder_cache) {
		for (size_t i = 0; i < g_file_count; ++i) {
			if (strcmp(g_files[i].path, path) == 0) {
				const bool found = has_text(g_files[i].data, g_files[i].size, text);
				pthread_mutex_unlock(&g_folder_mutex);
				return found;
			}
		}
	}
	size_t     size  = 0;
	char*      data  = ReadWholeFile(path, &size);
	const bool found = has_text(data, size, text);
	char*      copy  = g_folder_cache ? StrDup(path) : NULL;
	if (copy && ArrayGrow((void**)&g_files, g_file_count, sizeof *g_files)) {
		g_files[g_file_count++] = (CachedFile) { copy, data, size };
		data                    = NULL;
	} else {
		free(copy);
	}
	pthread_mutex_unlock(&g_folder_mutex);
	free(data);
	return found;
}

bool Xwau_DllHasText(const char* game, const char* dll, const char* text) {
	char path[1024];
	return Xwau_FindFile(game, dll, path, sizeof path) && Xwau_FileHasText(path, text);
}

/* ---- Text ------------------------------------------------------------------ */

char Xwau_Lower(char c) {
	const unsigned char u = (unsigned char)c;
	if ((u >= 'A' && u <= 'Z') || (u >= 0xC0 && u <= 0xDE && u != 0xD7))
		return (char)(u + 0x20);
	return c;
}

bool Xwau_SameName(const char* a, const char* b) {
	for (; *a && *b; ++a, ++b)
		if (Xwau_Lower(*a) != Xwau_Lower(*b))
			return false;
	return !*a && !*b;
}

static bool is_space(char c, XwauRules rules) {
	const unsigned char u = (unsigned char)c;
	if (u == ' ' || (u >= '\t' && u <= '\r'))
		return true;
	return rules == XWAU_RULES_CS && (u == 0x85 || u == 0xA0);
}

char* Xwau_Trim(char* s, XwauRules rules) {
	char* start = s;
	while (*start && is_space(*start, rules))
		++start;
	size_t len = strlen(start);
	while (len && is_space(start[len - 1], rules))
		--len;
	memmove(s, start, len);
	s[len] = 0;
	return s;
}

bool XwauLines_Add(XwauLines* lines, const char* text, size_t len) {
	char*  copy = StrNDup(text, len);
	char** slot = copy ? ARRAY_PUSH(lines->items, lines->count) : NULL;
	if (!slot) {
		free(copy);
		return false;
	}
	*slot = copy;
	return true;
}

bool XwauLines_Read(const char* path, const char* section, XwauRules rules, XwauLines* out) {
	memset(out, 0, sizeof *out);
	size_t size = 0;
	char*  text = path ? ReadWholeFile(path, &size) : NULL;
	if (!text)
		return true;
	bool reading = !section[0];
	bool ok      = true;
	for (size_t at = 0; ok && at <= size;) {
		/* The line ends at '\n' (C++), or "\r\n", '\r' or '\n' (C#). */
		size_t end = at;
		while (end < size && text[end] != '\n' && (rules == XWAU_RULES_CPP || text[end] != '\r'))
			++end;
		size_t next = end + 1;
		if (rules == XWAU_RULES_CS && end < size && text[end] == '\r' && end + 1 < size &&
			text[end + 1] == '\n')
			next = end + 2;
		/* Copy and trim the line. A NUL byte in the text ends the line. */
		char* line = (char*)malloc(end - at + 1);
		if (!line) {
			ok = false;
			break;
		}
		memcpy(line, text + at, end - at);
		line[end - at] = 0;
		Xwau_Trim(line, rules);
		const size_t len = strlen(line);
		if (len && line[0] != '#' && line[0] != ';' && strncmp(line, "//", 2) != 0) {
			if (line[0] == '[' && line[len - 1] == ']') {
				line[len - 1] = 0;
				reading       = Xwau_SameName(line + 1, section);
			} else if (reading) {
				ok = XwauLines_Add(out, line, len);
			}
		}
		free(line);
		at = next;
	}
	free(text);
	if (!ok)
		XwauLines_Free(out);
	return ok;
}

void XwauLines_Free(XwauLines* lines) {
	for (size_t i = 0; lines->items && i < lines->count; ++i)
		free(lines->items[i]);
	free(lines->items);
	memset(lines, 0, sizeof *lines);
}

void XwauLines_Value(const XwauLines* lines, const char* key, XwauRules rules, char* out, size_t cap) {
	out[0] = 0;
	for (size_t i = 0; i < lines->count; ++i) {
		const char* eq = strchr(lines->items[i], '=');
		if (!eq)
			continue;
		char name[512];
		snprintf(name, sizeof name, "%.*s", (int)(eq - lines->items[i]), lines->items[i]);
		Xwau_Trim(name, rules);
		if (name[0] && Xwau_SameName(name, key)) {
			/* The value without the white space around it must fit. */
			const char* start = eq + 1;
			while (*start && is_space(*start, rules))
				++start;
			size_t len = strlen(start);
			while (len && is_space(start[len - 1], rules))
				--len;
			if (len >= cap)
				ReportFatal(NULL, "the value of %s is %zu characters long, but the import reads only %zu",
							name, len, cap - 1);
			snprintf(out, cap, "%.*s", (int)len, start);
			return;
		}
	}
}

bool Xwau_LineKey(const char* line, XwauRules rules, char* key, size_t cap) {
	const char* eq = strchr(line, '=');
	if (!eq)
		return false;
	snprintf(key, cap, "%.*s", (int)(eq - line), line);
	Xwau_Trim(key, rules);
	return key[0] != 0;
}

bool Xwau_SplitLine(const char* line, XwauRules rules, char* key, size_t key_cap, char* value,
					size_t value_cap) {
	if (!Xwau_LineKey(line, rules, key, key_cap))
		return false;
	snprintf(value, value_cap, "%s", strchr(line, '=') + 1);
	Xwau_Trim(value, rules);
	return true;
}

bool XwauSection_ReadPaths(const char* txt, const char* ini, const char* section, XwauRules rules,
						   XwauSection* out) {
	memset(out, 0, sizeof *out);
	out->rules = rules;
	if (!XwauLines_Read(txt, "", rules, &out->lines))
		return false;
	if (out->lines.count) {
		snprintf(out->source, sizeof out->source, "%s", txt);
		return true;
	}
	if (!XwauLines_Read(ini, section, rules, &out->lines))
		return false;
	if (out->lines.count)
		snprintf(out->source, sizeof out->source, "%s [%s]", ini, section);
	return true;
}

bool XwauSection_Read(const char* game, const char* txt, const char* ini, const char* section,
					  XwauRules rules, XwauSection* out) {
	char txt_path[1024], ini_path[1024];
	return XwauSection_ReadPaths(txt && Xwau_FindFile(game, txt, txt_path, sizeof txt_path) ? txt_path : NULL,
								 ini && Xwau_FindFile(game, ini, ini_path, sizeof ini_path) ? ini_path : NULL,
								 section, rules, out);
}

bool XwauSection_ReadModel(const char* game, const char* model, const char* section, XwauRules rules,
						   XwauSection* out) {
	char txt[600], ini[600];
	snprintf(txt, sizeof txt, "FlightModels/%s%s.txt", model, section);
	snprintf(ini, sizeof ini, "FlightModels/%s.ini", model);
	return XwauSection_Read(game, txt, ini, section, rules, out);
}

bool XwauSection_ReadGlobal(const char* game, const char* section, XwauRules rules, XwauSection* out) {
	char txt[300];
	snprintf(txt, sizeof txt, "FlightModels/%s.txt", section);
	return XwauSection_Read(game, txt, "FlightModels/default.ini", section, rules, out);
}

bool XwauSection_ReadMission(const char* game, const char* base, const char* section, XwauRules rules,
							 XwauSection* out) {
	char txt[1200], ini[1200];
	snprintf(txt, sizeof txt, "%s_%s.txt", base, section);
	snprintf(ini, sizeof ini, "%s.ini", base);
	return XwauSection_Read(game, txt, ini, section, rules, out);
}

bool XwauSection_ReadHook(const char* game, const char* hook, XwauSection* out) {
	char cfg[300];
	snprintf(cfg, sizeof cfg, "%s.cfg", hook);
	return XwauSection_Read(game, cfg, "Hooks.ini", hook, XWAU_RULES_CPP, out);
}

void XwauSection_Free(XwauSection* section) {
	XwauLines_Free(&section->lines);
	section->source[0] = 0;
}

void XwauSection_Text(const XwauSection* section, const char* key, char* out, size_t cap) {
	XwauLines_Value(&section->lines, key, section->rules, out, cap);
}

/* Reports a value that isn't a number. */
static void report_bad_value(const XwauSection* section, const char* key, const char* text) {
	Report(section->source[0] ? section->source : NULL,
		   "%s = %s isn't a number, which would crash XWAU. It is left out", key, text);
}

XwauValue XwauLines_Int(const XwauLines* lines, XwauRules rules, const char* key, long long* out) {
	char      text[256];
	long long value;
	XwauLines_Value(lines, key, rules, text, sizeof text);
	if (!text[0])
		return XWAU_VALUE_MISSING;
	if (!Xwau_StoiAutoInt(text, &value))
		return XWAU_VALUE_BAD;
	*out = value;
	return XWAU_VALUE_SET;
}

XwauValue XwauSection_Int(const XwauSection* section, const char* key, long long* out) {
	const XwauValue result = XwauLines_Int(&section->lines, section->rules, key, out);
	if (result == XWAU_VALUE_BAD) {
		char text[256];
		XwauSection_Text(section, key, text, sizeof text);
		report_bad_value(section, key, text);
	}
	return result;
}

long long XwauSection_IntOr(const XwauSection* section, const char* key, long long fallback) {
	long long value = fallback;
	XwauSection_Int(section, key, &value);
	return value;
}

XwauValue XwauSection_Float(const XwauSection* section, const char* key, float* out) {
	char  text[256];
	float value;
	XwauSection_Text(section, key, text, sizeof text);
	if (!text[0])
		return XWAU_VALUE_MISSING;
	if (!Xwau_Stof(text, &value)) {
		report_bad_value(section, key, text);
		return XWAU_VALUE_BAD;
	}
	*out = value;
	return XWAU_VALUE_SET;
}

float XwauSection_FloatOr(const XwauSection* section, const char* key, float fallback) {
	float value = fallback;
	XwauSection_Float(section, key, &value);
	return value;
}

bool XwauSection_KeyAt(const XwauSection* section, size_t index, char* key, size_t cap) {
	if (!Xwau_LineKey(section->lines.items[index], section->rules, key, cap))
		return false;
	for (size_t i = 0; i < index; ++i) {
		char other[512];
		if (Xwau_LineKey(section->lines.items[i], section->rules, other, sizeof other) &&
			Xwau_SameName(key, other))
			return false;
	}
	return true;
}

void XwauSection_ReportUnknownKeys(const XwauSection* section, const char* const* keys,
								   const char* const* prefixes) {
	for (size_t i = 0; i < section->lines.count; ++i) {
		char key[512];
		bool known = false;
		if (!XwauSection_KeyAt(section, i, key, sizeof key))
			continue;
		for (size_t k = 0; keys && keys[k] && !known; ++k)
			known = Xwau_SameName(key, keys[k]);
		for (size_t k = 0; prefixes && prefixes[k] && !known; ++k) {
			const size_t length = strlen(prefixes[k]);
			known               = strncasecmp(key, prefixes[k], length) == 0 && key[length];
		}
		if (!known)
			Report(section->source, "%s isn't a key that the hook reads, so it is ignored", key);
	}
}

bool Xwau_ModelKey(const char* path, char* out, size_t cap) {
	const char* base = path;
	for (const char* c = path; *c; ++c)
		if (*c == '\\' || *c == '/')
			base = c + 1;
	const char*  dot = strrchr(base, '.');
	const size_t len = dot ? (size_t)(dot - base) : strlen(base);
	if (!len || len >= cap)
		return false;
	for (size_t i = 0; i < len; ++i)
		out[i] = Xwau_Lower(base[i]);
	out[len] = 0;
	return true;
}

void Xwau_WriteString(FILE* f, const char* text) {
	fputc('"', f);
	for (const unsigned char* c = (const unsigned char*)text; *c; ++c) {
		if (*c == '"' || *c == '\\') {
			fputc('\\', f);
			fputc(*c, f);
		} else if (*c < 0x80) {
			fputc(*c, f);
		} else {
			fputc(0xC0 | (*c >> 6), f);
			fputc(0x80 | (*c & 0x3F), f);
		}
	}
	fputc('"', f);
}

void Xwau_WriteDecimal(FILE* f, double value) {
	char text[64];
	snprintf(text, sizeof text, "%.3f", value);
	size_t length = strlen(text);
	while (length > 0 && text[length - 1] == '0')
		text[--length] = 0;
	if (length > 0 && text[length - 1] == '.')
		text[--length] = 0;
	fputs(strcmp(text, "-0") == 0 ? "0" : text, f);
}

double Xwau_FromGame(XwauUnit unit, double game) {
	static const double kGamePerUnit[] = { 1.0, 40.96, 0.25, 2.25, 65536.0 / 360.0, 65536.0 / 100.0 };
	return game / kGamePerUnit[unit];
}

void Xwau_WriteUnit(FILE* f, XwauUnit unit, long long game) {
	if (unit == XWAU_UNIT_NONE)
		fprintf(f, "%lld", game);
	else
		Xwau_WriteDecimal(f, Xwau_FromGame(unit, (double)game));
}

double Xwau_Metres(double units) { return Xwau_FromGame(XWAU_UNIT_METRES, units); }

void Xwau_WriteMetres(FILE* f, int units) { Xwau_WriteDecimal(f, Xwau_Metres(units)); }

void Xwau_WriteMetres3(FILE* f, int x, int y, int z) {
	fputc('[', f);
	Xwau_WriteMetres(f, x);
	fputs(", ", f);
	Xwau_WriteMetres(f, y);
	fputs(", ", f);
	Xwau_WriteMetres(f, z);
	fputc(']', f);
}

void Xwau_WriteDegrees(FILE* f, int steps) { Xwau_WriteDecimal(f, steps * 360.0 / 65536.0); }

bool Xwau_Tokenize(const char* value, XwauRules rules, XwauLines* out) {
	memset(out, 0, sizeof *out);
	char* copy = StrNDup(value, strlen(value));
	if (!copy)
		return false;
	Xwau_Trim(copy, rules);
	bool ok = true;
	if (copy[0]) {
		for (char* at = copy;;) {
			char*      sep  = strpbrk(at, ",;");
			const bool last = !sep;
			if (sep)
				*sep = 0;
			/* The C++ hooks skip the empty text between two separators, but
			   keep a word of spaces, as an empty word. */
			const bool empty = !at[0];
			Xwau_Trim(at, rules);
			if (!empty || rules == XWAU_RULES_CS)
				ok = ok && XwauLines_Add(out, at, strlen(at));
			if (last)
				break;
			at = sep + 1;
		}
	}
	free(copy);
	if (!ok)
		XwauLines_Free(out);
	return ok;
}

/* ---- Numbers --------------------------------------------------------------- */

/* Skips the white space and the sign, and returns the sign. */
static int sign_of(const char** text) {
	while (is_space(**text, XWAU_RULES_CPP))
		++*text;
	int sign = 1;
	if (**text == '+' || **text == '-') {
		sign = **text == '-' ? -1 : 1;
		++*text;
	}
	return sign;
}

static int digit_value(char c) {
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return 99;
}

/* Reads the digits in `base` at `text`, up to the first other character.
 * Returns false when there are none. The value stops growing far outside
 * the int range. */
static bool digits(const char* text, int base, long long* out) {
	long long value = 0;
	int       count = 0;
	for (; digit_value(*text) < base; ++text, ++count)
		if (value < (1LL << 40))
			value = value * base + digit_value(*text);
	*out = value;
	return count > 0;
}

bool Xwau_Stoi(const char* text, long long* out) {
	const int sign = sign_of(&text);
	long long value;
	if (!digits(text, 10, &value))
		return false;
	value *= sign;
	if (value < -2147483648LL || value > 2147483647LL)
		return false;
	*out = value;
	return true;
}

bool Xwau_StoiAuto(const char* text, long long* out) {
	const int sign = sign_of(&text);
	int       base = 10;
	if (text[0] == '0' && (text[1] == 'x' || text[1] == 'X') && digit_value(text[2]) < 16) {
		base = 16;
		text += 2;
	} else if (text[0] == '0') {
		base = 8;
	}
	long long value;
	if (!digits(text, base, &value))
		return false;
	*out = sign * value;
	return true;
}

bool Xwau_StoiAutoInt(const char* text, long long* out) {
	long long value;
	if (!Xwau_StoiAuto(text, &value) || value < -2147483648LL || value > 2147483647LL)
		return false;
	*out = value;
	return true;
}

bool Xwau_Stof(const char* text, float* out) {
	char* end = NULL;
	errno     = 0;
	*out      = strtof(text, &end);
	return end != text && errno != ERANGE;
}

bool Xwau_StoulHex(const char* text, uint32_t* out) {
	const int sign = sign_of(&text);
	if (text[0] == '0' && (text[1] == 'x' || text[1] == 'X') && digit_value(text[2]) < 16)
		text += 2;
	long long value;
	if (!digits(text, 16, &value) || value > 0xffffffffLL)
		return false;
	*out = (uint32_t)(sign * value);
	return true;
}

bool Xwau_DatNames(const char* written, char* rel, size_t rel_cap, char* library, size_t lib_cap) {
	if (!PathFormat(rel, rel_cap, "%s", written))
		return false;
	for (char* p = rel; *p; ++p)
		*p = *p == '\\' ? '/' : (char)tolower((unsigned char)*p);
	const char*  base = strrchr(rel, '/');
	const char*  name = base ? base + 1 : rel;
	const char*  dot  = strrchr(name, '.');
	const size_t len  = dot ? (size_t)(dot - name) : strlen(name);
	return PathFormat(library, lib_cap, "%.*s", (int)len, name);
}

bool Xwau_EncodeName(const char* name, char* out, size_t cap) {
	size_t n = 0;
	for (const unsigned char* c = (const unsigned char*)name; *c; ++c) {
		const unsigned char lower = *c >= 'A' && *c <= 'Z' ? (unsigned char)(*c + 32) : *c;
		if ((lower >= 'a' && lower <= 'z') || (lower >= '0' && lower <= '9') || lower == '_') {
			if (n + 1 >= cap)
				return false;
			out[n++] = (char)lower;
		} else {
			if (n + 3 >= cap)
				return false;
			n += (size_t)snprintf(out + n, cap - n, ".%02x", *c);
		}
	}
	out[n] = 0;
	return true;
}

bool Xwau_ReadSettingNumbers(const char* text, int count, double* out) {
	float v[3] = { 0 };
	if (count == 1) {
		out[0] = atof(text);
		return true;
	}
	if (sscanf(text, "%f, %f, %f", &v[0], &v[1], &v[2]) < 3)
		return false;
	for (int i = 0; i < 3; ++i)
		out[i] = v[i];
	return true;
}

bool Xwau_ReadSettings(const char* game, const char* name, XwauSettingFn fn, void* data) {
	char   path[1024];
	size_t size = 0;
	char*  text = Xwau_FindFile(game, name, path, sizeof path) ? ReadWholeFile(path, &size) : NULL;
	if (!text)
		return false;
	for (char* line = text; *line;) {
		char* end = strchr(line, '\n');
		if (end)
			*end = 0;
		/* Like sscanf("%s = %s"), which skips the white space before the key. */
		const char* start = line;
		while (isspace((unsigned char)*start))
			++start;
		char key[128];
		int  key_length = 0;
		while (start[key_length] && !isspace((unsigned char)start[key_length]) &&
			   key_length + 1 < (int)sizeof key)
			++key_length;
		memcpy(key, start, (size_t)key_length);
		key[key_length]   = 0;
		const char* value = start + key_length;
		while (isspace((unsigned char)*value))
			++value;
		if (line[0] != ';' && line[0] != '#' && *value == '=')
			fn(key, value + 1, data);
		if (!end)
			break;
		line = end + 1;
	}
	free(text);
	return true;
}
