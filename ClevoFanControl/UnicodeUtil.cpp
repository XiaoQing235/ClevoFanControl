#include "UnicodeUtil.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <limits>

namespace
{
void SetDiagnostic(std::string* diagnostic, const char* message)
{
	if (diagnostic != nullptr)
	{
		*diagnostic = message;
	}
}

bool MultiByteToWide(UINT codePage, DWORD flags, const std::string& input,
	std::wstring* output, std::string* diagnostic)
{
	if (output == nullptr)
	{
		SetDiagnostic(diagnostic, "wide string output is null");
		return false;
	}
	if (input.empty())
	{
		output->clear();
		if (diagnostic != nullptr) diagnostic->clear();
		return true;
	}
	if (input.size() > static_cast<size_t>((std::numeric_limits<int>::max)()))
	{
		SetDiagnostic(diagnostic, "input string is too large to convert");
		return false;
	}

	const int sourceLength = static_cast<int>(input.size());
	const int required = MultiByteToWideChar(codePage, flags,
		input.data(), sourceLength, nullptr, 0);
	if (required == 0)
	{
		SetDiagnostic(diagnostic, "multi-byte string contains invalid text");
		return false;
	}
	std::wstring converted(static_cast<size_t>(required), L'\0');
	if (MultiByteToWideChar(codePage, flags, input.data(), sourceLength,
		&converted[0], required) != required)
	{
		SetDiagnostic(diagnostic, "multi-byte string conversion failed");
		return false;
	}
	output->swap(converted);
	if (diagnostic != nullptr) diagnostic->clear();
	return true;
}
}

bool Utf8ToWide(const std::string& input, std::wstring* output,
	std::string* diagnostic)
{
	return MultiByteToWide(CP_UTF8, MB_ERR_INVALID_CHARS, input, output, diagnostic);
}

bool WideToUtf8(const std::wstring& input, std::string* output,
	std::string* diagnostic)
{
	if (output == nullptr)
	{
		SetDiagnostic(diagnostic, "UTF-8 output is null");
		return false;
	}
	if (input.empty())
	{
		output->clear();
		if (diagnostic != nullptr) diagnostic->clear();
		return true;
	}
	if (input.size() > static_cast<size_t>((std::numeric_limits<int>::max)()))
	{
		SetDiagnostic(diagnostic, "wide string is too large to convert");
		return false;
	}

	const int sourceLength = static_cast<int>(input.size());
	const int required = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
		input.data(), sourceLength, nullptr, 0, nullptr, nullptr);
	if (required == 0)
	{
		SetDiagnostic(diagnostic, "wide string contains invalid Unicode");
		return false;
	}
	std::string converted(static_cast<size_t>(required), '\0');
	if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, input.data(), sourceLength,
		&converted[0], required, nullptr, nullptr) != required)
	{
		SetDiagnostic(diagnostic, "wide string conversion failed");
		return false;
	}
	output->swap(converted);
	if (diagnostic != nullptr) diagnostic->clear();
	return true;
}

bool AnsiToWide(const std::string& input, std::wstring* output,
	std::string* diagnostic)
{
	return MultiByteToWide(CP_ACP, 0, input, output, diagnostic);
}
