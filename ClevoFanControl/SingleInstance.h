#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

enum class SingleInstanceStatus
{
	Acquired,
	AlreadyRunning,
	Unavailable
};

class SingleInstanceGuard
{
public:
	SingleInstanceGuard();
	~SingleInstanceGuard();

	SingleInstanceStatus Acquire(const wchar_t* name);
	DWORD ErrorCode() const;

	SingleInstanceGuard(const SingleInstanceGuard&) = delete;
	SingleInstanceGuard& operator=(const SingleInstanceGuard&) = delete;

private:
	HANDLE handle_;
	DWORD errorCode_;
};
