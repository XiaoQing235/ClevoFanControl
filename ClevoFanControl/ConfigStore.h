#ifndef CONFIG_STORE_H
#define CONFIG_STORE_H

#include "FanConfig.h"

#include <string>

enum class ConfigLoadStatus
{
	Loaded,
	Missing,
	Invalid,
	IoError
};

class ConfigStore
{
public:
	static const char* FileName();
	static const wchar_t* WideFileName();
	static ConfigLoadStatus Load(const std::string& path, FanConfig* output, std::string* diagnostic);
	static ConfigLoadStatus Load(const std::wstring& path, FanConfig* output, std::string* diagnostic);
	static bool Save(const std::string& path, const FanConfig& config, std::string* diagnostic);
	static bool Save(const std::wstring& path, const FanConfig& config, std::string* diagnostic);
};

#endif
