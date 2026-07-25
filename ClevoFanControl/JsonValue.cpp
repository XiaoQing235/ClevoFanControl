#include "JsonValue.h"

#include <limits>
#include <sstream>

namespace
{
void SetError(std::string* diagnostic, size_t offset, const std::string& message)
{
	if (diagnostic != nullptr)
	{
		std::ostringstream text;
		text << message << " at byte " << offset;
		*diagnostic = text.str();
	}
}

bool IsWhitespace(char value)
{
	return value == ' ' || value == '\t' || value == '\r' || value == '\n';
}

bool IsHex(char value)
{
	return (value >= '0' && value <= '9') ||
		(value >= 'a' && value <= 'f') ||
		(value >= 'A' && value <= 'F');
}

uint32_t HexValue(char value)
{
	if (value >= '0' && value <= '9') return static_cast<uint32_t>(value - '0');
	if (value >= 'a' && value <= 'f') return static_cast<uint32_t>(value - 'a' + 10);
	return static_cast<uint32_t>(value - 'A' + 10);
}

void AppendUtf8(uint32_t codepoint, std::string* output)
{
	if (codepoint <= 0x7fU)
	{
		output->push_back(static_cast<char>(codepoint));
	}
	else if (codepoint <= 0x7ffU)
	{
		output->push_back(static_cast<char>(0xc0U | (codepoint >> 6)));
		output->push_back(static_cast<char>(0x80U | (codepoint & 0x3fU)));
	}
	else if (codepoint <= 0xffffU)
	{
		output->push_back(static_cast<char>(0xe0U | (codepoint >> 12)));
		output->push_back(static_cast<char>(0x80U | ((codepoint >> 6) & 0x3fU)));
		output->push_back(static_cast<char>(0x80U | (codepoint & 0x3fU)));
	}
	else
	{
		output->push_back(static_cast<char>(0xf0U | (codepoint >> 18)));
		output->push_back(static_cast<char>(0x80U | ((codepoint >> 12) & 0x3fU)));
		output->push_back(static_cast<char>(0x80U | ((codepoint >> 6) & 0x3fU)));
		output->push_back(static_cast<char>(0x80U | (codepoint & 0x3fU)));
	}
}

void AppendEscapedString(const std::string& value, std::string* output)
{
	output->push_back('"');
	for (size_t i = 0; i < value.size(); ++i)
	{
		const unsigned char byte = static_cast<unsigned char>(value[i]);
		switch (byte)
		{
		case '"': output->append("\\\""); break;
		case '\\': output->append("\\\\"); break;
		case '\b': output->append("\\b"); break;
		case '\f': output->append("\\f"); break;
		case '\n': output->append("\\n"); break;
		case '\r': output->append("\\r"); break;
		case '\t': output->append("\\t"); break;
		default:
			if (byte < 0x20U)
			{
				static const char hex[] = "0123456789abcdef";
				output->append("\\u00");
				output->push_back(hex[(byte >> 4) & 0x0fU]);
				output->push_back(hex[byte & 0x0fU]);
			}
			else
			{
				output->push_back(static_cast<char>(byte));
			}
			break;
		}
	}
	output->push_back('"');
}

class Parser
{
public:
	Parser(const std::string& text, std::string* diagnostic)
		: text_(text), offset_(0), diagnostic_(diagnostic)
	{
	}

	bool ParseDocument(JsonValue* output)
	{
		SkipWhitespace();
		if (!ParseValue(output))
		{
			return false;
		}
		SkipWhitespace();
		if (offset_ != text_.size())
		{
			return Fail("unexpected trailing characters");
		}
		return true;
	}

private:
	bool Fail(const std::string& message)
	{
		SetError(diagnostic_, offset_, message);
		return false;
	}

	void SkipWhitespace()
	{
		while (offset_ < text_.size() && IsWhitespace(text_[offset_]))
		{
			++offset_;
		}
	}

	bool Consume(char expected)
	{
		if (offset_ >= text_.size() || text_[offset_] != expected)
		{
			return false;
		}
		++offset_;
		return true;
	}

	bool ParseValue(JsonValue* output)
	{
		if (output == nullptr)
		{
			return Fail("output is null");
		}
		if (offset_ >= text_.size())
		{
			return Fail("expected a JSON value");
		}
		switch (text_[offset_])
		{
		case 'n': return ParseLiteral("null", JsonValue(), output);
		case 't': return ParseLiteral("true", JsonValue(true), output);
		case 'f': return ParseLiteral("false", JsonValue(false), output);
		case '"':
		{
			std::string value;
			if (!ParseString(&value)) return false;
			*output = JsonValue(value);
			return true;
		}
		case '[': return ParseArray(output);
		case '{': return ParseObject(output);
		default: return ParseInteger(output);
		}
	}

	bool ParseLiteral(const char* literal, const JsonValue& value, JsonValue* output)
	{
		const size_t start = offset_;
		for (size_t i = 0; literal[i] != '\0'; ++i)
		{
			if (offset_ >= text_.size() || text_[offset_] != literal[i])
			{
				offset_ = start;
				return Fail("invalid JSON literal");
			}
			++offset_;
		}
		*output = value;
		return true;
	}

	bool ParseString(std::string* output)
	{
		if (!Consume('"'))
		{
			return Fail("expected a string");
		}
		output->clear();
		while (offset_ < text_.size())
		{
			const unsigned char byte = static_cast<unsigned char>(text_[offset_++]);
			if (byte == '"')
			{
				return true;
			}
			if (byte < 0x20U)
			{
				return Fail("control character is not allowed in a JSON string");
			}
			if (byte != '\\')
			{
				output->push_back(static_cast<char>(byte));
				continue;
			}
			if (offset_ >= text_.size())
			{
				return Fail("unterminated JSON escape");
			}
			const char escape = text_[offset_++];
			switch (escape)
			{
			case '"': output->push_back('"'); break;
			case '\\': output->push_back('\\'); break;
			case '/': output->push_back('/'); break;
			case 'b': output->push_back('\b'); break;
			case 'f': output->push_back('\f'); break;
			case 'n': output->push_back('\n'); break;
			case 'r': output->push_back('\r'); break;
			case 't': output->push_back('\t'); break;
			case 'u':
			{
				if (offset_ + 4U > text_.size())
				{
					return Fail("incomplete Unicode escape");
				}
				uint32_t codepoint = 0U;
				for (size_t i = 0; i < 4U; ++i)
				{
					if (!IsHex(text_[offset_ + i]))
					{
						return Fail("invalid Unicode escape");
					}
					codepoint = (codepoint << 4) | HexValue(text_[offset_ + i]);
				}
				offset_ += 4U;
				if (codepoint >= 0xd800U && codepoint <= 0xdbffU)
				{
					if (offset_ + 6U > text_.size() || text_[offset_] != '\\' || text_[offset_ + 1U] != 'u')
					{
						return Fail("high surrogate must be followed by a low surrogate");
					}
					offset_ += 2U;
					uint32_t low = 0U;
					for (size_t i = 0; i < 4U; ++i)
					{
						if (!IsHex(text_[offset_ + i]))
						{
							return Fail("invalid low surrogate");
						}
						low = (low << 4) | HexValue(text_[offset_ + i]);
					}
					offset_ += 4U;
					if (low < 0xdc00U || low > 0xdfffU)
					{
						return Fail("invalid low surrogate");
					}
					codepoint = 0x10000U + ((codepoint - 0xd800U) << 10) + (low - 0xdc00U);
				}
				else if (codepoint >= 0xdc00U && codepoint <= 0xdfffU)
				{
					return Fail("unexpected low surrogate");
				}
				AppendUtf8(codepoint, output);
				break;
			}
			default: return Fail("invalid JSON escape");
			}
		}
		return Fail("unterminated JSON string");
	}

	bool ParseInteger(JsonValue* output)
	{
		const size_t start = offset_;
		bool negative = false;
		if (offset_ < text_.size() && text_[offset_] == '-')
		{
			negative = true;
			++offset_;
		}
		if (offset_ >= text_.size() || text_[offset_] < '0' || text_[offset_] > '9')
		{
			offset_ = start;
			return Fail("expected an integer JSON value");
		}
		if (text_[offset_] == '0')
		{
			++offset_;
			if (offset_ < text_.size() && text_[offset_] >= '0' && text_[offset_] <= '9')
			{
				return Fail("leading zero is not allowed");
			}
		}
		else
		{
			while (offset_ < text_.size() && text_[offset_] >= '0' && text_[offset_] <= '9')
			{
				++offset_;
			}
		}
		if (offset_ < text_.size() && (text_[offset_] == '.' || text_[offset_] == 'e' || text_[offset_] == 'E'))
		{
			return Fail("only integer JSON numbers are supported");
		}

		const std::string digits = text_.substr(start, offset_ - start);
		uint64_t magnitude = 0U;
		for (size_t i = negative ? 1U : 0U; i < digits.size(); ++i)
		{
			const uint32_t digit = static_cast<uint32_t>(digits[i] - '0');
			if (magnitude > (std::numeric_limits<uint64_t>::max() - digit) / 10U)
			{
				return Fail("integer JSON number is too large");
			}
			magnitude = magnitude * 10U + digit;
		}
		if ((!negative && magnitude > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) ||
			(negative && magnitude > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) + 1U))
		{
			return Fail("integer JSON number is out of range");
		}
		const int64_t value = negative ?
			(magnitude == static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) + 1U ?
				std::numeric_limits<int64_t>::min() : -static_cast<int64_t>(magnitude)) :
			static_cast<int64_t>(magnitude);
		*output = JsonValue(value);
		return true;
	}

	bool ParseArray(JsonValue* output)
	{
		JsonValue array = JsonValue::MakeArray();
		Consume('[');
		SkipWhitespace();
		if (Consume(']'))
		{
			*output = array;
			return true;
		}
		while (true)
		{
			JsonValue value;
			if (!ParseValue(&value)) return false;
			array.Append(value);
			SkipWhitespace();
			if (Consume(']')) break;
			if (!Consume(',')) return Fail("expected ',' or ']' in JSON array");
			SkipWhitespace();
		}
		*output = array;
		return true;
	}

	bool ParseObject(JsonValue* output)
	{
		JsonValue object = JsonValue::MakeObject();
		Consume('{');
		SkipWhitespace();
		if (Consume('}'))
		{
			*output = object;
			return true;
		}
		while (true)
		{
			std::string key;
			if (!ParseString(&key)) return false;
			SkipWhitespace();
			if (!Consume(':')) return Fail("expected ':' after JSON object key");
			SkipWhitespace();
			JsonValue value;
			if (!ParseValue(&value)) return false;
			const JsonValue::Object* existing = object.GetObjectValue();
			if (existing->find(key) != existing->end())
			{
				return Fail("duplicate JSON object key");
			}
			object.Set(key, value);
			SkipWhitespace();
			if (Consume('}')) break;
			if (!Consume(',')) return Fail("expected ',' or '}' in JSON object");
			SkipWhitespace();
		}
		*output = object;
		return true;
	}

	const std::string& text_;
	size_t offset_;
	std::string* diagnostic_;
};

void Indent(std::string* output, int depth)
{
	for (int i = 0; i < depth * 2; ++i) output->push_back(' ');
}

bool SerializeValue(const JsonValue& value, std::string* output, bool pretty, int depth)
{
	switch (value.GetType())
	{
	case JsonValue::Type::Null:
		output->append("null");
		return true;
	case JsonValue::Type::Boolean:
	{
		bool booleanValue = false;
		value.GetBoolean(&booleanValue);
		output->append(booleanValue ? "true" : "false");
		return true;
	}
	case JsonValue::Type::Integer:
	{
		int64_t integerValue = 0;
		if (!value.GetInteger64(&integerValue)) return false;
		std::ostringstream number;
		number << integerValue;
		output->append(number.str());
		return true;
	}
	case JsonValue::Type::String:
	{
		std::string stringValue;
		value.GetString(&stringValue);
		AppendEscapedString(stringValue, output);
		return true;
	}
	case JsonValue::Type::Array:
	{
		const JsonValue::Array* array = value.GetArray();
		output->push_back('[');
		for (size_t i = 0; i < array->size(); ++i)
		{
			if (i != 0U) output->push_back(',');
			if (pretty) { output->push_back('\n'); Indent(output, depth + 1); }
			if (!SerializeValue((*array)[i], output, pretty, depth + 1)) return false;
		}
		if (pretty && !array->empty()) { output->push_back('\n'); Indent(output, depth); }
		output->push_back(']');
		return true;
	}
	case JsonValue::Type::Object:
	{
		const JsonValue::Object* object = value.GetObjectValue();
		output->push_back('{');
		size_t index = 0U;
		for (JsonValue::Object::const_iterator it = object->begin(); it != object->end(); ++it, ++index)
		{
			if (index != 0U) output->push_back(',');
			if (pretty) { output->push_back('\n'); Indent(output, depth + 1); }
			AppendEscapedString(it->first, output);
			output->push_back(':');
			if (pretty) output->push_back(' ');
			if (!SerializeValue(it->second, output, pretty, depth + 1)) return false;
		}
		if (pretty && !object->empty()) { output->push_back('\n'); Indent(output, depth); }
		output->push_back('}');
		return true;
	}
	}
	return false;
}
}

JsonValue::JsonValue()
	: type_(Type::Null), booleanValue_(false), integerValue_(0)
{
}

JsonValue::JsonValue(bool value)
	: type_(Type::Boolean), booleanValue_(value), integerValue_(0)
{
}

JsonValue::JsonValue(int64_t value)
	: type_(Type::Integer), booleanValue_(false), integerValue_(value)
{
}

JsonValue::JsonValue(const std::string& value)
	: type_(Type::String), booleanValue_(false), integerValue_(0), stringValue_(value)
{
}

JsonValue JsonValue::MakeArray()
{
	JsonValue value;
	value.type_ = Type::Array;
	return value;
}

JsonValue JsonValue::MakeObject()
{
	JsonValue value;
	value.type_ = Type::Object;
	return value;
}

JsonValue::Type JsonValue::GetType() const { return type_; }
bool JsonValue::IsNull() const { return type_ == Type::Null; }
bool JsonValue::IsBoolean() const { return type_ == Type::Boolean; }
bool JsonValue::IsInteger() const { return type_ == Type::Integer; }
bool JsonValue::IsString() const { return type_ == Type::String; }
bool JsonValue::IsArray() const { return type_ == Type::Array; }
bool JsonValue::IsObject() const { return type_ == Type::Object; }

bool JsonValue::GetBoolean(bool* value) const
{
	if (value == nullptr || type_ != Type::Boolean) return false;
	*value = booleanValue_;
	return true;
}

bool JsonValue::GetInteger64(int64_t* value) const
{
	if (value == nullptr || type_ != Type::Integer) return false;
	*value = integerValue_;
	return true;
}

bool JsonValue::GetInteger(int* value) const
{
	if (value == nullptr || type_ != Type::Integer ||
		integerValue_ < static_cast<int64_t>(std::numeric_limits<int>::min()) ||
		integerValue_ > static_cast<int64_t>(std::numeric_limits<int>::max())) return false;
	*value = static_cast<int>(integerValue_);
	return true;
}

bool JsonValue::GetString(std::string* value) const
{
	if (value == nullptr || type_ != Type::String) return false;
	*value = stringValue_;
	return true;
}

const JsonValue::Array* JsonValue::GetArray() const
{
	return type_ == Type::Array ? &arrayValue_ : nullptr;
}

const JsonValue::Object* JsonValue::GetObjectValue() const
{
	return type_ == Type::Object ? &objectValue_ : nullptr;
}

void JsonValue::Append(const JsonValue& value)
{
	if (type_ == Type::Array) arrayValue_.push_back(value);
}

void JsonValue::Set(const std::string& key, const JsonValue& value)
{
	if (type_ == Type::Object) objectValue_[key] = value;
}

bool JsonValue::Parse(const std::string& text, JsonValue* output, std::string* diagnostic)
{
	if (diagnostic != nullptr) diagnostic->clear();
	if (output == nullptr)
	{
		SetError(diagnostic, 0U, "JSON output is null");
		return false;
	}
	JsonValue candidate;
	Parser parser(text, diagnostic);
	if (!parser.ParseDocument(&candidate)) return false;
	*output = candidate;
	return true;
}

bool JsonValue::Serialize(std::string* output, bool pretty, std::string* diagnostic) const
{
	if (diagnostic != nullptr) diagnostic->clear();
	if (output == nullptr)
	{
		if (diagnostic != nullptr) *diagnostic = "JSON output is null";
		return false;
	}
	output->clear();
	if (!SerializeValue(*this, output, pretty, 0))
	{
		if (diagnostic != nullptr) *diagnostic = "unsupported JSON value";
		return false;
	}
	if (pretty) output->push_back('\n');
	return true;
}
