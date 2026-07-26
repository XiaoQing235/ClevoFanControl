#ifndef PRESET_STORE_H
#define PRESET_STORE_H

#include "FanConfig.h"

#include <string>
#include <vector>

struct FanPreset
{
	std::string name;
	std::string processPattern;
	FanConfig config;
};

struct PresetCollection
{
	bool autoSwitch;
	std::vector<FanPreset> presets;

	PresetCollection();
	void LoadDefault();
	bool Validate(std::string* error) const;
};

enum class PresetLoadStatus
{
	Loaded,
	Missing,
	Invalid,
	IoError
};

class PresetStore
{
public:
	static const char* FileName();
	static const wchar_t* WideFileName();
	static PresetLoadStatus Load(const std::string& path, PresetCollection* output, std::string* diagnostic);
	static PresetLoadStatus Load(const std::wstring& path, PresetCollection* output, std::string* diagnostic);
	static bool Save(const std::string& path, const PresetCollection& collection, std::string* diagnostic);
	static bool Save(const std::wstring& path, const PresetCollection& collection, std::string* diagnostic);
};

bool SameControlSettings(const FanConfig& left, const FanConfig& right);
void CopyControlSettings(const FanConfig& source, FanConfig* target);

#endif
