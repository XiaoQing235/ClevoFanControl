#include "FanCurveModel.h"
#include "ConfigStore.h"
#include "FanConfig.h"
#include "PresetMatcher.h"
#include "PresetStore.h"
#include "SingleInstance.h"
#include "TaskXml.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <exception>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <stdint.h>
#include <vector>

namespace
{
void Expect(bool condition, const char* message)
{
	if (!condition)
	{
		throw std::runtime_error(message);
	}
}

bool SamePoints(const FanCurvePoints& left, const FanCurvePoints& right)
{
	if (left.size() != right.size())
	{
		return false;
	}

	for (size_t i = 0; i < left.size(); ++i)
	{
		if (left[i].temperature != right[i].temperature || left[i].duty != right[i].duty)
		{
			return false;
		}
	}
	return true;
}

bool SameConfig(const FanConfig& left, const FanConfig& right)
{
	return SamePoints(left.CpuCurve, right.CpuCurve) &&
		SamePoints(left.GpuCurve, right.GpuCurve) &&
		left.TransitionTemp == right.TransitionTemp &&
		left.UpdateInterval == right.UpdateInterval &&
		left.ForceTemp == right.ForceTemp &&
		left.UiFontSize == right.UiFontSize &&
		left.Linear == right.Linear &&
		left.TakeOver == right.TakeOver &&
		left.ForceCooling == right.ForceCooling &&
		left.SoftControl == right.SoftControl &&
		left.AutoRun == right.AutoRun &&
		left.CloseToTray == right.CloseToTray;
}

bool SameControlConfig(const FanConfig& left, const FanConfig& right)
{
	return SamePoints(left.CpuCurve, right.CpuCurve) &&
		SamePoints(left.GpuCurve, right.GpuCurve) &&
		left.TransitionTemp == right.TransitionTemp &&
		left.UpdateInterval == right.UpdateInterval &&
		left.ForceTemp == right.ForceTemp &&
		left.Linear == right.Linear &&
		left.TakeOver == right.TakeOver &&
		left.ForceCooling == right.ForceCooling &&
		left.SoftControl == right.SoftControl;
}

FanPreset MakePreset(const char* name, const char* pattern, int dutyOffset)
{
	FanPreset preset;
	preset.name = name;
	preset.processPattern = pattern;
	preset.config.LoadDefault();
	preset.config.CpuCurve[0].duty += dutyOffset;
	preset.config.GpuCurve[0].duty += dutyOffset;
	preset.config.TransitionTemp = 4;
	preset.config.UpdateInterval = 1;
	preset.config.ForceTemp = 70;
	preset.config.Linear = true;
	preset.config.TakeOver = true;
	preset.config.ForceCooling = true;
	preset.config.SoftControl = true;
	preset.config.UiFontSize = 16;
	preset.config.AutoRun = true;
	preset.config.CloseToTray = true;
	return preset;
}

class TempDirectory
{
public:
	TempDirectory()
	{
		char tempDirectory[MAX_PATH];
		const DWORD length = GetTempPathA(static_cast<DWORD>(sizeof(tempDirectory)), tempDirectory);
		if (length == 0U || length >= sizeof(tempDirectory))
		{
			throw std::runtime_error("GetTempPathA failed");
		}

		char uniquePath[MAX_PATH];
		if (GetTempFileNameA(tempDirectory, "mfc", 0U, uniquePath) == 0U)
		{
			throw std::runtime_error("GetTempFileNameA failed");
		}
		if (!DeleteFileA(uniquePath) || !CreateDirectoryA(uniquePath, nullptr))
		{
			DeleteFileA(uniquePath);
			throw std::runtime_error("cannot create temporary test directory");
		}
		directory_ = uniquePath;
	}

	~TempDirectory()
	{
		for (size_t i = 0; i < files_.size(); ++i)
		{
			DeleteFileA(files_[i].c_str());
		}
		RemoveDirectoryA(directory_.c_str());
	}

	std::string File(const char* name)
	{
		const std::string path = directory_ + "\\" + name;
		files_.push_back(path);
		return path;
	}

	const std::string& Path() const
	{
		return directory_;
	}

	void AssertNoTemporaryFiles() const
	{
		WIN32_FIND_DATAA findData;
		const std::string pattern = directory_ + "\\*.tmp.*";
		const HANDLE findHandle = FindFirstFileA(pattern.c_str(), &findData);
		if (findHandle == INVALID_HANDLE_VALUE)
		{
			const DWORD errorCode = GetLastError();
			Expect(errorCode == ERROR_FILE_NOT_FOUND, "cannot inspect temporary test files");
			return;
		}

		FindClose(findHandle);
		throw std::runtime_error("failed save left a temporary file");
	}

private:
	std::string directory_;
	std::vector<std::string> files_;
};

std::string ReadText(const std::string& path)
{
	std::ifstream stream(path.c_str(), std::ios::in | std::ios::binary | std::ios::ate);
	if (!stream.is_open())
	{
		throw std::runtime_error("cannot open test file for reading");
	}
	const std::streampos end = stream.tellg();
	if (end == std::streampos(-1))
	{
		throw std::runtime_error("cannot determine test file length");
	}
	const std::streamoff length = static_cast<std::streamoff>(end);
	if (length < 0)
	{
		throw std::runtime_error("test file length is invalid");
	}
	std::string text(static_cast<size_t>(length), '\0');
	stream.seekg(0, std::ios::beg);
	if (!stream)
	{
		throw std::runtime_error("cannot seek test file");
	}
	if (!text.empty())
	{
		stream.read(&text[0], static_cast<std::streamsize>(text.size()));
	}
	if (!stream || stream.gcount() != static_cast<std::streamsize>(text.size()))
	{
		throw std::runtime_error("cannot read test file");
	}
	return text;
}

void WriteText(const std::string& path, const std::string& text)
{
	std::ofstream stream(path.c_str(), std::ios::out | std::ios::binary | std::ios::trunc);
	if (!stream.is_open())
	{
		throw std::runtime_error("cannot open test file for writing");
	}
	if (!text.empty())
	{
		stream.write(text.data(), static_cast<std::streamsize>(text.size()));
	}
	stream.flush();
	if (!stream)
	{
		throw std::runtime_error("cannot write test file");
	}
	stream.close();
	if (stream.fail())
	{
		throw std::runtime_error("cannot close test file");
	}
}
FanConfig MakeRoundTripConfig()
{
	FanConfig config;
	config.CpuCurve = FanCurvePoints{{35, 12}, {55, 34}, {80, 70}};
	config.GpuCurve = FanCurvePoints{{40, 20}, {60, 50}, {90, 90}, {100, 100}};
	config.TransitionTemp = 7;
	config.UpdateInterval = 5;
	config.ForceTemp = 88;
	config.UiFontSize = 13;
	config.Linear = true;
	config.TakeOver = true;
	config.ForceCooling = true;
	config.SoftControl = true;
	config.AutoRun = true;
	config.CloseToTray = true;
	return config;
}

void ExpectInvalidFile(const std::string& path, const std::string& text, const char* message)
{
	WriteText(path, text);
	const std::string beforeLoad = ReadText(path);
	FanConfig output;
	output.UpdateInterval = 99;
	std::string diagnostic = "stale";
	Expect(ConfigStore::Load(path, &output, &diagnostic) == ConfigLoadStatus::Invalid, message);
	FanConfig defaults;
	Expect(SameConfig(output, defaults), "invalid config should load defaults");
	Expect(diagnostic.find(path) != std::string::npos, "invalid config diagnostic should include path");
	Expect(diagnostic.size() > path.size(), "invalid config diagnostic should include a reason");
	Expect(ReadText(path) == beforeLoad, "invalid config load must not modify the source file");
}

std::string ReplaceFirst(const std::string& text, const std::string& from, const std::string& to)
{
	const size_t position = text.find(from);
	Expect(position != std::string::npos, "test fixture text did not contain the expected fragment");
	std::string result = text;
	result.replace(position, from.size(), to);
	return result;
}

void TestFanConfigDefaultsAndValidation()
{
	FanConfig config;
	FanConfig defaults;
	Expect(SameConfig(config, defaults), "FanConfig defaults differ from legacy values");
	Expect(config.TransitionTemp == 3, "default transition temperature differs from legacy value");
	Expect(config.UpdateInterval == 2, "default update interval differs from legacy value");
	Expect(config.ForceTemp == 50, "default force temperature differs from legacy value");
	Expect(config.UiFontSize == 10, "default UI font size differs from legacy value");
	Expect(!config.Linear && !config.TakeOver && !config.SoftControl &&
		!config.ForceCooling &&
		!config.AutoRun && !config.CloseToTray, "default flags differ from legacy values");

	std::string error = "stale";
	Expect(config.Validate(&error), "default FanConfig should validate");
	Expect(error.empty(), "successful FanConfig validation should clear the error");
	Expect(config.Validate(nullptr), "FanConfig validation should accept a null error pointer");

	config.UpdateInterval = 0;
	Expect(!config.Validate(&error), "invalid update interval should fail FanConfig validation");
	Expect(!error.empty(), "invalid update interval should report an error");
	config.LoadDefault();
	config.TransitionTemp = 11;
	Expect(!config.Validate(&error), "invalid transition temperature should fail validation");
	config.LoadDefault();
	config.ForceTemp = -1;
	Expect(!config.Validate(&error), "invalid force temperature should fail validation");
	config.LoadDefault();
	config.UiFontSize = 7;
	Expect(!config.Validate(&error), "UI font size below minimum should fail validation");
	config.LoadDefault();
	config.UiFontSize = 17;
	Expect(!config.Validate(&error), "UI font size above maximum should fail validation");
	config.LoadDefault();
	config.CpuCurve[0].duty = 101;
	Expect(!config.Validate(&error), "invalid CPU curve should fail FanConfig validation");

}

void TestConfigRoundTrip()
{
	TempDirectory directory;
	const std::string path = directory.File("roundtrip.json");
	const FanConfig expected = MakeRoundTripConfig();
	std::string diagnostic = "stale";
	Expect(ConfigStore::Save(path, expected, &diagnostic), "valid config should save");
	Expect(diagnostic.empty(), "successful save should clear the diagnostic");
	const std::string text = ReadText(path);
	Expect(!text.empty() && text[0] == '{', "config should be written as a JSON object");
	Expect(text.find("\"cpuCurve\"") != std::string::npos, "JSON config should contain the CPU curve");

	FanConfig actual;
	actual.LoadDefault();
	diagnostic = "stale";
	Expect(ConfigStore::Load(path, &actual, &diagnostic) == ConfigLoadStatus::Loaded, "saved config should load");
	Expect(diagnostic.empty(), "successful load should clear the diagnostic");
	Expect(SameConfig(actual, expected), "round-trip config differs by field or point");
}

void TestMissingIoAndVersionedName()
{
	TempDirectory directory;
	const std::string missingPath = directory.File("missing.json");
	FanConfig output;
	output.UpdateInterval = 99;
	std::string diagnostic;
	Expect(ConfigStore::Load(missingPath, &output, &diagnostic) == ConfigLoadStatus::Missing, "missing config should report Missing");
	FanConfig defaults;
	Expect(SameConfig(output, defaults), "missing config should load defaults");
	Expect(GetFileAttributesA(missingPath.c_str()) == INVALID_FILE_ATTRIBUTES, "missing load should not create a file");

	const std::string jsonPath = directory.File(ConfigStore::FileName());
	const std::string legacyPath = directory.File("ClevoFanControl.v3.cfg");
	Expect(ConfigStore::FileName() == std::string("ClevoFanControl.json"), "ConfigStore should use the JSON file name");
	Expect(ConfigStore::Save(jsonPath, defaults, &diagnostic), "JSON config should save");
	Expect(GetFileAttributesA(legacyPath.c_str()) == INVALID_FILE_ATTRIBUTES, "legacy cfg file must not be created");

	Expect(ConfigStore::Load(directory.Path(), &output, &diagnostic) == ConfigLoadStatus::IoError, "directory load should report IoError");
	Expect(SameConfig(output, defaults), "I/O error should load defaults");
}

void TestInvalidConfigFiles()
{
	TempDirectory directory;
	const FanConfig config = MakeRoundTripConfig();
	const std::string validPath = directory.File("valid.json");
	std::string diagnostic;
	Expect(ConfigStore::Save(validPath, config, &diagnostic), "valid fixture should save");
	const std::string valid = ReadText(validPath);
	ExpectInvalidFile(directory.File("malformed.json"), "{", "malformed JSON should be rejected");
	ExpectInvalidFile(directory.File("trailing.json"), valid + "{}", "trailing JSON should be rejected");
	ExpectInvalidFile(directory.File("unknown-field.json"),
		"{\n  \"unknown\": true," + valid.substr(1), "unknown JSON fields should be rejected");
	ExpectInvalidFile(directory.File("wrong-type.json"),
		ReplaceFirst(valid, "\"forceTemp\": 88", "\"forceTemp\": \"88\""),
		"wrong JSON field types should be rejected");
	ExpectInvalidFile(directory.File("invalid-curve.json"),
		ReplaceFirst(valid, "\"duty\": 12", "\"duty\": 101"),
		"invalid curve values should be rejected");
	ExpectInvalidFile(directory.File("old-binary.cfg"), std::string("MFC2\x04\0\0\0", 8),
		"old binary configuration should be rejected");
	ExpectInvalidFile(directory.File("oversized.json"), std::string(64U * 1024U + 1U, 'x'),
		"JSON files over 64 KiB should be rejected");
}

void TestInvalidConfigIsOverwrittenBySave()
{
	TempDirectory directory;
	const std::string path = directory.File("overwrite-invalid.cfg");
	WriteText(path, "MFC2 old configuration");

	FanConfig output;
	std::string diagnostic;
	Expect(ConfigStore::Load(path, &output, &diagnostic) == ConfigLoadStatus::Invalid,
		"incompatible config should be skipped before save");
	const FanConfig expected = MakeRoundTripConfig();
	Expect(ConfigStore::Save(path, expected, &diagnostic),
		"saving a replacement config should overwrite an incompatible file");
	Expect(ConfigStore::Load(path, &output, &diagnostic) == ConfigLoadStatus::Loaded,
		"overwritten config should load in the current format");
	Expect(SameConfig(output, expected), "overwritten config should contain the saved values");
}

void TestFailedReplacementPreservesSentinel()
{
	TempDirectory directory;
	const std::string path = directory.File("sentinel.json");
	const std::string sentinel = "sentinel";
	WriteText(path, sentinel);

	const HANDLE handle = CreateFileA(
		path.c_str(),
		GENERIC_READ,
		FILE_SHARE_READ | FILE_SHARE_WRITE,
		nullptr,
		OPEN_EXISTING,
		FILE_ATTRIBUTE_NORMAL,
		nullptr);
	if (handle == INVALID_HANDLE_VALUE)
	{
		throw std::runtime_error("cannot open sentinel without delete sharing");
	}

	std::string diagnostic;
	const bool saved = ConfigStore::Save(path, MakeRoundTripConfig(), &diagnostic);
	CloseHandle(handle);
	Expect(!saved, "replacement should fail while the target is locked");
	directory.AssertNoTemporaryFiles();
	Expect(ReadText(path) == sentinel, "failed replacement must preserve the sentinel file");
	Expect(diagnostic.find(path) != std::string::npos, "replacement failure should include the target path");
}

void TestDefaultCurve()
{
	const FanCurvePoints expected{
		FanCurvePoint{45, 18},
		FanCurvePoint{50, 18},
		FanCurvePoint{55, 18},
		FanCurvePoint{60, 25},
		FanCurvePoint{65, 30},
		FanCurvePoint{70, 35},
		FanCurvePoint{75, 55},
		FanCurvePoint{80, 70},
		FanCurvePoint{85, 80},
		FanCurvePoint{90, 95}
	};
	const FanCurvePoints actual = MakeDefaultFanCurve();
	std::string error;

	Expect(SamePoints(actual, expected), "default fan curve differs from legacy values");
	Expect(ValidateFanCurve(actual, &error), "default fan curve should validate");
	Expect(error.empty(), "successful validation should clear the error");
}

void TestValidationBoundaries()
{
	std::string error;
	Expect(!ValidateFanCurve(FanCurvePoints{}, &error), "empty curves should fail validation");
	Expect(!error.empty(), "empty curves should report an error");
	Expect(ValidateFanCurve(FanCurvePoints{{30, 0}, {100, 100}}, &error), "range boundary points should validate");

	Expect(!ValidateFanCurve(FanCurvePoints{{30, 0}}, &error), "curves with too few points should fail");
	Expect(!error.empty(), "too few points should report an error");

	FanCurvePoints tooMany;
	for (int i = 0; i < 17; ++i)
	{
		tooMany.push_back(FanCurvePoint{30 + i, 0});
	}
	Expect(!ValidateFanCurve(tooMany, &error), "curves with too many points should fail");

	Expect(!ValidateFanCurve(FanCurvePoints{{29, 0}, {40, 20}}, &error), "temperature below minimum should fail");
	Expect(!ValidateFanCurve(FanCurvePoints{{40, 20}, {101, 30}}, &error), "temperature above maximum should fail");
	Expect(!ValidateFanCurve(FanCurvePoints{{40, -1}, {60, 30}}, &error), "duty below minimum should fail");
	Expect(!ValidateFanCurve(FanCurvePoints{{40, 20}, {60, 101}}, &error), "duty above maximum should fail");
}

void TestInvalidEvaluationAndNullArguments()
{
	const FanCurvePoints invalidExtreme{
		FanCurvePoint{std::numeric_limits<int>::min(), std::numeric_limits<int>::min()},
		FanCurvePoint{std::numeric_limits<int>::max(), std::numeric_limits<int>::max()}
	};
	std::string error;

	Expect(!ValidateFanCurve(invalidExtreme, &error), "extreme invalid curves should fail validation");
	Expect(!ValidateFanCurve(invalidExtreme, nullptr), "validation should safely accept a null error pointer");
	Expect(EvaluateLinearDuty(invalidExtreme, 0) == 0, "linear evaluation should reject invalid extreme curves");

	int selectedIndex = 7;
	Expect(EvaluateStepDuty(invalidExtreme, 0, 3, &selectedIndex) == 0, "step evaluation should reject invalid extreme curves");
	Expect(selectedIndex == 0, "invalid step evaluation should reset the selected index");

	FanCurvePoints points{{40, 10}, {60, 30}};
	Expect(ValidateFanCurve(points, nullptr), "valid curves should accept a null error pointer");
	Expect(TrySetFanCurvePoint(&points, 0, 41, 11, nullptr), "point editing should accept a null error pointer");
	Expect(TryInsertFanCurvePoint(&points, 50, 20, nullptr), "point insertion should accept a null error pointer");
	Expect(TryDeleteFanCurvePoint(&points, 1, nullptr), "point deletion should accept a null error pointer");

	Expect(!TrySetFanCurvePoint(nullptr, 0, 40, 10, nullptr), "point editing should reject a null curve pointer");
	Expect(!TryInsertFanCurvePoint(nullptr, 40, 10, nullptr), "point insertion should reject a null curve pointer");
	Expect(!TryDeleteFanCurvePoint(nullptr, 0, nullptr), "point deletion should reject a null curve pointer");
}

void TestStrictOrderingAndAtomicSet()
{
	std::string error;
	Expect(!ValidateFanCurve(FanCurvePoints{{40, 10}, {40, 20}}, &error), "duplicate temperatures should fail validation");
	Expect(!ValidateFanCurve(FanCurvePoints{{60, 20}, {40, 10}}, &error), "descending temperatures should fail validation");

	FanCurvePoints points{{40, 10}, {60, 30}, {80, 50}};
	FanCurvePoints before = points;
	Expect(TrySetFanCurvePoint(&points, 1, 61, 31, &error), "valid point update should succeed");
	Expect(points[1].temperature == 61 && points[1].duty == 31, "valid point update should be applied");

	before = points;
	Expect(!TrySetFanCurvePoint(&points, 1, 39, 40, &error), "point update cannot cross the previous point");
	Expect(SamePoints(points, before), "failed point update must roll back");
	Expect(!TrySetFanCurvePoint(&points, 1, 81, 40, &error), "point update cannot cross the next point");
	Expect(SamePoints(points, before), "failed point update must preserve the vector");
	Expect(!TrySetFanCurvePoint(&points, 1, 61, 101, &error), "point update with invalid duty should fail");
	Expect(SamePoints(points, before), "invalid point update must preserve the vector");
	Expect(!TrySetFanCurvePoint(&points, 99, 61, 40, &error), "out-of-range point index should fail");
	Expect(SamePoints(points, before), "out-of-range point update must preserve the vector");
}

void TestLinearEvaluation()
{
	const FanCurvePoints empty;
	Expect(EvaluateLinearDuty(empty, 50) == 0, "linear evaluation should safely handle an empty curve");

	const FanCurvePoints points{{30, 0}, {34, 1}, {50, 100}};
	Expect(EvaluateLinearDuty(points, 20) == 0, "linear evaluation should clamp below the first point");
	Expect(EvaluateLinearDuty(points, 30) == 0, "linear evaluation should use the first endpoint");
	Expect(EvaluateLinearDuty(points, 32) == 1, "linear evaluation should round half up");
	Expect(EvaluateLinearDuty(points, 42) == 51, "linear evaluation should interpolate within an interval");
	Expect(EvaluateLinearDuty(points, 50) == 100, "linear evaluation should use the last endpoint");
	Expect(EvaluateLinearDuty(points, 60) == 100, "linear evaluation should clamp above the last point");

	const FanCurvePoints descendingDuty{{30, 100}, {40, 0}};
	Expect(EvaluateLinearDuty(descendingDuty, 35) == 50, "linear evaluation should support descending duty values");
}

void TestStepEvaluation()
{
	const FanCurvePoints empty;
	int emptySelectedIndex = 5;
	Expect(EvaluateStepDuty(empty, 50, 3, &emptySelectedIndex) == 0, "step evaluation should safely handle an empty curve");
	Expect(emptySelectedIndex == 0, "empty step evaluation should reset the selected index");

	const FanCurvePoints points{{40, 10}, {50, 20}, {60, 30}, {70, 40}};
	int selectedIndex = -1;

	Expect(EvaluateStepDuty(points, 45, 3, &selectedIndex) == 10, "invalid selected index should restart at point zero");
	Expect(selectedIndex == 0, "temperature below the next point should keep point zero selected");
	Expect(EvaluateStepDuty(points, 69, 3, &selectedIndex) == 30, "rising temperature should cross all reached points");
	Expect(selectedIndex == 2, "rising temperature should select the last reached point");
	Expect(EvaluateStepDuty(points, 70, 3, &selectedIndex) == 40, "a node should activate at its exact temperature");
	Expect(selectedIndex == 3, "the final point should be selected at its threshold");

	selectedIndex = 3;
	Expect(EvaluateStepDuty(points, 67, 3, &selectedIndex) == 40, "hysteresis should hold at the exact lower threshold");
	Expect(selectedIndex == 3, "selected index should hold at the hysteresis threshold");
	Expect(EvaluateStepDuty(points, 66, 3, &selectedIndex) == 30, "temperature below the hysteresis threshold should lower one level");
	Expect(selectedIndex == 2, "hysteresis should select the preceding point");
	Expect(EvaluateStepDuty(points, 56, 3, &selectedIndex) == 20, "falling temperature should cross all released points");
	Expect(selectedIndex == 1, "falling temperature should stop at the first unreleased point");
	Expect(EvaluateStepDuty(points, 46, 3, &selectedIndex) == 10, "falling below another threshold should lower again");
	Expect(selectedIndex == 0, "hysteresis should eventually reach point zero");

	selectedIndex = 3;
	Expect(EvaluateStepDuty(points, 69, 0, &selectedIndex) == 30, "zero transition temperature should allow immediate lowering");
	Expect(selectedIndex == 2, "zero transition temperature should lower at the next degree");

	int firstSelectedIndex = -1;
	int secondSelectedIndex = -1;
	Expect(EvaluateStepDuty(points, 69, 3, &firstSelectedIndex) == 30, "first step state should advance independently");
	Expect(firstSelectedIndex == 2, "first step state should retain its own selected index");
	Expect(EvaluateStepDuty(points, 45, 3, &secondSelectedIndex) == 10, "second step state should start independently");
	Expect(secondSelectedIndex == 0, "second step state should retain its own selected index");
	Expect(EvaluateStepDuty(points, 70, 3, &firstSelectedIndex) == 40, "first step state should continue independently");
	Expect(firstSelectedIndex == 3, "first step state should not be affected by the second state");
	Expect(EvaluateStepDuty(points, 49, 3, &secondSelectedIndex) == 10, "second step state should remain independent");
	Expect(secondSelectedIndex == 0, "second step state should not be affected by the first state");
}

void TestInsertionDeletionAndLimits()
{
	std::string error;
	FanCurvePoints singlePoint{{50, 20}};
	FanCurvePoints singlePointBefore = singlePoint;
	Expect(!TryInsertFanCurvePoint(&singlePoint, 70, 40, &error), "insertion into a curve below the minimum count should fail");
	Expect(SamePoints(singlePoint, singlePointBefore), "minimum-count insertion failure must roll back");

	FanCurvePoints points{{40, 10}, {60, 30}};

	Expect(TryInsertFanCurvePoint(&points, 50, 20, &error), "insertion between points should succeed");
	Expect(SamePoints(points, FanCurvePoints{{40, 10}, {50, 20}, {60, 30}}), "inserted points should remain sorted");

	FanCurvePoints before = points;
	Expect(!TryInsertFanCurvePoint(&points, 50, 25, &error), "duplicate temperature insertion should fail");
	Expect(SamePoints(points, before), "duplicate insertion must roll back");
	Expect(!TryInsertFanCurvePoint(&points, 29, 25, &error), "out-of-range insertion should fail");
	Expect(SamePoints(points, before), "invalid insertion must roll back");
	Expect(!TryInsertFanCurvePoint(&points, 55, 101, &error), "insertion with duty above maximum should fail");
	Expect(SamePoints(points, before), "out-of-range duty insertion must roll back");
	Expect(!TryInsertFanCurvePoint(&points, 55, -1, &error), "insertion with duty below minimum should fail");
	Expect(SamePoints(points, before), "negative duty insertion must roll back");

	FanCurvePoints invalid{{60, 30}, {40, 10}};
	FanCurvePoints invalidBefore = invalid;
	Expect(!TryInsertFanCurvePoint(&invalid, 50, 20, &error), "insertion into a descending curve should fail before searching");
	Expect(SamePoints(invalid, invalidBefore), "insertion into an invalid curve must roll back");

	Expect(TryDeleteFanCurvePoint(&points, 1, &error), "deleting a middle point should succeed");
	Expect(SamePoints(points, FanCurvePoints{{40, 10}, {60, 30}}), "deletion should preserve remaining order");
	before = points;
	Expect(!TryDeleteFanCurvePoint(&points, 0, &error), "deleting below the minimum point count should fail");
	Expect(SamePoints(points, before), "minimum-count deletion failure must roll back");
	Expect(!TryDeleteFanCurvePoint(&points, 99, &error), "out-of-range deletion should fail");
	Expect(SamePoints(points, before), "invalid deletion must preserve the vector");

	FanCurvePoints maximum{{30, 0}, {31, 1}};
	for (int temperature = 32; temperature <= 45; ++temperature)
	{
		Expect(TryInsertFanCurvePoint(&maximum, temperature, temperature - 30, &error), "insertion below the maximum count should succeed");
	}
	Expect(maximum.size() == FAN_CURVE_MAX_POINTS, "the model should allow exactly 16 points");
	before = maximum;
	Expect(!TryInsertFanCurvePoint(&maximum, 46, 16, &error), "the seventeenth point should be rejected");
	Expect(SamePoints(maximum, before), "maximum-count insertion failure must roll back");
}

void ExpectInvalidPresetFile(const std::string& path, const std::string& text, const char* message)
{
	WriteText(path, text);
	const std::string beforeLoad = ReadText(path);
	PresetCollection output;
	output.autoSwitch = true;
	output.presets.push_back(MakePreset("stale", "stale.exe", 0));
	std::string diagnostic = "stale";
	Expect(PresetStore::Load(path, &output, &diagnostic) == PresetLoadStatus::Invalid, message);
	PresetCollection defaults;
	Expect(output.autoSwitch == defaults.autoSwitch && output.presets.empty(), "invalid preset should load defaults");
	Expect(diagnostic.find(path) != std::string::npos, "invalid preset diagnostic should include path");
	Expect(diagnostic.size() > path.size(), "invalid preset diagnostic should include a reason");
	Expect(ReadText(path) == beforeLoad, "invalid preset load must not modify the source file");
}

void TestPresetMatching()
{
	Expect(WildcardMatch("", ""), "an empty pattern should match an empty value");
	Expect(!WildcardMatch("", "game.exe"), "an empty pattern should not match a non-empty value");
	Expect(WildcardMatch("*", ""), "a star should match an empty value");
	Expect(WildcardMatch("*", "game.exe"), "a star should match a non-empty value");
	Expect(WildcardMatch("*ab", "*cab"), "a leading star must be treated as wildcard before literal comparison");
	Expect(WildcardMatch("**game**.exe", "mygame_dx12.exe"), "consecutive stars should match any sequences");
	Expect(WildcardMatch("?game.exe", "xgame.exe"), "a question mark at the start should match one character");
	Expect(!WildcardMatch("?game.exe", "game.exe"), "a leading question mark must consume one character");
	Expect(WildcardMatch("game.ex?", "game.exe"), "a question mark at the end should match one character");
	Expect(!WildcardMatch("game.ex?", "game.executable"), "a trailing question mark must match one character");
	Expect(WildcardMatch("game.exe", "GAME.EXE"), "matching must ignore case");
	Expect(WildcardMatch("game*.exe", "game_dx12.exe"), "star wildcard should match a suffix");
	Expect(WildcardMatch("game?.exe", "game1.exe"), "question wildcard should match one character");
	Expect(!WildcardMatch("game?.exe", "game12.exe"), "question wildcard must match exactly one character");
	Expect(!WildcardMatch("game.exe", "other.exe"), "nonmatching names must be rejected");
	Expect(!WildcardMatch("game.exe", "game.exe.bak"), "ordinary suffixes must not be ignored");

	PresetCollection collection;
	collection.presets.push_back(MakePreset("Broad", "game*.exe", 1));
	collection.presets.push_back(MakePreset("Specific", "game_dx12.exe", 2));
	std::vector<std::string> running;
	running.push_back("game_dx12.exe");
	int selected = -1;
	Expect(FindMatchingPresetIndex(collection, running, &selected), "a running matching process should select a preset");
	Expect(selected == 0, "the first matching rule must win");
	running.clear();
	running.push_back("notepad.exe");
	selected = 99;
	Expect(!FindMatchingPresetIndex(collection, running, &selected), "no matching process should report no preset");
	Expect(selected == -1, "no match must reset the output index");
}

void TestPresetValidationAndControlIsolation()
{
	PresetCollection collection;
	collection.autoSwitch = true;
	collection.presets.push_back(MakePreset("Gaming", "game*.exe", 0));
	collection.presets.push_back(MakePreset("Rendering", "render.exe", 3));
	std::string error;
	Expect(collection.Validate(&error), "valid preset collection should validate");

	PresetCollection duplicate = collection;
	duplicate.presets[1].name = duplicate.presets[0].name;
	Expect(!duplicate.Validate(&error), "duplicate preset names must fail validation");

	PresetCollection emptyName = collection;
	emptyName.presets[0].name.clear();
	Expect(!emptyName.Validate(&error), "empty preset names must fail validation");

	PresetCollection emptyPattern = collection;
	emptyPattern.presets[0].processPattern.clear();
	Expect(!emptyPattern.Validate(&error), "empty process patterns must fail validation");

	PresetCollection badPattern = collection;
	badPattern.presets[0].processPattern = "C:\\game.exe";
	Expect(!badPattern.Validate(&error), "process paths must be rejected");

	PresetCollection badSyntax = collection;
	badSyntax.presets[0].processPattern = "game[0].exe";
	Expect(!badSyntax.Validate(&error), "unsupported wildcard syntax must be rejected");

	PresetCollection badControl = collection;
	badControl.presets[0].processPattern = std::string("game") + "\x01" + ".exe";
	Expect(!badControl.Validate(&error), "control characters in process patterns must be rejected");

	PresetCollection badNul = collection;
	badNul.presets[0].processPattern = std::string("game\0.exe", 9);
	Expect(!badNul.Validate(&error), "embedded NUL characters in process patterns must be rejected");

	PresetCollection name64 = collection;
	name64.presets[0].name = std::string(63, 'n') + "A";
	Expect(name64.Validate(&error), "64-byte preset names should be accepted");
	PresetCollection name65 = collection;
	name65.presets[0].name = std::string(64, 'n') + "A";
	Expect(!name65.Validate(&error), "65-byte preset names must be rejected");

	PresetCollection pattern128 = collection;
	pattern128.presets[0].processPattern = std::string(127, 'p') + "A";
	Expect(pattern128.Validate(&error), "128-byte process patterns should be accepted");
	PresetCollection pattern129 = collection;
	pattern129.presets[0].processPattern = std::string(128, 'p') + "A";
	Expect(!pattern129.Validate(&error), "129-byte process patterns must be rejected");

	PresetCollection exactly32;
	exactly32.autoSwitch = true;
	for (int i = 0; i < 32; ++i)
	{
		const std::string name = std::string("Preset") + std::to_string(i);
		const std::string pattern = std::string("application") + std::to_string(i) + ".exe";
		exactly32.presets.push_back(MakePreset(name.c_str(), pattern.c_str(), 0));
	}
	Expect(exactly32.Validate(&error), "32 presets should be accepted");
	PresetCollection tooMany = exactly32;
	tooMany.presets.push_back(MakePreset("Preset32", "application32.exe", 0));
	Expect(!tooMany.Validate(&error), "more than 32 presets must fail validation");

	PresetCollection invalidCurve = collection;
	invalidCurve.presets[0].config.CpuCurve[0].duty = 101;
	Expect(!invalidCurve.Validate(&error), "invalid preset curves must fail validation");

	FanConfig target;
	target.LoadDefault();
	target.UiFontSize = 8;
	target.AutoRun = true;
	target.CloseToTray = true;
	CopyControlSettings(collection.presets[0].config, &target);
	Expect(SameControlConfig(target, collection.presets[0].config), "control fields must be copied");
	Expect(target.UiFontSize == 8 && target.AutoRun && target.CloseToTray,
		"copying preset control fields must preserve global UI fields");
}

void TestPresetStoreRoundTrip()
{
	TempDirectory directory;
	const std::string path = directory.File("presets.json");
	PresetCollection expected;
	expected.autoSwitch = true;
	expected.presets.push_back(MakePreset("Gaming", "game*.exe", 0));
	expected.presets.push_back(MakePreset("Render", "render?.exe", 3));
	std::string diagnostic = "stale";
	Expect(PresetStore::Save(path, expected, &diagnostic), "valid preset collection should save");
	Expect(diagnostic.empty(), "successful preset save should clear diagnostics");
	const std::string valid = ReadText(path);
	Expect(!valid.empty() && valid[0] == '{', "preset collection should be written as JSON");
	Expect(valid.find("\"presets\"") != std::string::npos, "JSON preset file should contain presets");
	Expect(valid.find("uiFontSize") == std::string::npos && valid.find("autoRun") == std::string::npos,
		"preset JSON must not persist global-only fields");

	PresetCollection actual;
	Expect(PresetStore::Load(path, &actual, &diagnostic) == PresetLoadStatus::Loaded,
		"saved preset collection should load");
	Expect(actual.autoSwitch == expected.autoSwitch && actual.presets.size() == expected.presets.size(),
		"preset metadata should round-trip");
	for (size_t i = 0; i < expected.presets.size(); ++i)
	{
		Expect(actual.presets[i].name == expected.presets[i].name &&
			actual.presets[i].processPattern == expected.presets[i].processPattern &&
			SameControlConfig(actual.presets[i].config, expected.presets[i].config),
			"preset order and control fields should round-trip");
		Expect(actual.presets[i].config.UiFontSize == FAN_UI_FONT_SIZE_DEFAULT &&
			!actual.presets[i].config.AutoRun && !actual.presets[i].config.CloseToTray,
			"preset load must restore defaults for global-only fields");
	}

	ExpectInvalidPresetFile(directory.File("malformed.json"), "{", "malformed preset JSON should be rejected");
	ExpectInvalidPresetFile(directory.File("unknown-field.json"),
		"{\n  \"version\": 1," + valid.substr(1), "unknown preset JSON fields should be rejected");
	ExpectInvalidPresetFile(directory.File("wrong-type.json"),
		ReplaceFirst(valid, "\"autoSwitch\": true", "\"autoSwitch\": 1"),
		"wrong preset JSON field types should be rejected");
	ExpectInvalidPresetFile(directory.File("invalid-content.json"),
		ReplaceFirst(valid, "\"forceTemp\": 70", "\"forceTemp\": 101"),
		"invalid preset content should be rejected");
	ExpectInvalidPresetFile(directory.File("old-binary.cfg"), "MFS1 old preset configuration",
		"old binary preset configuration should be rejected");
	ExpectInvalidPresetFile(directory.File("trailing.json"), valid + "{}", "trailing preset JSON should be rejected");

	const std::string overwritePath = directory.File("overwrite-invalid.cfg");
	WriteText(overwritePath, "MFS1 old preset configuration");
	PresetCollection overwritten;
	Expect(PresetStore::Load(overwritePath, &overwritten, &diagnostic) == PresetLoadStatus::Invalid,
		"invalid preset should be rejected before overwrite");
	Expect(PresetStore::Save(overwritePath, expected, &diagnostic), "invalid preset file should be overwritten");
	Expect(PresetStore::Load(overwritePath, &overwritten, &diagnostic) == PresetLoadStatus::Loaded,
		"overwritten preset file should load");
	Expect(overwritten.autoSwitch && overwritten.presets.size() == expected.presets.size(),
		"overwritten preset file should contain the new collection");
}

void TestSingleInstanceGuard()
{
	const std::wstring name = L"Global\\ClevoFanControl.Tests." +
		std::to_wstring(static_cast<unsigned long>(GetCurrentProcessId()));
	{
		SingleInstanceGuard first;
		SingleInstanceGuard second;
		Expect(first.Acquire(name.c_str()) == SingleInstanceStatus::Acquired,
			"the first single-instance guard should acquire its mutex");
		Expect(first.Acquire((name + L".again").c_str()) == SingleInstanceStatus::Unavailable,
			"a guard should only acquire a mutex once");
		Expect(first.ErrorCode() == ERROR_INVALID_STATE,
			"repeated acquisition should preserve ERROR_INVALID_STATE");
		Expect(second.Acquire(name.c_str()) == SingleInstanceStatus::AlreadyRunning,
			"a second guard should detect the existing mutex");
		Expect(second.ErrorCode() == ERROR_ALREADY_EXISTS,
			"duplicate acquisition should preserve ERROR_ALREADY_EXISTS");
	}

	SingleInstanceGuard third;
	Expect(third.Acquire(name.c_str()) == SingleInstanceStatus::Acquired,
		"a destroyed guard should release its mutex");
}

void TestTaskXmlSerialization()
{
	std::string xml;
	std::string diagnostic;
	const std::wstring target = L"C:\\R&D\\\u6e05\"Fan'Control.exe";
	Expect(BuildTaskXmlUtf8(target, &xml, &diagnostic),
		"task XML should serialize a Unicode target path");
	Expect(diagnostic.empty(), "successful task XML serialization should clear diagnostics");
	Expect(xml.find("encoding=\"UTF-8\"") != std::string::npos,
		"task XML should declare UTF-8");
	Expect(xml.find("UTF-16") == std::string::npos,
		"task XML should not declare UTF-16");
	Expect(xml.find("R&amp;D") != std::string::npos,
		"task XML should escape ampersands");
	Expect(xml.find("&quot;Fan&apos;Control.exe") != std::string::npos,
		"task XML should escape quotes and apostrophes");
	Expect(xml.find("\xe6\xb8\x85") != std::string::npos,
		"task XML should encode non-ASCII text as UTF-8");

	Expect(BuildTaskXmlUtf8(L"C:\\<Fan>.exe", &xml, &diagnostic),
		"task XML should serialize angle brackets in a target path");
	Expect(xml.find("&lt;Fan&gt;") != std::string::npos,
		"task XML should escape less-than and greater-than characters");
	Expect(!BuildTaskXmlUtf8(target, nullptr, &diagnostic),
		"task XML should reject a null output");
	Expect(!diagnostic.empty(), "null task XML output should provide a diagnostic");

	xml = "unchanged";
	const std::wstring invalidUnicode(1, static_cast<wchar_t>(0xd800));
	Expect(!BuildTaskXmlUtf8(invalidUnicode, &xml, &diagnostic),
		"task XML should reject invalid Unicode");
	Expect(!diagnostic.empty(), "invalid Unicode should provide a diagnostic");
	Expect(xml == "unchanged", "failed task XML serialization should not replace output");

	auto expectInvalidXmlCharacter = [](const std::wstring& invalidTarget, const char* message)
	{
		std::string invalidOutput = "unchanged";
		std::string invalidDiagnostic;
		Expect(!BuildTaskXmlUtf8(invalidTarget, &invalidOutput, &invalidDiagnostic), message);
		Expect(!invalidDiagnostic.empty(), "invalid XML character should provide a diagnostic");
		Expect(invalidOutput == "unchanged", "invalid XML character should not replace output");
	};

	std::wstring embeddedNull = L"C:\\Fan";
	embeddedNull.push_back(L'\0');
	embeddedNull.append(L"Control.exe");
	expectInvalidXmlCharacter(embeddedNull, "task XML should reject an embedded NUL");

	std::wstring forbiddenControl = L"C:\\Fan";
	forbiddenControl.push_back(static_cast<wchar_t>(0x0001));
	forbiddenControl.append(L"Control.exe");
	expectInvalidXmlCharacter(forbiddenControl, "task XML should reject forbidden C0 controls");
	expectInvalidXmlCharacter(std::wstring(1, static_cast<wchar_t>(0xfffe)),
		"task XML should reject U+FFFE");
	expectInvalidXmlCharacter(std::wstring(1, static_cast<wchar_t>(0xffff)),
		"task XML should reject U+FFFF");
	expectInvalidXmlCharacter(std::wstring(1, static_cast<wchar_t>(0xdc00)),
		"task XML should reject an isolated low surrogate");

	std::wstring supplementaryTarget = L"C:\\Fan";
	supplementaryTarget.push_back(static_cast<wchar_t>(0xd83d));
	supplementaryTarget.push_back(static_cast<wchar_t>(0xde00));
	supplementaryTarget.append(L"Control.exe");
	Expect(BuildTaskXmlUtf8(supplementaryTarget, &xml, &diagnostic),
		"task XML should accept a valid supplementary-plane character");
	Expect(diagnostic.empty(), "supplementary-plane serialization should clear diagnostics");
	Expect(xml.find("\xf0\x9f\x98\x80") != std::string::npos,
		"task XML should encode a supplementary-plane character as UTF-8");
}
}

int main()
{
	try
	{
		TestDefaultCurve();
		TestValidationBoundaries();
		TestInvalidEvaluationAndNullArguments();
		TestStrictOrderingAndAtomicSet();
		TestLinearEvaluation();
		TestStepEvaluation();
		TestInsertionDeletionAndLimits();
		TestFanConfigDefaultsAndValidation();
		TestConfigRoundTrip();
		TestMissingIoAndVersionedName();
		TestInvalidConfigFiles();
		TestInvalidConfigIsOverwrittenBySave();
		TestFailedReplacementPreservesSentinel();
		TestPresetMatching();
		TestPresetValidationAndControlIsolation();
		TestPresetStoreRoundTrip();
		TestSingleInstanceGuard();
		TestTaskXmlSerialization();
		std::cout << "FanCurveModelTests: PASS\n";
		return 0;
	}
	catch (const std::exception& exception)
	{
		std::cerr << "FanCurveModelTests: FAIL: " << exception.what() << "\n";
		return 1;
	}
}
