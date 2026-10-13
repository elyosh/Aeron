#ifndef AERON_OPT2GLTF_XWAU_CONFIG_H
#define AERON_OPT2GLTF_XWAU_CONFIG_H

/*
 * XWA Upgrade (XWAU) data folders and the hooks' configuration files.
 *
 * The roots are layered game folders (an installation, or unpacked package
 * groups), searched in order: the first one holding a file wins. Paths are
 * game-relative ('\' or '/'), every component matched case-insensitively.
 *
 * The hooks read their .ini and .txt files with two readers, whose rules
 * differ and are both reproduced:
 *   - XWAU_RULES_CPP, the C++ hooks' config.cpp: lines end at '\n' and are
 *     trimmed of " \t\n\r\f\v"; tokens split on ',' and ';', trimmed, empty
 *     ones dropped;
 *   - XWAU_RULES_CS, the C# hooks' XwaHooksConfig (hook_32bpp's skins):
 *     lines end at "\r\n", '\r' or '\n' and are also trimmed of U+0085 and
 *     U+00A0; empty tokens are kept.
 * Both skip empty lines and lines starting with '#', ';' or "//"; a section
 * is a whole line "[name]", matched case-insensitively; a key lookup takes
 * the first line whose trimmed name before '=' matches case-insensitively.
 * Text is ISO-8859-1.
 */

#include <stdbool.h>
#include <stddef.h>

#define XWAU_MAX_ROOTS 16

typedef struct XwauRoots {
	const char* dirs[XWAU_MAX_ROOTS];
	int         count;
} XwauRoots;

/* The first root's file at `rel`; false when no root has it. */
bool XwauRoots_FindFile(const XwauRoots* roots, const char* rel, char* out, size_t cap);

/* Root `root`'s folder at `rel`; false when it has none. */
bool XwauRoots_FindDir(const XwauRoots* roots, int root, const char* rel, char* out, size_t cap);

/* The entries of folder `dir` (not "." and ".."), in the order the file
 * system lists them, or sorted by name with `sorted`. NULL with *count 0
 * when it cannot be read. */
char** Xwau_ListDir(const char* dir, bool sorted, size_t* count);
void   Xwau_FreeList(char** names, size_t count);

typedef enum XwauRules { XWAU_RULES_CPP, XWAU_RULES_CS } XwauRules;

typedef struct XwauLines {
	char** items;
	size_t count;
} XwauLines;

/* The lines of `section` of the file `path`, or of the whole file with an
 * empty section (until a section starts). No file (`path` NULL or
 * unreadable) gives no lines. False when out of memory. */
bool XwauLines_Read(const char* path, const char* section, XwauRules rules, XwauLines* out);
void XwauLines_Free(XwauLines* lines);

/* The value of the first line with `key`, trimmed, or "" (truncated to
 * `cap`). */
void XwauLines_Value(const XwauLines* lines, const char* key, XwauRules rules, char* out, size_t cap);

/* The tokens of `value`. False when out of memory. */
bool Xwau_Tokenize(const char* value, XwauRules rules, XwauLines* out);

/* Whether two strings are equal, ISO-8859-1 letters compared without case. */
bool Xwau_SameName(const char* a, const char* b);

/* The ISO-8859-1 lower case of `c`. */
char Xwau_Lower(char c);

/* Trims `s` in place by the rules' white space; returns it. */
char* Xwau_Trim(char* s, XwauRules rules);

/* std::stoi(text) (base 10): leading white space, an optional sign and
 * digits up to the first other character. False where stoi throws (no
 * digits, or outside the int range). */
bool Xwau_Stoi(const char* text, long long* out);

/* std::stoi(text, 0, 0): a 0x hexadecimal, leading-0 octal or decimal
 * number. False without digits. */
bool Xwau_StoiAuto(const char* text, long long* out);

/* A copy of `text`, or of its `len` first bytes; NULL when out of memory. */
char* Xwau_Strdup(const char* text);
char* Xwau_Strndup(const char* text, size_t len);

/* Creates folder `path` and its missing parents; true when it exists. */
bool Xwau_MakeDir(const char* path);

/* Removes `path`, a file or a folder and everything in it. */
bool Xwau_RemoveTree(const char* path);

/* A profile, skin or model name as a tag part: lower-case ASCII letters,
 * digits and '_', every other byte written as '.' and two hex digits
 * ("Con-L+K" -> "con.2dl.2bk"), as the runtime encodes it. False when it
 * does not fit `cap`. */
bool Xwau_EncodeName(const char* name, char* out, size_t cap);

#endif /* AERON_OPT2GLTF_XWAU_CONFIG_H */
