#ifndef AERON_XWAU_CONFIG_H
#define AERON_XWAU_CONFIG_H

/*
 * Reads XWA Upgrade (XWAU) data and the configuration files of the hooks.
 *
 * The data is read from an XWAU installation, which is the game folder that
 * XWAU is installed into. Paths are relative to the game folder and use
 * '\' or '/'. Each path component is matched in any case.
 *
 * The hooks read their .ini and .txt files with two readers. Their rules
 * differ, and both are reproduced here:
 *   - XWAU_RULES_CPP follows config.cpp of the C++ hooks. Lines end at '\n'
 *     and are trimmed of " \t\n\r\f\v". Tokens are split on ',' and ';' and
 *     trimmed, and empty tokens are dropped.
 *   - XWAU_RULES_CS follows XwaHooksConfig of the C# hooks, which reads the
 *     skins of hook_32bpp. Lines end at "\r\n", '\r' or '\n', and they are
 *     also trimmed of U+0085 and U+00A0. Empty tokens are kept.
 * Both readers skip empty lines and lines that start with '#', ';' or "//".
 * A section starts with a whole line "[name]", and the name can be written
 * in any case. A key lookup uses the first line whose trimmed name before
 * '=' matches the key in any case. Text is ISO-8859-1.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

/* Finds the file at `rel` in the installation `game`. Returns false when
 * there is none. */
bool Xwau_FindFile(const char* game, const char* rel, char* out, size_t cap);

/* Finds the folder at `rel` in the installation `game`. Returns false when
 * there is none. */
bool Xwau_FindDir(const char* game, const char* rel, char* out, size_t cap);

/* Returns whether the file at `path` holds the bytes of `text`. The
 * importer uses it to find the features of a hook DLL from its strings,
 * since the mods don't ship the same hook versions. */
bool Xwau_FileHasText(const char* path, const char* text);

typedef enum XwauRules { XWAU_RULES_CPP, XWAU_RULES_CS } XwauRules;

typedef struct XwauLines {
	char** items;
	size_t count;
} XwauLines;

/* Reads the lines of `section` in the file `path`. With an empty section,
 * it reads the lines before the first section. When `path` is NULL or
 * cannot be read, there are no lines. Returns false when out of memory. */
bool XwauLines_Read(const char* path, const char* section, XwauRules rules, XwauLines* out);
void XwauLines_Free(XwauLines* lines);

/* Appends a copy of the `len` bytes of `text`. Returns false when out of
 * memory. */
bool XwauLines_Add(XwauLines* lines, const char* text, size_t len);

/* Writes the trimmed value of the first line with `key`, or "" when there
 * is none. The value is truncated to `cap`. */
void XwauLines_Value(const XwauLines* lines, const char* key, XwauRules rules, char* out, size_t cap);

/* Writes the model key of `path` ("FlightModels\\XWing.opt" gives
 * "xwing"): its file name, in lower case, without the folder and the
 * extension. Returns false when the key is empty or doesn't fit in `cap`. */
bool Xwau_ModelKey(const char* path, char* out, size_t cap);

/* Writes ISO-8859-1 `text` as a quoted YAML string, in UTF-8. */
void Xwau_WriteString(FILE* f, const char* text);

/* Splits `value` into tokens at ',' and ';', and trims each one. With
 * XWAU_RULES_CPP, the empty text between two separators is skipped, but a
 * token of spaces is kept, as an empty token: "a,,b" gives "a" and "b", and
 * "a, ,b" gives "a", "" and "b". Returns false when out of memory. */
bool Xwau_Tokenize(const char* value, XwauRules rules, XwauLines* out);

/* Returns whether two strings are equal, comparing ISO-8859-1 letters in
 * any case. */
bool Xwau_SameName(const char* a, const char* b);

/* The ISO-8859-1 lower case of `c`. */
char Xwau_Lower(char c);

/* Trims the white space of `rules` from `s` in place, and returns `s`. */
char* Xwau_Trim(char* s, XwauRules rules);

/* Parses `text` as std::stoi(text) does in base 10: leading white space,
 * an optional sign, then digits up to the first other character. Returns
 * false where stoi would throw, which is when there are no digits or the
 * value does not fit in an int. */
bool Xwau_Stoi(const char* text, long long* out);

/* Parses `text` as std::stoi(text, 0, 0) does: a hexadecimal number with
 * 0x, an octal number with a leading 0, or a decimal number. Returns false
 * when there are no digits. */
bool Xwau_StoiAuto(const char* text, long long* out);

/* Converts a DAT path written in XWAU's files ("Effects\\GenericDamage.dat")
 * into a lower-case relative path ("effects/genericdamage.dat") and a
 * library name ("genericdamage"). Returns false when one of them does not
 * fit, which is reported. */
bool Xwau_DatNames(const char* written, char* rel, size_t rel_cap, char* library, size_t lib_cap);

/* Encodes a profile, skin or model name as a part of a tag, the same way as
 * the runtime does. ASCII letters become lower case, and digits and '_' are
 * kept. Every other byte is written as '.' and two hex digits
 * ("Con-L+K" -> "con.2dl.2bk"). Returns false when the result does not fit
 * in `cap`. */
bool Xwau_EncodeName(const char* name, char* out, size_t cap);

#endif /* AERON_XWAU_CONFIG_H */
