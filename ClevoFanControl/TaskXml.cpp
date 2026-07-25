#include "TaskXml.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <limits>
#include <new>
#include <stdexcept>
#include <string>

namespace
{
const wchar_t kTaskXmlPrefix[] =
	L"<?xml version=\"1.0\" encoding=\"UTF-8\"?>\r\n"
	L"<Task version=\"1.2\" xmlns=\"http://schemas.microsoft.com/windows/2004/02/mit/task\">\r\n"
	L"  <Triggers><LogonTrigger><Enabled>true</Enabled></LogonTrigger></Triggers>\r\n"
	L"  <Principals><Principal id=\"Author\"><GroupId>S-1-5-32-545</GroupId><RunLevel>HighestAvailable</RunLevel></Principal></Principals>\r\n"
	L"  <Settings><MultipleInstancesPolicy>StopExisting</MultipleInstancesPolicy><StartWhenAvailable>true</StartWhenAvailable><AllowStartOnDemand>true</AllowStartOnDemand><Enabled>true</Enabled></Settings>\r\n"
	L"  <Actions Context=\"Author\"><Exec><Command>";

const wchar_t kTaskXmlSuffix[] =
	L"</Command></Exec></Actions>\r\n"
	L"</Task>\r\n";

bool Fail(const char* message, std::string* diagnostic)
{
	if (diagnostic != nullptr)
	{
		*diagnostic = message;
	}
	return false;
}

bool FailConversion(const char* operation, DWORD error, std::string* diagnostic)
{
	if (diagnostic != nullptr)
	{
		if (error == ERROR_NO_UNICODE_TRANSLATION)
		{
			*diagnostic = "Task XML contains invalid Unicode and cannot be encoded as UTF-8.";
		}
		else
		{
			*diagnostic = std::string(operation) + " failed with Windows error " +
				std::to_string(static_cast<unsigned long>(error)) + ".";
		}
	}
	return false;
}

size_t EscapedCharacterLength(wchar_t character)
{
	switch (character)
	{
	case L'&':
		return 5;
	case L'<':
	case L'>':
		return 4;
	case L'\"':
	case L'\'':
		return 6;
	default:
		return 1;
	}
}

void AppendEscapedCharacter(wchar_t character, std::wstring* escaped)
{
	switch (character)
	{
	case L'&':
		escaped->append(L"&amp;");
		break;
	case L'<':
		escaped->append(L"&lt;");
		break;
	case L'>':
		escaped->append(L"&gt;");
		break;
	case L'\"':
		escaped->append(L"&quot;");
		break;
	case L'\'':
		escaped->append(L"&apos;");
		break;
	default:
		escaped->push_back(character);
		break;
	}
}
}

bool BuildTaskXmlUtf8(
	const std::wstring& targetPath,
	std::string* output,
	std::string* diagnostic)
{
	if (output == nullptr)
	{
		return Fail("Task XML output pointer must not be null.", diagnostic);
	}

	const size_t prefixLength = (sizeof(kTaskXmlPrefix) / sizeof(kTaskXmlPrefix[0])) - 1;
	const size_t suffixLength = (sizeof(kTaskXmlSuffix) / sizeof(kTaskXmlSuffix[0])) - 1;
	const size_t maximumDocumentLength = static_cast<size_t>((std::numeric_limits<int>::max)());
	const size_t fixedLength = prefixLength + suffixLength;
	size_t escapedLength = 0;
	for (size_t i = 0; i < targetPath.size(); ++i)
	{
		const size_t characterLength = EscapedCharacterLength(targetPath[i]);
		if (escapedLength > maximumDocumentLength - fixedLength - characterLength)
		{
			return Fail("Task XML target path is too large to serialize.", diagnostic);
		}
		escapedLength += characterLength;
	}

	try
	{
		std::wstring document;
		document.reserve(fixedLength + escapedLength);
		document.append(kTaskXmlPrefix);
		for (size_t i = 0; i < targetPath.size(); ++i)
		{
			AppendEscapedCharacter(targetPath[i], &document);
		}
		document.append(kTaskXmlSuffix);

		const int sourceLength = static_cast<int>(document.size());
		const int requiredBytes = WideCharToMultiByte(
			CP_UTF8, WC_ERR_INVALID_CHARS, document.c_str(), sourceLength, nullptr, 0, nullptr, nullptr);
		if (requiredBytes == 0)
		{
			return FailConversion("UTF-8 size calculation", GetLastError(), diagnostic);
		}

		std::string utf8(static_cast<size_t>(requiredBytes), '\0');
		const int bytesWritten = WideCharToMultiByte(
			CP_UTF8, WC_ERR_INVALID_CHARS, document.c_str(), sourceLength,
			&utf8[0], requiredBytes, nullptr, nullptr);
		if (bytesWritten != requiredBytes)
		{
			const DWORD error = bytesWritten == 0 ? GetLastError() : ERROR_INVALID_DATA;
			return FailConversion("UTF-8 conversion", error, diagnostic);
		}

		output->swap(utf8);
	}
	catch (const std::bad_alloc&)
	{
		return Fail("Task XML is too large to allocate.", diagnostic);
	}
	catch (const std::length_error&)
	{
		return Fail("Task XML exceeds the supported string size.", diagnostic);
	}

	if (diagnostic != nullptr)
	{
		diagnostic->clear();
	}
	return true;
}
