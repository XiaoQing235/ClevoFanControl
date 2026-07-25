#ifndef PRESET_MATCHER_H
#define PRESET_MATCHER_H

#include "PresetStore.h"

#include <string>
#include <vector>

bool WildcardMatch(const std::string& pattern, const std::string& value);
bool FindMatchingPresetIndex(const PresetCollection& collection, const std::vector<std::string>& processNames, int* index);
bool CollectRunningProcessNames(std::vector<std::string>* processNames, std::string* diagnostic);

#endif
