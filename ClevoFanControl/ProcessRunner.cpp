#include "ProcessRunner.h"

#include <algorithm>
#include <cerrno>
#include <new>
#include <process.h>
#include <vector>

namespace
{
const size_t kMaximumCapturedOutput = 1024U * 1024U;
const DWORD kTerminationWaitMilliseconds = 2000;
const DWORD kResponsiveThreadGraceMilliseconds = 1000;

class ScopedHandle
{
public:
	ScopedHandle() : m_handle(NULL) {}
	explicit ScopedHandle(HANDLE handle) : m_handle(handle) {}
	~ScopedHandle()
	{
		if (m_handle != NULL && m_handle != INVALID_HANDLE_VALUE)
		{
			CloseHandle(m_handle);
		}
	}

	HANDLE Get() const { return m_handle; }
	HANDLE* Address() { return &m_handle; }
	HANDLE Release()
	{
		HANDLE handle = m_handle;
		m_handle = NULL;
		return handle;
	}
	void Reset(HANDLE handle = NULL)
	{
		if (m_handle != NULL && m_handle != INVALID_HANDLE_VALUE)
		{
			CloseHandle(m_handle);
		}
		m_handle = handle;
	}

private:
	ScopedHandle(const ScopedHandle&);
	ScopedHandle& operator=(const ScopedHandle&);
	HANDLE m_handle;
};

bool DrainAvailableOutput(HANDLE pipe, ProcessRunResult* result)
{
	for (;;)
	{
		DWORD available = 0;
		if (!PeekNamedPipe(pipe, NULL, 0, NULL, &available, NULL))
		{
			const DWORD error = GetLastError();
			if (error == ERROR_BROKEN_PIPE)
			{
				return true;
			}
			result->readSucceeded = false;
			result->errorCode = error;
			return false;
		}
		if (available == 0)
		{
			return true;
		}

		char buffer[4096];
		const DWORD requested = (std::min)(available, static_cast<DWORD>(sizeof(buffer)));
		DWORD bytesRead = 0;
		if (!ReadFile(pipe, buffer, requested, &bytesRead, NULL))
		{
			const DWORD error = GetLastError();
			if (error == ERROR_BROKEN_PIPE)
			{
				return true;
			}
			result->readSucceeded = false;
			result->errorCode = error;
			return false;
		}
		if (bytesRead == 0)
		{
			return true;
		}

		const size_t remaining = result->output.size() < kMaximumCapturedOutput
			? kMaximumCapturedOutput - result->output.size()
			: 0;
		const size_t bytesToKeep = (std::min)(remaining, static_cast<size_t>(bytesRead));
		if (bytesToKeep != 0)
		{
			result->output.append(buffer, bytesToKeep);
		}
		if (bytesToKeep != bytesRead)
		{
			result->outputTruncated = true;
		}
	}
}

void PumpPendingMessages()
{
	MSG message = {};
	while (PeekMessage(&message, NULL, WM_PAINT, WM_PAINT, PM_REMOVE))
	{
		TranslateMessage(&message);
		DispatchMessage(&message);
	}
}

void TerminateAndWait(HANDLE process, DWORD exitCode, ProcessRunResult* result)
{
	if (!TerminateProcess(process, exitCode))
	{
		const DWORD error = GetLastError();
		if (result->errorCode == ERROR_SUCCESS) result->errorCode = error;
	}
	const DWORD waitResult = WaitForSingleObject(process, kTerminationWaitMilliseconds);
	if (waitResult == WAIT_FAILED && result->errorCode == ERROR_SUCCESS)
	{
		result->errorCode = GetLastError();
	}
	else if (waitResult == WAIT_TIMEOUT && result->errorCode == ERROR_SUCCESS)
	{
		result->errorCode = ERROR_TIMEOUT;
	}
}

struct ResponsiveProcessContext
{
	ResponsiveProcessContext()
		: references(1)
		, timeoutMilliseconds(0)
	{
	}

	void AddReference()
	{
		InterlockedIncrement(&references);
	}

	void ReleaseReference()
	{
		if (InterlockedDecrement(&references) == 0)
		{
			delete this;
		}
	}

	LONG references;
	std::wstring applicationPath;
	std::wstring arguments;
	DWORD timeoutMilliseconds;
	ProcessRunResult result;
};

unsigned __stdcall ResponsiveProcessThread(void* parameter)
{
	ResponsiveProcessContext* context =
		reinterpret_cast<ResponsiveProcessContext*>(parameter);
	if (context == nullptr)
	{
		return 0;
	}
	try
	{
		context->result = RunProcess(context->applicationPath,
			context->arguments, context->timeoutMilliseconds, NULL);
	}
	catch (...)
	{
		context->result.errorCode = ERROR_NOT_ENOUGH_MEMORY;
	}
	context->ReleaseReference();
	return 0;
}

}

ProcessRunResult::ProcessRunResult()
	: launched(false)
	, timedOut(false)
	, readSucceeded(true)
	, outputTruncated(false)
	, errorCode(ERROR_SUCCESS)
	, exitCode(STILL_ACTIVE)
{
}

bool ProcessRunResult::Succeeded() const
{
	return launched && !timedOut && readSucceeded && exitCode == 0;
}

ResponsiveWaitResult::ResponsiveWaitResult()
	: completed(false)
	, timedOut(false)
	, errorCode(ERROR_SUCCESS)
{
}

ProcessRunResult RunProcess(const std::wstring& applicationPath,
	const std::wstring& arguments, DWORD timeoutMilliseconds, HWND messageWindow)
{
	ProcessRunResult result;
	if (applicationPath.empty())
	{
		result.errorCode = ERROR_INVALID_PARAMETER;
		return result;
	}

	SECURITY_ATTRIBUTES attributes = {sizeof(attributes), NULL, TRUE};
	ScopedHandle readHandle;
	ScopedHandle writeHandle;
	if (!CreatePipe(readHandle.Address(), writeHandle.Address(), &attributes, 0))
	{
		result.errorCode = GetLastError();
		return result;
	}
	if (!SetHandleInformation(readHandle.Get(), HANDLE_FLAG_INHERIT, 0))
	{
		result.errorCode = GetLastError();
		return result;
	}
	ScopedHandle inputHandle(CreateFileW(L"NUL", GENERIC_READ,
		FILE_SHARE_READ | FILE_SHARE_WRITE, &attributes, OPEN_EXISTING,
		FILE_ATTRIBUTE_NORMAL, NULL));
	if (inputHandle.Get() == INVALID_HANDLE_VALUE)
	{
		result.errorCode = GetLastError();
		return result;
	}

	STARTUPINFOW startup = {};
	startup.cb = sizeof(startup);
	startup.dwFlags = STARTF_USESHOWWINDOW | STARTF_USESTDHANDLES;
	startup.wShowWindow = SW_HIDE;
	startup.hStdInput = inputHandle.Get();
	startup.hStdOutput = writeHandle.Get();
	startup.hStdError = writeHandle.Get();
	PROCESS_INFORMATION process = {};

	std::wstring commandLine = L"\"" + applicationPath + L"\"";
	if (!arguments.empty())
	{
		commandLine += L" ";
		commandLine += arguments;
	}
	std::vector<wchar_t> mutableCommandLine(commandLine.begin(), commandLine.end());
	mutableCommandLine.push_back(L'\0');
	if (!CreateProcessW(applicationPath.c_str(), &mutableCommandLine[0],
		NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &startup, &process))
	{
		result.errorCode = GetLastError();
		return result;
	}
	result.launched = true;
	ScopedHandle processHandle(process.hProcess);
	ScopedHandle threadHandle(process.hThread);
	writeHandle.Reset();

	const ULONGLONG deadline = GetTickCount64() + timeoutMilliseconds;
	for (;;)
	{
		if (!DrainAvailableOutput(readHandle.Get(), &result))
		{
			TerminateAndWait(processHandle.Get(), result.errorCode, &result);
			break;
		}

		const ULONGLONG now = GetTickCount64();
		if (now >= deadline)
		{
			result.timedOut = true;
			result.errorCode = ERROR_TIMEOUT;
			TerminateAndWait(processHandle.Get(), ERROR_TIMEOUT, &result);
			break;
		}

		const DWORD remaining = static_cast<DWORD>((std::min)(
			deadline - now, static_cast<ULONGLONG>(50)));
		DWORD waitResult = WAIT_FAILED;
		if (messageWindow != NULL && IsWindow(messageWindow))
		{
			const HANDLE handle = processHandle.Get();
			waitResult = MsgWaitForMultipleObjectsEx(1, &handle, remaining,
				QS_PAINT | QS_SENDMESSAGE, MWMO_INPUTAVAILABLE);
			if (waitResult == WAIT_OBJECT_0 + 1)
			{
				PumpPendingMessages();
				continue;
			}
		}
		else
		{
			waitResult = WaitForSingleObject(processHandle.Get(), remaining);
		}

		if (waitResult == WAIT_OBJECT_0)
		{
			break;
		}
		if (waitResult == WAIT_FAILED)
		{
			result.errorCode = GetLastError();
			TerminateAndWait(processHandle.Get(), result.errorCode, &result);
			break;
		}
	}

	DrainAvailableOutput(readHandle.Get(), &result);
	if (!GetExitCodeProcess(processHandle.Get(), &result.exitCode))
	{
		result.errorCode = GetLastError();
		result.exitCode = STILL_ACTIVE;
	}
	return result;
}

ResponsiveWaitResult WaitForThreadResponsive(HANDLE thread,
	DWORD timeoutMilliseconds)
{
	ResponsiveWaitResult result;
	if (thread == NULL || thread == INVALID_HANDLE_VALUE)
	{
		result.errorCode = ERROR_INVALID_HANDLE;
		return result;
	}

	bool quitPending = false;
	int quitCode = 0;
	const ULONGLONG deadline = GetTickCount64() + timeoutMilliseconds;
	for (;;)
	{
		MSG quitMessage = {};
		while (PeekMessage(&quitMessage, NULL, WM_QUIT, WM_QUIT, PM_REMOVE))
		{
			if (!quitPending)
			{
				quitCode = static_cast<int>(quitMessage.wParam);
			}
			quitPending = true;
		}

		const DWORD immediateWait = WaitForSingleObject(thread, 0);
		if (immediateWait == WAIT_OBJECT_0)
		{
			result.completed = true;
			break;
		}
		if (immediateWait == WAIT_FAILED)
		{
			result.errorCode = GetLastError();
			break;
		}

		const ULONGLONG now = GetTickCount64();
		if (now >= deadline)
		{
			result.timedOut = true;
			result.errorCode = ERROR_TIMEOUT;
			break;
		}
		const DWORD waitMilliseconds = static_cast<DWORD>((std::min)(
			deadline - now, static_cast<ULONGLONG>(50)));
		const DWORD waitResult = MsgWaitForMultipleObjectsEx(1, &thread,
			waitMilliseconds, QS_PAINT, MWMO_INPUTAVAILABLE);
		if (waitResult == WAIT_OBJECT_0)
		{
			result.completed = true;
			break;
		}
		if (waitResult == WAIT_OBJECT_0 + 1)
		{
			PumpPendingMessages();
			continue;
		}
		if (waitResult == WAIT_FAILED)
		{
			result.errorCode = GetLastError();
			break;
		}
	}
	if (quitPending)
	{
		PostQuitMessage(quitCode);
	}
	return result;
}

ProcessRunResult RunProcessResponsive(const std::wstring& applicationPath,
	const std::wstring& arguments, DWORD timeoutMilliseconds)
{
	ResponsiveProcessContext* context = new (std::nothrow) ResponsiveProcessContext;
	if (context == NULL)
	{
		ProcessRunResult result;
		result.errorCode = ERROR_NOT_ENOUGH_MEMORY;
		return result;
	}
	try
	{
		context->applicationPath = applicationPath;
		context->arguments = arguments;
		context->timeoutMilliseconds = timeoutMilliseconds;
	}
	catch (...)
	{
		context->ReleaseReference();
		ProcessRunResult result;
		result.errorCode = ERROR_NOT_ENOUGH_MEMORY;
		return result;
	}

	context->AddReference();
	const uintptr_t threadValue = _beginthreadex(NULL, 0,
		ResponsiveProcessThread, context, 0, NULL);
	if (threadValue == 0)
	{
		const int threadError = errno;
		context->ReleaseReference();
		context->ReleaseReference();
		ProcessRunResult result;
		result.errorCode = threadError == EINVAL
			? ERROR_INVALID_PARAMETER : ERROR_NOT_ENOUGH_MEMORY;
		return result;
	}
	ScopedHandle thread(reinterpret_cast<HANDLE>(threadValue));
	const ULONGLONG maximumWait = static_cast<ULONGLONG>(timeoutMilliseconds) +
		kTerminationWaitMilliseconds + kResponsiveThreadGraceMilliseconds;
	const DWORD responsiveTimeout = maximumWait > MAXDWORD
		? MAXDWORD : static_cast<DWORD>(maximumWait);
	const ResponsiveWaitResult waitResult =
		WaitForThreadResponsive(thread.Get(), responsiveTimeout);
	if (!waitResult.completed)
	{
		ProcessRunResult result;
		result.timedOut = waitResult.timedOut;
		result.errorCode = waitResult.errorCode;
		context->ReleaseReference();
		return result;
	}
	ProcessRunResult result = context->result;
	context->ReleaseReference();
	return result;
}
