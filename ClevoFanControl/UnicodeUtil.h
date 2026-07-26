#pragma once

#include <string>

bool Utf8ToWide(const std::string& input, std::wstring* output,
	std::string* diagnostic = nullptr);
bool WideToUtf8(const std::wstring& input, std::string* output,
	std::string* diagnostic = nullptr);
bool AnsiToWide(const std::string& input, std::wstring* output,
	std::string* diagnostic = nullptr);
