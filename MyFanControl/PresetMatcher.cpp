#include "PresetMatcher.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <tlhelp32.h>

#include <new>

namespace
{
char FoldAscii(char value)
{
	if (value >= 'A' && value <= 'Z')
	{
		return static_cast<char>(value + ('a' - 'A'));
	}
	return value;
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
	size_t patternIndex = 0;
	size_t valueIndex = 0;
	size_t starIndex = std::string::npos;
	size_t starValueIndex = 0;

	while (valueIndex < value.size())
	{
		if (patternIndex < pattern.size() && pattern[patternIndex] == '*')
		{
			starIndex = patternIndex++;
			starValueIndex = valueIndex;
		}
		else if (patternIndex < pattern.size() &&
			(pattern[patternIndex] == '?' ||
			 FoldAscii(pattern[patternIndex]) == FoldAscii(value[valueIndex])))
		{
			++patternIndex;
			++valueIndex;
		}
		else if (starIndex != std::string::npos)
		{
			patternIndex = starIndex + 1;
			valueIndex = ++starValueIndex;
		}
		else
		{
			return false;
		}
	}

	while (patternIndex < pattern.size() && pattern[patternIndex] == '*')
	{
		++patternIndex;
	}
	return patternIndex == pattern.size();
}

bool FindMatchingPresetIndex(const PresetCollection& collection, const std::vector<std::string>& processNames, int* index)
{
	if (index == nullptr)
	{
		return false;
	}
	*index = -1;

	for (size_t presetIndex = 0; presetIndex < collection.presets.size(); ++presetIndex)
	{
		const FanPreset& preset = collection.presets[presetIndex];
		for (size_t processIndex = 0; processIndex < processNames.size(); ++processIndex)
		{
			if (WildcardMatch(preset.processPattern, processNames[processIndex]))
			{
				*index = static_cast<int>(presetIndex);
				return true;
			}
		}
	}
	return false;
}

bool CollectRunningProcessNames(std::vector<std::string>* processNames, std::string* diagnostic)
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

	// The project uses MultiByte; the SDK's unsuffixed Toolhelp APIs are ANSI here.
	PROCESSENTRY32 entry = {};
	entry.dwSize = sizeof(entry);
	if (!Process32First(snapshot.Get(), &entry))
	{
		const DWORD errorCode = GetLastError();
		if (diagnostic != nullptr)
		{
			*diagnostic = WindowsDiagnostic("Process32First", errorCode);
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
		if (!Process32Next(snapshot.Get(), &entry))
		{
			const DWORD errorCode = GetLastError();
			if (errorCode == ERROR_NO_MORE_FILES)
			{
				return snapshot.Close(diagnostic);
			}
			if (diagnostic != nullptr)
			{
				*diagnostic = WindowsDiagnostic("Process32Next", errorCode);
			}
			return false;
		}
	}
}
