#include "xwau_config.h"

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "util.h"

/* ---- Paths ---------------------------------------------------------------- */

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
			snprintf(out, cap, "%s", eq + 1);
			Xwau_Trim(out, rules);
			return;
		}
	}
}

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
			Xwau_Trim(at, rules);
			if (at[0] || rules == XWAU_RULES_CS)
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
