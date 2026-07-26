#include "PresetMatcher.h"
#include "UnicodeUtil.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <tlhelp32.h>

#include <new>
#include <vector>

namespace
{
std::vector<std::wstring> SplitUnicodeCharacters(const std::wstring& value)
{
	std::vector<std::wstring> characters;
	characters.reserve(value.size());
	for (size_t i = 0; i < value.size(); ++i)
	{
		const wchar_t codeUnit = value[i];
		if (codeUnit >= 0xd800 && codeUnit <= 0xdbff && i + 1 < value.size() &&
			value[i + 1] >= 0xdc00 && value[i + 1] <= 0xdfff)
		{
			characters.push_back(value.substr(i, 2));
			++i;
		}
		else
		{
			characters.push_back(value.substr(i, 1));
		}
	}
	return characters;
}

bool SameCharacterIgnoreCase(const std::wstring& left, const std::wstring& right)
{
	return CompareStringOrdinal(left.data(), static_cast<int>(left.size()),
		right.data(), static_cast<int>(right.size()), TRUE) == CSTR_EQUAL;
}

bool WildcardMatchCharacters(const std::vector<std::wstring>& pattern,
	const std::vector<std::wstring>& value)
{
	size_t patternIndex = 0;
	size_t valueIndex = 0;
	size_t starIndex = static_cast<size_t>(-1);
	size_t starValueIndex = 0;

	while (valueIndex < value.size())
	{
		if (patternIndex < pattern.size() && pattern[patternIndex] == L"*")
		{
			starIndex = patternIndex++;
			starValueIndex = valueIndex;
		}
		else if (patternIndex < pattern.size() &&
			(pattern[patternIndex] == L"?" ||
			 SameCharacterIgnoreCase(pattern[patternIndex], value[valueIndex])))
		{
			++patternIndex;
			++valueIndex;
		}
		else if (starIndex != static_cast<size_t>(-1))
		{
			patternIndex = starIndex + 1;
			valueIndex = ++starValueIndex;
		}
		else
		{
			return false;
		}
	}

	while (patternIndex < pattern.size() && pattern[patternIndex] == L"*")
	{
		++patternIndex;
	}
	return patternIndex == pattern.size();
}

void ClearDiagnostic(std::string* diagnostic)
{
	if (diagnostic != nullptr)
	{
		diagnostic->clear();
	}
}

std::string WindowsDiagnostic(const char* operation, DWORD errorCode)
{
	return std::string(operation) + " failed (Windows error " +
		std::to_string(static_cast<unsigned long>(errorCode)) + ")";
}

class ScopedSnapshot
{
public:
	explicit ScopedSnapshot(HANDLE handle)
		: handle_(handle)
	{
	}

	~ScopedSnapshot()
	{
		if (handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE)
		{
			CloseHandle(handle_);
		}
	}

	ScopedSnapshot(const ScopedSnapshot&) = delete;
	ScopedSnapshot& operator=(const ScopedSnapshot&) = delete;

	HANDLE Get() const
	{
		return handle_;
	}

	bool Close(std::string* diagnostic)
	{
		if (handle_ == nullptr || handle_ == INVALID_HANDLE_VALUE)
		{
			return true;
		}
		if (!CloseHandle(handle_))
		{
			const DWORD errorCode = GetLastError();
			if (diagnostic != nullptr)
			{
				*diagnostic = WindowsDiagnostic("CloseHandle", errorCode);
			}
			return false;
		}
		handle_ = INVALID_HANDLE_VALUE;
		return true;
	}

private:
	HANDLE handle_;
};
}

bool WildcardMatch(const std::string& pattern, const std::string& value)
{
	std::wstring widePattern;
	std::wstring wideValue;
	return Utf8ToWide(pattern, &widePattern) && Utf8ToWide(value, &wideValue) &&
		WildcardMatch(widePattern, wideValue);
}

bool WildcardMatch(const std::wstring& pattern, const std::wstring& value)
{
	return WildcardMatchCharacters(
		SplitUnicodeCharacters(pattern), SplitUnicodeCharacters(value));
}

bool FindMatchingPresetIndex(const PresetCollection& collection, const std::vector<std::string>& processNames, int* index)
{
	std::vector<std::wstring> wideProcessNames;
	wideProcessNames.reserve(processNames.size());
	for (size_t i = 0; i < processNames.size(); ++i)
	{
		std::wstring name;
		if (Utf8ToWide(processNames[i], &name)) wideProcessNames.push_back(name);
	}
	return FindMatchingPresetIndex(collection, wideProcessNames, index);
}

bool FindMatchingPresetIndex(const PresetCollection& collection,
	const std::vector<std::wstring>& processNames, int* index)
{
	if (index == nullptr)
	{
		return false;
	}
	*index = -1;

	for (size_t presetIndex = 0; presetIndex < collection.presets.size(); ++presetIndex)
	{
		const FanPreset& preset = collection.presets[presetIndex];
		std::wstring widePattern;
		if (!Utf8ToWide(preset.processPattern, &widePattern))
		{
			continue;
		}
		for (size_t processIndex = 0; processIndex < processNames.size(); ++processIndex)
		{
			if (WildcardMatch(widePattern, processNames[processIndex]))
			{
				*index = static_cast<int>(presetIndex);
				return true;
			}
		}
	}
	return false;
}

int ResolveAutomaticPresetIndex(const PresetCollection& collection, const std::vector<std::string>& processNames)
{
	int index = -1;
	FindMatchingPresetIndex(collection, processNames, &index);
	return index;
}

int ResolveAutomaticPresetIndex(const PresetCollection& collection,
	const std::vector<std::wstring>& processNames)
{
	int index = -1;
	FindMatchingPresetIndex(collection, processNames, &index);
	return index;
}

bool CollectRunningProcessNames(std::vector<std::wstring>* processNames, std::string* diagnostic)
{
	if (processNames == nullptr)
	{
		if (diagnostic != nullptr)
		{
			*diagnostic = "process name output is null";
		}
		return false;
	}
	processNames->clear();
	ClearDiagnostic(diagnostic);

	ScopedSnapshot snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
	if (snapshot.Get() == INVALID_HANDLE_VALUE)
	{
		const DWORD errorCode = GetLastError();
		if (diagnostic != nullptr)
		{
			*diagnostic = WindowsDiagnostic("CreateToolhelp32Snapshot", errorCode);
		}
		return false;
	}

	PROCESSENTRY32W entry = {};
	entry.dwSize = sizeof(entry);
	if (!Process32FirstW(snapshot.Get(), &entry))
	{
		const DWORD errorCode = GetLastError();
		if (diagnostic != nullptr)
		{
			*diagnostic = WindowsDiagnostic("Process32FirstW", errorCode);
		}
		return false;
	}

	for (;;)
	{
		try
		{
			processNames->push_back(entry.szExeFile);
		}
		catch (const std::bad_alloc&)
		{
			if (diagnostic != nullptr)
			{
				*diagnostic = "cannot collect process names: out of memory";
			}
			return false;
		}
		if (!Process32NextW(snapshot.Get(), &entry))
		{
			const DWORD errorCode = GetLastError();
			if (errorCode == ERROR_NO_MORE_FILES)
			{
				return snapshot.Close(diagnostic);
			}
			if (diagnostic != nullptr)
			{
				*diagnostic = WindowsDiagnostic("Process32NextW", errorCode);
			}
			return false;
		}
	}
}
