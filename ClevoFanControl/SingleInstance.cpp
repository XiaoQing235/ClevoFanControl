#include "SingleInstance.h"

SingleInstanceGuard::SingleInstanceGuard()
	: handle_(nullptr)
	, errorCode_(ERROR_SUCCESS)
{
}

SingleInstanceGuard::~SingleInstanceGuard()
{
	if (handle_ != nullptr)
	{
		CloseHandle(handle_);
	}
}

SingleInstanceStatus SingleInstanceGuard::Acquire(const wchar_t* name)
{
	if (handle_ != nullptr)
	{
		errorCode_ = ERROR_INVALID_STATE;
		return SingleInstanceStatus::Unavailable;
	}

	if (name == nullptr || name[0] == L'\0')
	{
		errorCode_ = ERROR_INVALID_PARAMETER;
		return SingleInstanceStatus::Unavailable;
	}

	HANDLE handle = CreateMutexW(nullptr, FALSE, name);
	if (handle == nullptr)
	{
		errorCode_ = GetLastError();
		return SingleInstanceStatus::Unavailable;
	}

	const DWORD createError = GetLastError();
	if (createError == ERROR_ALREADY_EXISTS)
	{
		CloseHandle(handle);
		errorCode_ = ERROR_ALREADY_EXISTS;
		return SingleInstanceStatus::AlreadyRunning;
	}

	handle_ = handle;
	errorCode_ = ERROR_SUCCESS;
	return SingleInstanceStatus::Acquired;
}

DWORD SingleInstanceGuard::ErrorCode() const
{
	return errorCode_;
}
