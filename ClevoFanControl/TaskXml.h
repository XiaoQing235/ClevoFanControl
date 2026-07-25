#pragma once

#include <string>

bool BuildTaskXmlUtf8(
	const std::wstring& targetPath,
	std::string* output,
	std::string* diagnostic);
