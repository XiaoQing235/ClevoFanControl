#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <string>

struct ProcessRunResult
{
	bool launched;
	bool timedOut;
	bool readSucceeded;
	bool outputTruncated;
	DWORD errorCode;
	DWORD exitCode;
	std::string output;

	ProcessRunResult();
	bool Succeeded() const;
};

struct ResponsiveWaitResult
{
	bool completed;
	bool timedOut;
	DWORD errorCode;

	ResponsiveWaitResult();
};

ProcessRunResult RunProcess(
	const std::wstring& applicationPath,
	const std::wstring& arguments,
	DWORD timeoutMilliseconds,
	HWND messageWindow = NULL);

ProcessRunResult RunProcessResponsive(
	const std::wstring& applicationPath,
	const std::wstring& arguments,
	DWORD timeoutMilliseconds);

ResponsiveWaitResult WaitForThreadResponsive(
	HANDLE thread,
	DWORD timeoutMilliseconds);
