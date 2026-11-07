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
#include <stdint.h>
#include <stdio.h>

/* Turns the folder cache of Xwau_FindFile and Xwau_FindDir on or off. While it
 * is on, each folder is read only once, so files that are added or removed
 * later aren't seen. Turn it on only while the folders don't change, as during
 * an import. Turning it off frees the cache. Don't call it while another thread
 * is looking up files. */
void Xwau_SetFolderCache(bool on);

/* Finds the file at `rel` in the installation `game`. Returns false when
 * there is none. */
bool Xwau_FindFile(const char* game, const char* rel, char* out, size_t cap);

/* Finds the folder at `rel` in the installation `game`. Returns false when
 * there is none. */
bool Xwau_FindDir(const char* game, const char* rel, char* out, size_t cap);

/* Returns whether the file at `path` holds the bytes of `text`. The
 * importer uses it to find the features of a hook DLL from its strings,
 * since the mods don't ship the same hook versions. While the folder cache
 * is on, each file is read once. */
bool Xwau_FileHasText(const char* path, const char* text);
/* Whether the installation `game` has the file `dll`, such as
 * "hook_weapon_rate.dll", and the file holds `text`. */
bool Xwau_DllHasText(const char* game, const char* dll, const char* text);

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
 * is none. A value that doesn't fit in `cap` is cut, and this makes the
 * import fail. */
void XwauLines_Value(const XwauLines* lines, const char* key, XwauRules rules, char* out, size_t cap);

/* Whether a key is set, as the hooks' GetFileKeyValueInt reads it. */
typedef enum XwauValue {
	XWAU_VALUE_MISSING, /* no line has the key, or its value is empty */
	XWAU_VALUE_BAD,     /* the value isn't a number, which crashes XWAU */
	XWAU_VALUE_SET,
} XwauValue;

/* Reads `key` as std::stoi(value, 0, 0) reads it. A value out of the int
 * range is bad, since std::stoi then throws. `out` is only set for
 * XWAU_VALUE_SET. */
XwauValue XwauLines_Int(const XwauLines* lines, XwauRules rules, const char* key, long long* out);

/* Writes the trimmed key before the '=' of `line` into `key`. Returns false
 * when the line has no '=' or no key. */
bool Xwau_LineKey(const char* line, XwauRules rules, char* key, size_t cap);
/* Xwau_LineKey, which also writes the trimmed value after the '=' into
 * `value`. */
bool Xwau_SplitLine(const char* line, XwauRules rules, char* key, size_t key_cap, char* value,
					size_t value_cap);

/* The lines of one section of a hook's settings, and where they come from.
 *
 * The hooks read a section from a file of its own, or else from a section
 * of a shared file, when the first file has no lines. For example
 * FlightModels/AwingWarheadLock.txt, else the [WarheadLock] section of
 * FlightModels/Awing.ini. */
typedef struct XwauSection {
	XwauLines lines;
	XwauRules rules;
	/* The file that the lines come from, for reports, such as
	   "FlightModels/Awing.ini [WarheadLock]". Empty without lines. */
	char source[1100];
} XwauSection;

/* Reads the lines of the file `txt`, or else of the section `section` of
 * `ini`, when `txt` has none. Either path can be NULL. The paths are full
 * paths, as Xwau_FindFile gives them. Returns false when out of memory. */
bool XwauSection_ReadPaths(const char* txt, const char* ini, const char* section, XwauRules rules,
						   XwauSection* out);
/* XwauSection_ReadPaths with paths relative to the installation `game`. */
bool XwauSection_Read(const char* game, const char* txt, const char* ini, const char* section,
					  XwauRules rules, XwauSection* out);
/* A model's section: FlightModels/<model><section>.txt, else the
 * [<section>] section of FlightModels/<model>.ini. */
bool XwauSection_ReadModel(const char* game, const char* model, const char* section, XwauRules rules,
						   XwauSection* out);
/* A global section: FlightModels/<section>.txt, else the [<section>]
 * section of FlightModels/default.ini. */
bool XwauSection_ReadGlobal(const char* game, const char* section, XwauRules rules, XwauSection* out);
/* A mission's section. `base` is the mission's path without its extension,
 * such as "Missions/1b0m1": <base>_<section>.txt, else the [<section>]
 * section of <base>.ini. */
bool XwauSection_ReadMission(const char* game, const char* base, const char* section, XwauRules rules,
							 XwauSection* out);
/* A hook's settings: <hook>.cfg, else the [<hook>] section of Hooks.ini. */
bool XwauSection_ReadHook(const char* game, const char* hook, XwauSection* out);
void XwauSection_Free(XwauSection* section);

/* Writes the value of `key` into `out`, or "" without it. */
void XwauSection_Text(const XwauSection* section, const char* key, char* out, size_t cap);
/* Reads `key` as std::stoi(value, 0, 0) reads it. A value that isn't a
 * number, or that is out of the int range, is reported, and `out` is then
 * left alone. */
XwauValue XwauSection_Int(const XwauSection* section, const char* key, long long* out);
/* The value of `key`, or `fallback` when it isn't set or isn't a number. */
long long XwauSection_IntOr(const XwauSection* section, const char* key, long long fallback);
/* Reads `key` as std::stof(value) reads it, and reports a value that isn't
 * a number. */
XwauValue XwauSection_Float(const XwauSection* section, const char* key, float* out);
float     XwauSection_FloatOr(const XwauSection* section, const char* key, float fallback);

/* Writes the key of line `index` into `key`. Returns false when the line
 * has no key, or when an earlier line has the same key: the hooks only read
 * the first line of a key. */
bool XwauSection_KeyAt(const XwauSection* section, size_t index, char* key, size_t cap);

/* Reports each key that the hook doesn't read. `keys` and `prefixes` end
 * with NULL, and either can be NULL. A key that starts with one of the
 * prefixes, and has more after it, is read. */
void XwauSection_ReportUnknownKeys(const XwauSection* section, const char* const* keys,
								   const char* const* prefixes);

/* Writes the model key of `path` ("FlightModels\\XWing.opt" gives
 * "xwing"): its file name, in lower case, without the folder and the
 * extension. Returns false when the key is empty or doesn't fit in `cap`. */
bool Xwau_ModelKey(const char* path, char* out, size_t cap);

/* Writes ISO-8859-1 `text` as a quoted YAML string, in UTF-8. */
void Xwau_WriteString(FILE* f, const char* text);

/* Writes `value` with at most three decimals and no trailing zeros. Three
 * decimals are enough to get the exact game value back from a value in
 * any XwauUnit. */
void Xwau_WriteDecimal(FILE* f, double value);

/* The units of the mod files. The comment of each unit gives the game's
 * unit that it converts to. */
typedef enum XwauUnit {
	XWAU_UNIT_NONE,         /* the game's own value */
	XWAU_UNIT_METRES,       /* game units, 40.96 to the metre */
	XWAU_UNIT_MILLISECONDS, /* ticks of 4 ms */
	XWAU_UNIT_MGLT,         /* speed units, 2.25 to the MGLT */
	XWAU_UNIT_DEGREES,      /* angle units, 65536 to the turn */
	XWAU_UNIT_PERCENT_Q16,  /* a Q16 value, where 65536 is 100% */
} XwauUnit;

/* Converts `game`, a value in the game's unit, to `unit`. */
double Xwau_FromGame(XwauUnit unit, double game);
/* Writes `game`, a value in the game's unit, in `unit`. */
void Xwau_WriteUnit(FILE* f, XwauUnit unit, long long game);
/* Converts a length in game units (40.96 per metre) to metres. */
double Xwau_Metres(double units);
/* Writes a length in game units as metres. */
void Xwau_WriteMetres(FILE* f, int units);
/* Writes [x, y, z] in game units as a list of metres. */
void Xwau_WriteMetres3(FILE* f, int x, int y, int z);
/* Writes a 16-bit angle (65536 steps per turn) as degrees. */
void Xwau_WriteDegrees(FILE* f, int steps);

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
/* Xwau_StoiAuto, which also returns false when the value doesn't fit in an
 * int, as std::stoi then throws. */
bool Xwau_StoiAutoInt(const char* text, long long* out);

/* Parses `text` as std::stof(text) does: a float up to the first other
 * character. Returns false where stof would throw, which is when there are
 * no digits or the value is out of the float range. */
bool Xwau_Stof(const char* text, float* out);

/* Parses `text` as std::stoul(text, 0, 16) does with a 32-bit unsigned
 * long: leading white space, an optional sign, an optional 0x, then hex
 * digits up to the first other character. A negative value wraps around.
 * Returns false where stoul would throw, which is when there are no digits
 * or the value doesn't fit in 32 bits. */
bool Xwau_StoulHex(const char* text, uint32_t* out);

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

/* Called for each `key = value` line of a settings file. `value` is the
 * rest of the line after the '='. */
typedef void (*XwauSettingFn)(const char* key, const char* value, void* data);

/* Reads the settings file `name` of the installation `game` as ddraw and
 * Hook_CockpitLook read theirs, with sscanf("%s = %s"): a line is
 * `key = value`, lines that start with ';' or '#' are comments, and there
 * must be white space between the key and the '='. Calls `fn` for each
 * setting, in the order of the file, so the last line with a key wins.
 * Returns false when there is no such file. */
bool Xwau_ReadSettings(const char* game, const char* name, XwauSettingFn fn, void* data);

/* Reads `count` numbers, 1 or 3, from the value of a setting into `out`, as
 * ddraw reads them: one with atof, three with sscanf("%f, %f, %f"). Returns
 * false when there are fewer. */
bool Xwau_ReadSettingNumbers(const char* text, int count, double* out);

#endif /* AERON_XWAU_CONFIG_H */
