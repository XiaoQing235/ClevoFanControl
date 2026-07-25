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

void SetDiagnosticNoThrow(std::string* diagnostic, const char* message) noexcept
{
	if (diagnostic == nullptr)
	{
		return;
	}
	try
	{
		diagnostic->assign(message);
	}
	catch (...)
	{
		diagnostic->clear();
	}
}

bool Fail(const char* message, std::string* diagnostic) noexcept
{
	SetDiagnosticNoThrow(diagnostic, message);
	return false;
}

bool FailConversion(bool calculatingSize, DWORD error, std::string* diagnostic) noexcept
{
	if (error == ERROR_NO_UNICODE_TRANSLATION)
	{
		return Fail("Task XML contains invalid Unicode and cannot be encoded as UTF-8.", diagnostic);
	}
	return Fail(calculatingSize ?
		"WideCharToMultiByte failed while calculating the UTF-8 task XML size." :
		"WideCharToMultiByte failed while encoding the task XML as UTF-8.", diagnostic);
}

bool IsHighSurrogate(unsigned int codeUnit)
{
	return codeUnit >= 0xd800 && codeUnit <= 0xdbff;
}

bool IsLowSurrogate(unsigned int codeUnit)
{
	return codeUnit >= 0xdc00 && codeUnit <= 0xdfff;
}

bool IsXml10BmpCharacter(unsigned int codeUnit)
{
	return codeUnit == 0x0009 || codeUnit == 0x000a || codeUnit == 0x000d ||
		(codeUnit >= 0x0020 && codeUnit <= 0xd7ff) ||
		(codeUnit >= 0xe000 && codeUnit <= 0xfffd);
}

bool ValidateXml10Characters(const std::wstring& text, std::string* diagnostic) noexcept
{
	for (size_t i = 0; i < text.size(); ++i)
	{
		const unsigned int codeUnit = static_cast<unsigned int>(text[i]);
		if (IsHighSurrogate(codeUnit))
		{
			if (i + 1 >= text.size() ||
				!IsLowSurrogate(static_cast<unsigned int>(text[i + 1])))
			{
				return Fail("Task XML target path contains an unpaired UTF-16 surrogate.", diagnostic);
			}
			++i;
			continue;
		}
		if (IsLowSurrogate(codeUnit))
		{
			return Fail("Task XML target path contains an unpaired UTF-16 surrogate.", diagnostic);
		}
		if (!IsXml10BmpCharacter(codeUnit))
		{
			return Fail("Task XML target path contains a character prohibited by XML 1.0.", diagnostic);
		}
	}
	return true;
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
	if (!ValidateXml10Characters(targetPath, diagnostic))
	{
		return false;
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
			return FailConversion(true, GetLastError(), diagnostic);
		}

		std::string utf8(static_cast<size_t>(requiredBytes), '\0');
		const int bytesWritten = WideCharToMultiByte(
			CP_UTF8, WC_ERR_INVALID_CHARS, document.c_str(), sourceLength,
			&utf8[0], requiredBytes, nullptr, nullptr);
		if (bytesWritten != requiredBytes)
		{
			const DWORD error = bytesWritten == 0 ? GetLastError() : ERROR_INVALID_DATA;
			return FailConversion(false, error, diagnostic);
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
