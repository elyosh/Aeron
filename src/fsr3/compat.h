#pragma once

#include <cerrno>
#include <cstddef>
#include <cwchar>

#include <FidelityFX/host/ffx_util.h>

#if !defined(_WIN32)
#define _countof(array) (sizeof(array) / sizeof((array)[0]))

template <std::size_t N> static int aeronWcscpy(wchar_t (&destination)[N], const wchar_t* source) {
	if (!source) {
		destination[0] = L'\0';
		return EINVAL;
	}

	const std::size_t length = std::wcslen(source);
	if (length >= N) {
		destination[0] = L'\0';
		return ERANGE;
	}

	std::wmemcpy(destination, source, length + 1);
	return 0;
}

#define wcscpy_s(destination, source) aeronWcscpy(destination, source)
#endif
