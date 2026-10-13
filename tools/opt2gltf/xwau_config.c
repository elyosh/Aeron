#include "xwau_config.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#else
#include <unistd.h>
#endif

/* ---- Roots ---------------------------------------------------------------- */

/* `base`/`rel` with each component of `rel` matched case-insensitively, into
 * `out`; false when a component is missing. */
static bool resolve(const char* base, const char* rel, char* out, size_t cap) {
	char parts[1024];
	snprintf(out, cap, "%s", base);
	snprintf(parts, sizeof parts, "%s", rel);
	for (char* c = parts; *c; ++c)
		if (*c == '\\')
			*c = '/';
	for (char *part = parts, *next; part; part = next) {
		next = strchr(part, '/');
		if (next)
			*next++ = 0;
		if (!*part)
			continue;
		DIR* d = opendir(out);
		if (!d)
			return false;
		bool           found = false;
		struct dirent* de;
		while (!found && (de = readdir(d))) {
			if (strcasecmp(de->d_name, part) == 0) {
				const size_t len = strlen(out);
				snprintf(out + len, cap - len, "/%s", de->d_name);
				found = true;
			}
		}
		closedir(d);
		if (!found)
			return false;
	}
	return true;
}

bool XwauRoots_FindFile(const XwauRoots* roots, const char* rel, char* out, size_t cap) {
	for (int r = 0; r < roots->count; ++r) {
		char        path[1024];
		struct stat st;
		if (resolve(roots->dirs[r], rel, path, sizeof path) && stat(path, &st) == 0 && S_ISREG(st.st_mode)) {
			snprintf(out, cap, "%s", path);
			return true;
		}
	}
	return false;
}

bool XwauRoots_FindDir(const XwauRoots* roots, int root, const char* rel, char* out, size_t cap) {
	struct stat st;
	return root >= 0 && root < roots->count && resolve(roots->dirs[root], rel, out, cap) &&
		   stat(out, &st) == 0 && S_ISDIR(st.st_mode);
}

static int compare_names(const void* a, const void* b) { return strcmp(*(char* const*)a, *(char* const*)b); }

char** Xwau_ListDir(const char* dir, bool sorted, size_t* count) {
	*count = 0;
	DIR* d = opendir(dir);
	if (!d)
		return NULL;
	char**         names = NULL;
	struct dirent* de;
	while ((de = readdir(d))) {
		if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
			continue;
		char** grown = (char**)realloc(names, (*count + 1) * sizeof *grown);
		char*  name  = Xwau_Strndup(de->d_name, strlen(de->d_name));
		if (!grown || !name) {
			free(name);
			Xwau_FreeList(grown ? grown : names, *count);
			*count = 0;
			closedir(d);
			return NULL;
		}
		names             = grown;
		names[(*count)++] = name;
	}
	closedir(d);
	if (sorted && *count)
		qsort(names, *count, sizeof *names, compare_names);
	return names;
}

void Xwau_FreeList(char** names, size_t count) {
	for (size_t i = 0; names && i < count; ++i)
		free(names[i]);
	free(names);
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

static bool lines_add(XwauLines* lines, const char* text, size_t len) {
	char** grown = (char**)realloc(lines->items, (lines->count + 1) * sizeof *grown);
	if (!grown)
		return false;
	lines->items = grown;
	char* copy   = (char*)malloc(len + 1);
	if (!copy)
		return false;
	memcpy(copy, text, len);
	copy[len]                    = 0;
	lines->items[lines->count++] = copy;
	return true;
}

static char* read_file(const char* path, size_t* size) {
	FILE* f = fopen(path, "rb");
	if (!f)
		return NULL;
	char* data = NULL;
	long  n    = fseek(f, 0, SEEK_END) == 0 ? ftell(f) : -1;
	if (n >= 0 && fseek(f, 0, SEEK_SET) == 0 && (data = (char*)malloc((size_t)n + 1)) != NULL) {
		if (fread(data, 1, (size_t)n, f) != (size_t)n) {
			free(data);
			data = NULL;
		} else {
			data[n] = 0;
			*size   = (size_t)n;
		}
	}
	fclose(f);
	return data;
}

bool XwauLines_Read(const char* path, const char* section, XwauRules rules, XwauLines* out) {
	memset(out, 0, sizeof *out);
	size_t size = 0;
	char*  text = path ? read_file(path, &size) : NULL;
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
		/* The line, trimmed; text may hold NUL bytes, which end it. */
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
				ok = lines_add(out, line, len);
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
			snprintf(out, cap, "%s", eq + 1);
			Xwau_Trim(out, rules);
			return;
		}
	}
}

bool Xwau_Tokenize(const char* value, XwauRules rules, XwauLines* out) {
	memset(out, 0, sizeof *out);
	char* copy = Xwau_Strndup(value, strlen(value));
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
			Xwau_Trim(at, rules);
			if (at[0] || rules == XWAU_RULES_CS)
				ok = ok && lines_add(out, at, strlen(at));
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

/* Skips white space and the sign; returns the sign. */
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

/* The digits at `text` in `base`, until another character; false without
 * any. Saturates far outside the int range. */
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

char* Xwau_Strdup(const char* text) { return Xwau_Strndup(text, strlen(text)); }

char* Xwau_Strndup(const char* text, size_t len) {
	char* copy = (char*)malloc(len + 1);
	if (copy) {
		memcpy(copy, text, len);
		copy[len] = 0;
	}
	return copy;
}

bool Xwau_MakeDir(const char* path) {
	struct stat st;
	if (stat(path, &st) == 0)
		return S_ISDIR(st.st_mode);
	/* Its parent first. */
	char         parent[2048];
	const size_t len = strlen(path);
	snprintf(parent, sizeof parent, "%s", path);
	for (size_t i = len; i > 1; --i) {
		if (parent[i - 1] == '/' || parent[i - 1] == '\\') {
			parent[i - 1] = 0;
			if (parent[0] && !Xwau_MakeDir(parent))
				return false;
			break;
		}
	}
#ifdef _WIN32
	const int made = _mkdir(path);
#else
	const int made = mkdir(path, 0755);
#endif
	return made == 0 || (stat(path, &st) == 0 && S_ISDIR(st.st_mode));
}

bool Xwau_RemoveTree(const char* path) {
	struct stat st;
	if (stat(path, &st) != 0)
		return true;
	if (!S_ISDIR(st.st_mode))
		return remove(path) == 0;
	size_t count   = 0;
	char** entries = Xwau_ListDir(path, false, &count);
	bool   ok      = true;
	for (size_t i = 0; ok && i < count; ++i) {
		char child[2048];
		snprintf(child, sizeof child, "%s/%s", path, entries[i]);
		ok = Xwau_RemoveTree(child);
	}
	Xwau_FreeList(entries, count);
#ifdef _WIN32
	return ok && _rmdir(path) == 0;
#else
	return ok && rmdir(path) == 0;
#endif
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
