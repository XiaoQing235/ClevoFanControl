#ifndef JSON_VALUE_H
#define JSON_VALUE_H

#include <map>
#include <stdint.h>
#include <string>
#include <vector>

class JsonValue
{
public:
	enum class Type
	{
		Null,
		Boolean,
		Integer,
		String,
		Array,
		Object
	};

	typedef std::vector<JsonValue> Array;
	typedef std::map<std::string, JsonValue> Object;

	JsonValue();
	explicit JsonValue(bool value);
	explicit JsonValue(int64_t value);
	explicit JsonValue(const std::string& value);

	static JsonValue MakeArray();
	static JsonValue MakeObject();

	Type GetType() const;
	bool IsNull() const;
	bool IsBoolean() const;
	bool IsInteger() const;
	bool IsString() const;
	bool IsArray() const;
	bool IsObject() const;

	bool GetBoolean(bool* value) const;
	bool GetInteger64(int64_t* value) const;
	bool GetInteger(int* value) const;
	bool GetString(std::string* value) const;
	const Array* GetArray() const;
	const Object* GetObjectValue() const;

	void Append(const JsonValue& value);
	void Set(const std::string& key, const JsonValue& value);

	static bool Parse(const std::string& text, JsonValue* output, std::string* diagnostic);
	bool Serialize(std::string* output, bool pretty, std::string* diagnostic) const;

private:
	Type type_;
	bool booleanValue_;
	int64_t integerValue_;
	std::string stringValue_;
	Array arrayValue_;
	Object objectValue_;
};

#endif
