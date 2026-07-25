#include "PresetStore.h"
#include "JsonValue.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <atomic>
#include <fstream>
#include <limits>
#include <set>
#include <sstream>

namespace
{
const uint64_t kMaxJsonFileSize = 64ULL * 1024ULL;
const size_t kMaxPresetCount = 32U;
const size_t kMaxNameLength = 64U;
const size_t kMaxPatternLength = 128U;
std::atomic<unsigned long> g_tempSequence(0);

void ClearDiagnostic(std::string* diagnostic)
{
	if (diagnostic != nullptr) diagnostic->clear();
}

void SetDiagnostic(std::string* diagnostic, const std::string& path, const std::string& reason)
{
	if (diagnostic != nullptr) *diagnostic = "PresetStore [" + path + "]: " + reason;
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
	if (length > 0) stream.read(&(*text)[0], length);
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

bool ReadString(const JsonValue::Object& object, const char* name, std::string* output, std::string* reason)
{
	const JsonValue* value = nullptr;
	if (!GetField(object, name, &value, reason) || !value->GetString(output))
	{
		if (reason != nullptr && reason->empty()) *reason = std::string("JSON field '") + name + "' must be a string";
		return false;
	}
	return true;
}

bool SameCurve(const FanCurvePoints& left, const FanCurvePoints& right)
{
	if (left.size() != right.size()) return false;
	for (size_t i = 0; i < left.size(); ++i)
	{
		if (left[i].temperature != right[i].temperature || left[i].duty != right[i].duty) return false;
	}
	return true;
}

bool HasControlByte(const std::string& value)
{
	for (size_t i = 0; i < value.size(); ++i)
	{
		const unsigned char byte = static_cast<unsigned char>(value[i]);
		if (byte == 0U || byte < 0x20U || byte == 0x7fU) return true;
	}
	return false;
}

bool IsOrdinaryPatternByte(unsigned char byte)
{
	return byte >= 0x20U && byte <= 0x7eU && byte != '/' && byte != '\\' && byte != ':' &&
		byte != '"' && byte != '<' && byte != '>' && byte != '|' && byte != '[' && byte != ']';
}

bool ValidatePattern(const std::string& pattern, std::string* error, size_t index)
{
	if (pattern.empty())
	{
		if (error != nullptr) *error = "preset " + std::to_string(index) + " has an empty process pattern";
		return false;
	}
	if (pattern.size() > kMaxPatternLength)
	{
		if (error != nullptr) *error = "preset " + std::to_string(index) + " process pattern exceeds 128 bytes";
		return false;
	}
	for (size_t i = 0; i < pattern.size(); ++i)
	{
		if (!IsOrdinaryPatternByte(static_cast<unsigned char>(pattern[i])))
		{
			if (error != nullptr) *error = "preset " + std::to_string(index) + " process pattern contains an invalid byte";
			return false;
		}
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

bool ReadPreset(const JsonValue& value, FanPreset* output, std::string* reason)
{
	static const char* const fields[] = {
		"name", "processPattern", "updateInterval", "transitionTemp", "forceTemp", "linear",
		"takeOver", "forceCooling", "softControl", "cpuCurve", "gpuCurve"
	};
	const JsonValue::Object* object = nullptr;
	if (!HasExactFields(value, fields, sizeof(fields) / sizeof(fields[0]), &object, reason)) return false;
	FanPreset candidate;
	candidate.config.LoadDefault();
	if (!ReadString(*object, "name", &candidate.name, reason) ||
		!ReadString(*object, "processPattern", &candidate.processPattern, reason) ||
		!ReadInteger(*object, "updateInterval", &candidate.config.UpdateInterval, reason) ||
		!ReadInteger(*object, "transitionTemp", &candidate.config.TransitionTemp, reason) ||
		!ReadInteger(*object, "forceTemp", &candidate.config.ForceTemp, reason) ||
		!ReadBoolean(*object, "linear", &candidate.config.Linear, reason) ||
		!ReadBoolean(*object, "takeOver", &candidate.config.TakeOver, reason) ||
		!ReadBoolean(*object, "forceCooling", &candidate.config.ForceCooling, reason) ||
		!ReadBoolean(*object, "softControl", &candidate.config.SoftControl, reason)) return false;
	const JsonValue* cpu = nullptr;
	const JsonValue* gpu = nullptr;
	if (!GetField(*object, "cpuCurve", &cpu, reason) || !GetField(*object, "gpuCurve", &gpu, reason) ||
		!ReadCurve(*cpu, "cpuCurve", &candidate.config.CpuCurve, reason) ||
		!ReadCurve(*gpu, "gpuCurve", &candidate.config.GpuCurve, reason)) return false;
	*output = candidate;
	return true;
}

bool ParseCollection(const std::string& text, PresetCollection* output, std::string* reason)
{
	JsonValue root;
	std::string parseError;
	if (!JsonValue::Parse(text, &root, &parseError))
	{
		if (reason != nullptr) *reason = "invalid JSON: " + parseError;
		return false;
	}
	static const char* const fields[] = {"autoSwitch", "presets"};
	const JsonValue::Object* object = nullptr;
	if (!HasExactFields(root, fields, 2U, &object, reason)) return false;
	PresetCollection candidate;
	if (!ReadBoolean(*object, "autoSwitch", &candidate.autoSwitch, reason)) return false;
	const JsonValue* presetsValue = nullptr;
	if (!GetField(*object, "presets", &presetsValue, reason)) return false;
	const JsonValue::Array* presets = presetsValue->GetArray();
	if (presets == nullptr)
	{
		if (reason != nullptr) *reason = "JSON field 'presets' must be an array";
		return false;
	}
	if (presets->size() > kMaxPresetCount)
	{
		if (reason != nullptr) *reason = "preset count exceeds 32";
		return false;
	}
	candidate.presets.reserve(presets->size());
	for (size_t i = 0; i < presets->size(); ++i)
	{
		FanPreset preset;
		if (!ReadPreset((*presets)[i], &preset, reason)) return false;
		candidate.presets.push_back(preset);
	}
	if (!candidate.Validate(reason)) return false;
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

JsonValue BuildPreset(const FanPreset& preset)
{
	JsonValue value = JsonValue::MakeObject();
	value.Set("name", JsonValue(preset.name));
	value.Set("processPattern", JsonValue(preset.processPattern));
	value.Set("updateInterval", Integer(preset.config.UpdateInterval));
	value.Set("transitionTemp", Integer(preset.config.TransitionTemp));
	value.Set("forceTemp", Integer(preset.config.ForceTemp));
	value.Set("linear", JsonValue(preset.config.Linear));
	value.Set("takeOver", JsonValue(preset.config.TakeOver));
	value.Set("forceCooling", JsonValue(preset.config.ForceCooling));
	value.Set("softControl", JsonValue(preset.config.SoftControl));
	value.Set("cpuCurve", BuildCurve(preset.config.CpuCurve));
	value.Set("gpuCurve", BuildCurve(preset.config.GpuCurve));
	return value;
}

JsonValue BuildCollection(const PresetCollection& collection)
{
	JsonValue root = JsonValue::MakeObject();
	root.Set("autoSwitch", JsonValue(collection.autoSwitch));
	JsonValue presets = JsonValue::MakeArray();
	for (size_t i = 0; i < collection.presets.size(); ++i) presets.Append(BuildPreset(collection.presets[i]));
	root.Set("presets", presets);
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

PresetCollection::PresetCollection()
{
	LoadDefault();
}

void PresetCollection::LoadDefault()
{
	autoSwitch = false;
	presets.clear();
}

bool PresetCollection::Validate(std::string* error) const
{
	if (error != nullptr) error->clear();
	if (presets.size() > kMaxPresetCount)
	{
		if (error != nullptr) *error = "preset count exceeds 32";
		return false;
	}
	std::set<std::string> names;
	for (size_t i = 0; i < presets.size(); ++i)
	{
		const FanPreset& preset = presets[i];
		if (preset.name.empty())
		{
			if (error != nullptr) *error = "preset " + std::to_string(i) + " has an empty name";
			return false;
		}
		if (preset.name.size() > kMaxNameLength)
		{
			if (error != nullptr) *error = "preset " + std::to_string(i) + " name exceeds 64 bytes";
			return false;
		}
		if (HasControlByte(preset.name))
		{
			if (error != nullptr) *error = "preset " + std::to_string(i) + " name contains a control byte";
			return false;
		}
		if (!names.insert(preset.name).second)
		{
			if (error != nullptr) *error = "preset names must be unique";
			return false;
		}
		if (!ValidatePattern(preset.processPattern, error, i)) return false;
		std::string configError;
		if (!preset.config.Validate(&configError))
		{
			if (error != nullptr) *error = "preset " + std::to_string(i) + " configuration: " + configError;
			return false;
		}
	}
	return true;
}

bool SameControlSettings(const FanConfig& left, const FanConfig& right)
{
	return SameCurve(left.CpuCurve, right.CpuCurve) && SameCurve(left.GpuCurve, right.GpuCurve) &&
		left.TransitionTemp == right.TransitionTemp && left.UpdateInterval == right.UpdateInterval &&
		left.ForceTemp == right.ForceTemp && left.Linear == right.Linear && left.TakeOver == right.TakeOver &&
		left.ForceCooling == right.ForceCooling && left.SoftControl == right.SoftControl;
}

void CopyControlSettings(const FanConfig& source, FanConfig* target)
{
	if (target == nullptr) return;
	target->CpuCurve = source.CpuCurve;
	target->GpuCurve = source.GpuCurve;
	target->TransitionTemp = source.TransitionTemp;
	target->UpdateInterval = source.UpdateInterval;
	target->ForceTemp = source.ForceTemp;
	target->Linear = source.Linear;
	target->TakeOver = source.TakeOver;
	target->ForceCooling = source.ForceCooling;
	target->SoftControl = source.SoftControl;
}

const char* PresetStore::FileName()
{
	return "ClevoFanControl.presets.json";
}

PresetLoadStatus PresetStore::Load(const std::string& path, PresetCollection* output, std::string* diagnostic)
{
	ClearDiagnostic(diagnostic);
	if (output == nullptr)
	{
		SetDiagnostic(diagnostic, path, "output is null");
		return PresetLoadStatus::Invalid;
	}
	output->LoadDefault();
	std::string text;
	const ReadStatus status = ReadJsonText(path, &text, diagnostic);
	if (status == ReadStatus::Missing) return PresetLoadStatus::Missing;
	if (status == ReadStatus::IoError) return PresetLoadStatus::IoError;
	if (status == ReadStatus::Invalid) return PresetLoadStatus::Invalid;
	PresetCollection candidate;
	std::string reason;
	if (!ParseCollection(text, &candidate, &reason))
	{
		SetDiagnostic(diagnostic, path, reason);
		return PresetLoadStatus::Invalid;
	}
	*output = candidate;
	ClearDiagnostic(diagnostic);
	return PresetLoadStatus::Loaded;
}

bool PresetStore::Save(const std::string& path, const PresetCollection& collection, std::string* diagnostic)
{
	ClearDiagnostic(diagnostic);
	std::string validationError;
	if (!collection.Validate(&validationError))
	{
		SetDiagnostic(diagnostic, path, validationError);
		return false;
	}
	JsonValue root = BuildCollection(collection);
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
