#include "ConfigStore.h"
#include "JsonValue.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <atomic>
#include <fstream>
#include <limits>
#include <sstream>

namespace
{
const uint64_t kMaxJsonFileSize = 64ULL * 1024ULL;
std::atomic<unsigned long> g_tempSequence(0);

void ClearDiagnostic(std::string* diagnostic)
{
	if (diagnostic != nullptr) diagnostic->clear();
}

void SetDiagnostic(std::string* diagnostic, const std::string& path, const std::string& reason)
{
	if (diagnostic != nullptr) *diagnostic = "ConfigStore [" + path + "]: " + reason;
}

std::string WindowsError(const char* operation, DWORD errorCode)
{
	std::ostringstream message;
	message << operation << " (Windows error " << static_cast<unsigned long>(errorCode) << ")";
	return message.str();
}

enum class ReadStatus
{
	Loaded,
	Missing,
	Invalid,
	IoError
};

ReadStatus ReadJsonText(const std::string& path, std::string* text, std::string* diagnostic)
{
	const DWORD attributes = GetFileAttributesA(path.c_str());
	if (attributes == INVALID_FILE_ATTRIBUTES)
	{
		const DWORD errorCode = GetLastError();
		if (errorCode == ERROR_FILE_NOT_FOUND || errorCode == ERROR_PATH_NOT_FOUND)
		{
			SetDiagnostic(diagnostic, path, "file is missing");
			return ReadStatus::Missing;
		}
		SetDiagnostic(diagnostic, path, WindowsError("cannot inspect file", errorCode));
		return ReadStatus::IoError;
	}
	if ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0U)
	{
		SetDiagnostic(diagnostic, path, "path is a directory");
		return ReadStatus::IoError;
	}

	std::ifstream stream(path.c_str(), std::ios::in | std::ios::binary | std::ios::ate);
	if (!stream.is_open())
	{
		SetDiagnostic(diagnostic, path, "cannot open file for reading");
		return ReadStatus::IoError;
	}
	const std::streampos end = stream.tellg();
	if (end == std::streampos(-1))
	{
		SetDiagnostic(diagnostic, path, "cannot determine file length");
		return ReadStatus::IoError;
	}
	const std::streamoff length = static_cast<std::streamoff>(end);
	if (length < 0 || static_cast<uint64_t>(length) > kMaxJsonFileSize ||
		static_cast<uint64_t>(length) > static_cast<uint64_t>(std::numeric_limits<size_t>::max()))
	{
		SetDiagnostic(diagnostic, path, "JSON file exceeds the 64 KiB limit");
		return ReadStatus::Invalid;
	}

	text->assign(static_cast<size_t>(length), '\0');
	stream.seekg(0, std::ios::beg);
	if (!stream)
	{
		SetDiagnostic(diagnostic, path, "cannot seek to the start of the file");
		return ReadStatus::IoError;
	}
	if (length > 0)
	{
		stream.read(&(*text)[0], length);
	}
	if (stream.gcount() != length || !stream)
	{
		SetDiagnostic(diagnostic, path, "cannot read the complete file");
		return ReadStatus::IoError;
	}
	stream.close();
	if (stream.fail())
	{
		SetDiagnostic(diagnostic, path, "cannot close the file after reading");
		return ReadStatus::IoError;
	}
	return ReadStatus::Loaded;
}

bool HasExactFields(const JsonValue& value, const char* const* names, size_t count,
	const JsonValue::Object** object, std::string* reason)
{
	const JsonValue::Object* candidate = value.GetObjectValue();
	if (candidate == nullptr)
	{
		if (reason != nullptr) *reason = "expected a JSON object";
		return false;
	}
	if (candidate->size() != count)
	{
		if (reason != nullptr) *reason = "JSON object contains missing or unknown fields";
		return false;
	}
	for (JsonValue::Object::const_iterator it = candidate->begin(); it != candidate->end(); ++it)
	{
		bool known = false;
		for (size_t i = 0; i < count; ++i)
		{
			if (it->first == names[i])
			{
				known = true;
				break;
			}
		}
		if (!known)
		{
			if (reason != nullptr) *reason = "JSON object contains unknown field '" + it->first + "'";
			return false;
		}
	}
	*object = candidate;
	return true;
}

bool GetField(const JsonValue::Object& object, const char* name, const JsonValue** value, std::string* reason)
{
	JsonValue::Object::const_iterator it = object.find(name);
	if (it == object.end())
	{
		if (reason != nullptr) *reason = std::string("missing JSON field '") + name + "'";
		return false;
	}
	*value = &it->second;
	return true;
}

bool ReadBoolean(const JsonValue::Object& object, const char* name, bool* output, std::string* reason)
{
	const JsonValue* value = nullptr;
	if (!GetField(object, name, &value, reason) || !value->GetBoolean(output))
	{
		if (reason != nullptr && reason->empty()) *reason = std::string("JSON field '") + name + "' must be boolean";
		return false;
	}
	return true;
}

bool ReadInteger(const JsonValue::Object& object, const char* name, int* output, std::string* reason)
{
	const JsonValue* value = nullptr;
	if (!GetField(object, name, &value, reason) || !value->GetInteger(output))
	{
		if (reason != nullptr && reason->empty()) *reason = std::string("JSON field '") + name + "' must be an integer";
		return false;
	}
	return true;
}

bool ReadCurve(const JsonValue& value, const char* name, FanCurvePoints* output, std::string* reason)
{
	const JsonValue::Array* array = value.GetArray();
	if (array == nullptr)
	{
		if (reason != nullptr) *reason = std::string("JSON field '") + name + "' must be an array";
		return false;
	}
	if (array->size() < FAN_CURVE_MIN_POINTS || array->size() > FAN_CURVE_MAX_POINTS)
	{
		if (reason != nullptr) *reason = std::string(name) + " curve must contain between 2 and 16 points";
		return false;
	}
	FanCurvePoints candidate;
	candidate.reserve(array->size());
	static const char* const pointFields[] = {"temperature", "duty"};
	for (size_t i = 0; i < array->size(); ++i)
	{
		const JsonValue::Object* point = nullptr;
		if (!HasExactFields((*array)[i], pointFields, 2U, &point, reason)) return false;
		int temperature = 0;
		int duty = 0;
		if (!ReadInteger(*point, "temperature", &temperature, reason) ||
			!ReadInteger(*point, "duty", &duty, reason)) return false;
		candidate.push_back(FanCurvePoint{temperature, duty});
	}
	*output = candidate;
	return true;
}

bool ParseConfig(const std::string& text, FanConfig* output, std::string* reason)
{
	JsonValue root;
	std::string parseError;
	if (!JsonValue::Parse(text, &root, &parseError))
	{
		if (reason != nullptr) *reason = "invalid JSON: " + parseError;
		return false;
	}
	static const char* const fields[] = {
		"updateInterval", "transitionTemp", "forceTemp", "fontSize", "linear", "takeOver",
		"forceCooling", "softControl", "autoRun", "closeToTray", "cpuCurve", "gpuCurve"
	};
	const JsonValue::Object* object = nullptr;
	if (!HasExactFields(root, fields, sizeof(fields) / sizeof(fields[0]), &object, reason)) return false;

	FanConfig candidate;
	if (!ReadInteger(*object, "updateInterval", &candidate.UpdateInterval, reason) ||
		!ReadInteger(*object, "transitionTemp", &candidate.TransitionTemp, reason) ||
		!ReadInteger(*object, "forceTemp", &candidate.ForceTemp, reason) ||
		!ReadInteger(*object, "fontSize", &candidate.UiFontSize, reason) ||
		!ReadBoolean(*object, "linear", &candidate.Linear, reason) ||
		!ReadBoolean(*object, "takeOver", &candidate.TakeOver, reason) ||
		!ReadBoolean(*object, "forceCooling", &candidate.ForceCooling, reason) ||
		!ReadBoolean(*object, "softControl", &candidate.SoftControl, reason) ||
		!ReadBoolean(*object, "autoRun", &candidate.AutoRun, reason) ||
		!ReadBoolean(*object, "closeToTray", &candidate.CloseToTray, reason)) return false;
	const JsonValue* cpu = nullptr;
	const JsonValue* gpu = nullptr;
	if (!GetField(*object, "cpuCurve", &cpu, reason) || !GetField(*object, "gpuCurve", &gpu, reason) ||
		!ReadCurve(*cpu, "cpuCurve", &candidate.CpuCurve, reason) ||
		!ReadCurve(*gpu, "gpuCurve", &candidate.GpuCurve, reason)) return false;
	std::string validationError;
	if (!candidate.Validate(&validationError))
	{
		if (reason != nullptr) *reason = "configuration contains invalid fields: " + validationError;
		return false;
	}
	*output = candidate;
	return true;
}

JsonValue Integer(int value)
{
	return JsonValue(static_cast<int64_t>(value));
}

JsonValue BuildCurve(const FanCurvePoints& curve)
{
	JsonValue array = JsonValue::MakeArray();
	for (size_t i = 0; i < curve.size(); ++i)
	{
		JsonValue point = JsonValue::MakeObject();
		point.Set("temperature", Integer(curve[i].temperature));
		point.Set("duty", Integer(curve[i].duty));
		array.Append(point);
	}
	return array;
}

JsonValue BuildConfig(const FanConfig& config)
{
	JsonValue root = JsonValue::MakeObject();
	root.Set("updateInterval", Integer(config.UpdateInterval));
	root.Set("transitionTemp", Integer(config.TransitionTemp));
	root.Set("forceTemp", Integer(config.ForceTemp));
	root.Set("fontSize", Integer(config.UiFontSize));
	root.Set("linear", JsonValue(config.Linear));
	root.Set("takeOver", JsonValue(config.TakeOver));
	root.Set("forceCooling", JsonValue(config.ForceCooling));
	root.Set("softControl", JsonValue(config.SoftControl));
	root.Set("autoRun", JsonValue(config.AutoRun));
	root.Set("closeToTray", JsonValue(config.CloseToTray));
	root.Set("cpuCurve", BuildCurve(config.CpuCurve));
	root.Set("gpuCurve", BuildCurve(config.GpuCurve));
	return root;
}

std::string MakeTempPath(const std::string& path)
{
	const unsigned long sequence = g_tempSequence.fetch_add(1, std::memory_order_relaxed);
	std::ostringstream suffix;
	suffix << path << ".tmp." << static_cast<unsigned long>(GetCurrentProcessId()) << "."
		<< static_cast<unsigned long long>(GetTickCount64()) << "." << sequence;
	return suffix.str();
}

bool SaveTextAtomically(const std::string& path, const std::string& text, std::string* diagnostic)
{
	const std::string temporaryPath = MakeTempPath(path);
	std::ofstream stream(temporaryPath.c_str(), std::ios::out | std::ios::binary | std::ios::trunc);
	if (!stream.is_open())
	{
		DeleteFileA(temporaryPath.c_str());
		SetDiagnostic(diagnostic, path, "cannot create temporary file");
		return false;
	}
	stream.write(text.data(), static_cast<std::streamsize>(text.size()));
	if (!stream)
	{
		stream.close();
		DeleteFileA(temporaryPath.c_str());
		SetDiagnostic(diagnostic, path, "cannot write temporary file");
		return false;
	}
	stream.flush();
	if (!stream)
	{
		stream.close();
		DeleteFileA(temporaryPath.c_str());
		SetDiagnostic(diagnostic, path, "cannot flush temporary file");
		return false;
	}
	stream.close();
	if (stream.fail())
	{
		DeleteFileA(temporaryPath.c_str());
		SetDiagnostic(diagnostic, path, "cannot close temporary file");
		return false;
	}
	if (!MoveFileExA(temporaryPath.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
	{
		const DWORD errorCode = GetLastError();
		DeleteFileA(temporaryPath.c_str());
		SetDiagnostic(diagnostic, path, WindowsError("cannot atomically replace target", errorCode));
		return false;
	}
	return true;
}
}

const char* ConfigStore::FileName()
{
	return "MyFanControl.json";
}

ConfigLoadStatus ConfigStore::Load(const std::string& path, FanConfig* output, std::string* diagnostic)
{
	ClearDiagnostic(diagnostic);
	if (output == nullptr)
	{
		SetDiagnostic(diagnostic, path, "output is null");
		return ConfigLoadStatus::Invalid;
	}
	output->LoadDefault();
	std::string text;
	const ReadStatus status = ReadJsonText(path, &text, diagnostic);
	if (status == ReadStatus::Missing) return ConfigLoadStatus::Missing;
	if (status == ReadStatus::IoError) return ConfigLoadStatus::IoError;
	if (status == ReadStatus::Invalid) return ConfigLoadStatus::Invalid;
	std::string reason;
	FanConfig candidate;
	if (!ParseConfig(text, &candidate, &reason))
	{
		SetDiagnostic(diagnostic, path, reason);
		return ConfigLoadStatus::Invalid;
	}
	*output = candidate;
	ClearDiagnostic(diagnostic);
	return ConfigLoadStatus::Loaded;
}

bool ConfigStore::Save(const std::string& path, const FanConfig& config, std::string* diagnostic)
{
	ClearDiagnostic(diagnostic);
	std::string validationError;
	if (!config.Validate(&validationError))
	{
		SetDiagnostic(diagnostic, path, "configuration is invalid: " + validationError);
		return false;
	}
	JsonValue root = BuildConfig(config);
	std::string text;
	std::string serializationError;
	if (!root.Serialize(&text, true, &serializationError))
	{
		SetDiagnostic(diagnostic, path, "cannot serialize JSON: " + serializationError);
		return false;
	}
	if (text.size() > kMaxJsonFileSize)
	{
		SetDiagnostic(diagnostic, path, "serialized JSON exceeds the 64 KiB limit");
		return false;
	}
	if (!SaveTextAtomically(path, text, diagnostic)) return false;
	ClearDiagnostic(diagnostic);
	return true;
}
