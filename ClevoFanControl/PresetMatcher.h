#ifndef PRESET_MATCHER_H
#define PRESET_MATCHER_H

#include "PresetStore.h"

#include <string>
#include <vector>

bool WildcardMatch(const std::string& pattern, const std::string& value);
bool WildcardMatch(const std::wstring& pattern, const std::wstring& value);
bool FindMatchingPresetIndex(const PresetCollection& collection, const std::vector<std::string>& processNames, int* index);
bool FindMatchingPresetIndex(const PresetCollection& collection, const std::vector<std::wstring>& processNames, int* index);
int ResolveAutomaticPresetIndex(const PresetCollection& collection, const std::vector<std::string>& processNames);
int ResolveAutomaticPresetIndex(const PresetCollection& collection, const std::vector<std::wstring>& processNames);
bool CollectRunningProcessNames(std::vector<std::wstring>* processNames, std::string* diagnostic);

#endif
