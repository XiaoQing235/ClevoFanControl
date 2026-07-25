#include "stdafx.h"
#include "Core.h"
#include "FanControlLogic.h"

#ifdef max
#undef max
#endif
#ifdef min
#undef min
#endif

#include <algorithm>
#include <stdlib.h>
#include <time.h>

namespace
{
CString DiagnosticToCString(const std::string& diagnostic)
{
	CStringA narrow(diagnostic.c_str());
	return CString(narrow);
}

std::string ConfigurationPath()
{
	CStringA narrow = CStringA(GetExePath());
	narrow += "\\";
	narrow += ConfigStore::FileName();
	return std::string(narrow.GetString());
}

int CurveLevelAtTemperature(const FanCurvePoints& curve, int temperature)
{
	if (curve.empty())
	{
		return 0;
	}

	size_t index = 0;
	while (index + 1 < curve.size() && temperature >= curve[index + 1].temperature)
	{
		++index;
	}
	return static_cast<int>(index + 1);
}

int CurveLastLevel(const FanCurvePoints& curve)
{
	return static_cast<int>(curve.size());
}

}

int GetTime(tm* pt, int offset)
{
	tm t = {0};
	time_t tt;
	if (!pt)
	{
		pt = &t;
	}
	time(&tt);
	tt += offset;
	localtime_s(pt, &tt);
	return pt->tm_hour * 10000 + pt->tm_min * 100 + pt->tm_sec;
}

int GetTimeInterval(int a, int b, int* p)
{
	int a1 = a / 10000;
	int a2 = (a % 10000) / 100;
	int a3 = a % 100;

	int b1 = b / 10000;
	int b2 = (b % 10000) / 100;
	int b3 = b % 100;

	int seconds = (a1 - b1) * 3600 + (a2 - b2) * 60 + a3 - b3;
	if (p)
	{
		*p = seconds;
	}
	const int sign = seconds >= 0 ? 1 : -1;
	seconds = abs(seconds);
	const int encoded = (seconds / 3600) * 10000 +
		(seconds % 3600) / 60 * 100 + seconds % 60;
	return encoded * sign;
}

CString GetExePath()
{
	TCHAR pathbuf[MAX_PATH] = {0};
	DWORD pathlen = ::GetModuleFileName(NULL, pathbuf, MAX_PATH);
	if (pathlen == 0 || pathlen >= MAX_PATH)
	{
		return _T("");
	}

	int index = static_cast<int>(pathlen) - 1;
	while (index >= 0 && pathbuf[index] != _T('\\'))
	{
		--index;
	}
	if (index >= 0)
	{
		pathbuf[index + 1] = _T('\0');
	}
	else
	{
		pathbuf[0] = _T('\0');
	}
	return CString(pathbuf);
}

void CCore::ClearEcApi()
{
	m_pfnInitIo = NULL;
	m_pfnSetFanDuty = NULL;
	m_pfnSetFANDutyAuto = NULL;
	m_pfnGetTempFanDuty = NULL;
	m_pfnGetFANCounter = NULL;
	m_pfnGetECVersion = NULL;
	m_pfnGetFANRPM[0] = NULL;
	m_pfnGetFANRPM[1] = NULL;
}

void CCore::ResetCurveState()
{
	EnterCriticalSection(&m_csFanControl);
	for (int i = 0; i < 2; ++i)
	{
		m_nLinearTemperature[i] = 0;
		m_nSelectedCurvePoint[i] = 0;
	}
	LeaveCriticalSection(&m_csFanControl);
}

CCore::CCore()
{
	ClearEcApi();
	m_nInit = 0;
	m_nExit = 0;
	m_hInstDLL = NULL;
	m_nTimerID = 0;
	m_hSoftControlThread = NULL;
	m_pParentDlg = NULL;
	m_nConfigGeneration = 0;

	InitializeCriticalSectionEx(&m_csFanControl, 0, 0);
	InitializeCriticalSectionEx(&m_csConfig, 0, 0);

	for (int i = 0; i < 2; ++i)
	{
		m_nCurTemp[i] = 0;
		m_nLastTemp[i] = 0;
		m_nSetDuty[i] = 0;
		m_nSetDutyLevel[i] = 0;
		m_nCurDuty[i] = 0;
		m_nCurRPM[i] = 0;
		m_bFanAvailable[i] = FALSE;
		m_nSoftTargetDuty[i] = 0;
		m_nSoftCurrentDuty[i] = 0;
	}
	ResetCurveState();
	m_bUpdateRPM = FALSE;
	m_nLastUpdateTime = GetTime(NULL, -5);
	m_bForcedCooling = FALSE;
	m_nForceCoolingCompletionSequence = 0;
	m_bTakeOverStatus = FALSE;
	m_bForcedRefresh = FALSE;
}

CCore::~CCore()
{
	RequestExit(1);
	if (m_nTimerID != 0)
	{
		timeKillEvent(m_nTimerID);
		m_nTimerID = 0;
	}

	if (m_hSoftControlThread != NULL)
	{
		WaitForSingleObject(m_hSoftControlThread, INFINITE);
		CloseHandle(m_hSoftControlThread);
		m_hSoftControlThread = NULL;
	}

	SetParentDialog(NULL);
	Uninit();
	DeleteCriticalSection(&m_csFanControl);
	DeleteCriticalSection(&m_csConfig);
}

void CCore::SetParentDialog(CClevoFanControlDlg* pDlg)
{
	m_pParentDlg = pDlg;
}

BOOL CCore::Init()
{
	if (m_hInstDLL != NULL && m_nInit == 1)
	{
		return TRUE;
	}
	if (m_hInstDLL != NULL)
	{
		FreeLibrary(m_hInstDLL);
		m_hInstDLL = NULL;
	}
	ClearEcApi();

	m_nInit = -1;
	CString dllPath = GetExePath() + _T("\\ClevoEcInfo.dll");
	m_hInstDLL = LoadLibrary(dllPath);
	if (m_hInstDLL == NULL)
	{
		TRACE("Cannot load ClevoEcInfo.dll: %s\n", dllPath.GetString());
		m_nInit = 2;
		return FALSE;
	}

	m_pfnInitIo = reinterpret_cast<InitIo*>(::GetProcAddress(m_hInstDLL, "InitIo"));
	m_pfnSetFanDuty = reinterpret_cast<::SetFanDuty*>(::GetProcAddress(m_hInstDLL, "SetFanDuty"));
	m_pfnSetFANDutyAuto = reinterpret_cast<SetFANDutyAuto*>(::GetProcAddress(m_hInstDLL, "SetFanDutyAuto"));
	m_pfnGetTempFanDuty = reinterpret_cast<GetTempFanDuty*>(::GetProcAddress(m_hInstDLL, "GetTempFanDuty"));
	m_pfnGetFANCounter = reinterpret_cast<GetFANCounter*>(::GetProcAddress(m_hInstDLL, "GetFanCount"));
	m_pfnGetECVersion = reinterpret_cast<GetECVersion*>(::GetProcAddress(m_hInstDLL, "GetECVersion"));
	m_pfnGetFANRPM[0] = reinterpret_cast<GetFanRpm*>(::GetProcAddress(m_hInstDLL, "GetCpuFanRpm"));
	m_pfnGetFANRPM[1] = reinterpret_cast<GetFanRpm*>(::GetProcAddress(m_hInstDLL, "GetGpuFanRpm"));

	if (m_pfnInitIo == NULL ||
		m_pfnSetFanDuty == NULL ||
		m_pfnSetFANDutyAuto == NULL ||
		m_pfnGetTempFanDuty == NULL ||
		m_pfnGetFANCounter == NULL ||
		m_pfnGetECVersion == NULL ||
		m_pfnGetFANRPM[0] == NULL ||
		m_pfnGetFANRPM[1] == NULL)
	{
		TRACE0("ClevoEcInfo.dll is missing one or more exports\n");
		FreeLibrary(m_hInstDLL);
		m_hInstDLL = NULL;
		ClearEcApi();
		m_nInit = 2;
		return FALSE;
	}

	if (m_pfnInitIo() != 1)
	{
		TRACE0("ClevoEcInfo.dll initialization failed\n");
		FreeLibrary(m_hInstDLL);
		m_hInstDLL = NULL;
		ClearEcApi();
		m_nInit = 2;
		return FALSE;
	}

	m_nInit = 1;
	TRACE0("ClevoEcInfo.dll initialized\n");
	return TRUE;
}

void CCore::Uninit()
{
	ResetFan();
	if (m_hInstDLL != NULL)
	{
		FreeLibrary(m_hInstDLL);
		m_hInstDLL = NULL;
	}
	ClearEcApi();
	EnterCriticalSection(&m_csFanControl);
	m_bFanAvailable[0] = FALSE;
	m_bFanAvailable[1] = FALSE;
	LeaveCriticalSection(&m_csFanControl);
	m_nInit = 0;
}

BOOL CCore::LoadConfiguration(CString* warning)
{
	if (warning != NULL)
	{
		warning->Empty();
	}

	const std::string path = ConfigurationPath();
	CConfig loaded;
	std::string diagnostic;
	const ConfigLoadStatus status = ConfigStore::Load(path, &loaded, &diagnostic);
	if (status == ConfigLoadStatus::Missing)
	{
		loaded.LoadDefault();
	}
	else if (status == ConfigLoadStatus::Invalid || status == ConfigLoadStatus::IoError)
	{
		loaded.LoadDefault();
		if (warning != NULL && !diagnostic.empty())
		{
			*warning = DiagnosticToCString(diagnostic);
		}
	}
	else if (status != ConfigLoadStatus::Loaded)
	{
		loaded.LoadDefault();
		if (warning != NULL && !diagnostic.empty())
		{
			*warning = DiagnosticToCString(diagnostic);
		}
	}

	return ApplyConfig(loaded);
}

BOOL CCore::GetConfigSnapshot(CConfig* output)
{
	return GetConfigSnapshot(output, NULL);
}

BOOL CCore::GetConfigSnapshot(CConfig* output, LONG* generation)
{
	if (output == NULL)
	{
		return FALSE;
	}
	EnterCriticalSection(&m_csConfig);
	*output = m_config;
	if (generation != NULL)
	{
		*generation = InterlockedCompareExchange(&m_nConfigGeneration, 0, 0);
	}
	LeaveCriticalSection(&m_csConfig);
	return TRUE;
}

BOOL CCore::ApplyConfig(const CConfig& config)
{
	std::string validationError;
	const BOOL valid = config.Validate(&validationError) ? TRUE : FALSE;

	if (!valid)
	{
		TRACE("Rejected configuration: %s\n", validationError.c_str());
		return FALSE;
	}

	EnterCriticalSection(&m_csConfig);
	m_config = config;
	ResetCurveState();
	EnterCriticalSection(&m_csFanControl);
	m_bForcedCooling = config.ForceCooling ? TRUE : FALSE;
	InterlockedIncrement(&m_nConfigGeneration);
	LeaveCriticalSection(&m_csFanControl);
	LeaveCriticalSection(&m_csConfig);
	InterlockedExchange(reinterpret_cast<volatile LONG*>(&m_bForcedRefresh), TRUE);
	return valid;
}

BOOL CCore::GetStatusSnapshot(CCoreStatusSnapshot* output) const
{
	if (output == NULL)
	{
		return FALSE;
	}

	output->ecReady = InterlockedCompareExchange(
		reinterpret_cast<volatile LONG*>(const_cast<BOOL*>(&m_nInit)), 0, 0) == 1;

	EnterCriticalSection(&m_csConfig);
	const BOOL softControl = m_config.SoftControl ? TRUE : FALSE;
	LeaveCriticalSection(&m_csConfig);

	EnterCriticalSection(&m_csFanControl);
	for (int i = 0; i < 2; ++i)
	{
		output->currentTemperature[i] = m_nCurTemp[i];
		output->currentDuty[i] = m_nCurDuty[i];
		output->currentRPM[i] = m_nCurRPM[i];
		output->targetDuty[i] = softControl ? m_nSoftTargetDuty[i] : m_nSetDuty[i];
		output->targetCurveLevel[i] = m_nSetDutyLevel[i];
	}
	output->lastUpdateTime = static_cast<int>(InterlockedCompareExchange(
		reinterpret_cast<volatile LONG*>(const_cast<int*>(&m_nLastUpdateTime)), 0, 0));
	output->forcedCooling = m_bForcedCooling;
	output->forceCoolingCompletionSequence = InterlockedCompareExchange(
		const_cast<volatile LONG*>(&m_nForceCoolingCompletionSequence), 0, 0);
	output->gpuAvailable = m_bFanAvailable[1] && output->ecReady;
	LeaveCriticalSection(&m_csFanControl);
	return TRUE;
}

void CCore::SetUpdateRPM(BOOL enabled)
{
	EnterCriticalSection(&m_csFanControl);
	m_bUpdateRPM = enabled ? TRUE : FALSE;
	LeaveCriticalSection(&m_csFanControl);
}

void CCore::SetForcedCooling(BOOL enabled)
{
	EnterCriticalSection(&m_csFanControl);
	m_bForcedCooling = enabled ? TRUE : FALSE;
	LeaveCriticalSection(&m_csFanControl);
}

BOOL CCore::GetForcedCooling() const
{
	EnterCriticalSection(&m_csFanControl);
	const BOOL enabled = m_bForcedCooling;
	LeaveCriticalSection(&m_csFanControl);
	return enabled;
}

BOOL CCore::SetSoftControlTargets(const int* currentDuty, const int* targetDuty)
{
	if (currentDuty == NULL || targetDuty == NULL)
	{
		return FALSE;
	}

	EnterCriticalSection(&m_csFanControl);
	for (int i = 0; i < 2; ++i)
	{
		m_nSoftCurrentDuty[i] = currentDuty[i];
		m_nSoftTargetDuty[i] = targetDuty[i];
	}
	LeaveCriticalSection(&m_csFanControl);
	return TRUE;
}

void CCore::RequestExit(int state)
{
	InterlockedExchange(reinterpret_cast<volatile LONG*>(&m_nExit), static_cast<LONG>(state));
}

int CCore::GetExitState() const
{
	return static_cast<int>(InterlockedCompareExchange(
		reinterpret_cast<volatile LONG*>(const_cast<int*>(&m_nExit)), 0, 0));
}

void CALLBACK CCore::TimerCallback(UINT, UINT, DWORD_PTR dwUser, DWORD_PTR, DWORD_PTR)
{
	CCore* pCore = reinterpret_cast<CCore*>(dwUser);
	if (pCore != NULL && pCore->GetExitState() == 0)
	{
		InterlockedExchange(reinterpret_cast<volatile LONG*>(&pCore->m_bForcedRefresh), TRUE);
	}
}

DWORD WINAPI CCore::SoftControlThreadProc(LPVOID lpParam)
{
	CCore* pCore = reinterpret_cast<CCore*>(lpParam);
	if (pCore == NULL)
	{
		return 0;
	}

	while (pCore->GetExitState() == 0)
	{
		CConfig config;
		if (!pCore->GetConfigSnapshot(&config))
		{
			break;
		}
		if (config.SoftControl && config.TakeOver && !pCore->GetForcedCooling())
		{
			pCore->SoftControlDuty();
		}
		Sleep(100);
	}
	return 0;
}

void CCore::Run()
{
	RequestExit(0);
	CString warning;
	if (!LoadConfiguration(&warning))
	{
		TRACE0("Unable to apply the loaded configuration\n");
	}
	if (!warning.IsEmpty())
	{
		TRACE("Configuration warning: %s\n", warning.GetString());
	}

	if (!m_nInit)
	{
		Init();
	}
	SetParentDialog(m_pParentDlg);

	if (m_nInit == 1)
	{
		if (m_hSoftControlThread == NULL)
		{
			m_hSoftControlThread = CreateThread(NULL, 0, SoftControlThreadProc, this, 0, NULL);
			if (m_hSoftControlThread != NULL)
			{
				SetThreadPriority(m_hSoftControlThread, THREAD_PRIORITY_ABOVE_NORMAL);
			}
		}

		TIMECAPS caps;
		if (timeGetDevCaps(&caps, sizeof(caps)) == TIMERR_NOERROR)
		{
			CConfig config;
			if (!GetConfigSnapshot(&config))
			{
				config.LoadDefault();
			}
			UINT interval = static_cast<UINT>(config.UpdateInterval * 1000);
			interval = std::max(interval, static_cast<UINT>(caps.wPeriodMin));
			interval = std::min(interval, static_cast<UINT>(caps.wPeriodMax));

			m_nTimerID = timeSetEvent(
				interval,
				caps.wPeriodMin,
				TimerCallback,
				reinterpret_cast<DWORD_PTR>(this),
				TIME_PERIODIC | TIME_CALLBACK_FUNCTION);

			if (m_nTimerID != 0)
			{
				while (GetExitState() == 0)
				{
					if (InterlockedCompareExchange(
						reinterpret_cast<volatile LONG*>(&m_bForcedRefresh), 0, 0) != 0)
					{
						Work();
						InterlockedExchange(
							reinterpret_cast<volatile LONG*>(&m_nLastUpdateTime), GetTime());
						InterlockedExchange(
							reinterpret_cast<volatile LONG*>(&m_bForcedRefresh), FALSE);
					}
					Sleep(500);
				}
				timeKillEvent(m_nTimerID);
				m_nTimerID = 0;
			}
			else
			{
				RunOriginal();
			}
		}
		else
		{
			RunOriginal();
		}

		if (m_hSoftControlThread != NULL)
		{
			WaitForSingleObject(m_hSoftControlThread, INFINITE);
			CloseHandle(m_hSoftControlThread);
			m_hSoftControlThread = NULL;
		}
	}
	RequestExit(2);
}

void CCore::RunOriginal()
{
	int nextCheckTime = 0;
	BOOL prioritySet = FALSE;

	if (m_nInit == 1)
	{
		if (m_hSoftControlThread == NULL)
		{
			m_hSoftControlThread = CreateThread(NULL, 0, SoftControlThreadProc, this, 0, NULL);
			if (m_hSoftControlThread != NULL)
			{
				SetThreadPriority(m_hSoftControlThread, THREAD_PRIORITY_ABOVE_NORMAL);
			}
		}

		while (GetExitState() == 0)
		{
			const int currentTime = GetTime();
			if (currentTime >= nextCheckTime || InterlockedCompareExchange(
				reinterpret_cast<volatile LONG*>(&m_bForcedRefresh), 0, 0) != 0)
			{
				Work();
				InterlockedExchange(
					reinterpret_cast<volatile LONG*>(&m_nLastUpdateTime), currentTime);
				CConfig config;
				if (!GetConfigSnapshot(&config))
				{
					config.LoadDefault();
				}
				int interval = config.UpdateInterval;
				if (interval < 1)
				{
					interval = 1;
				}
				nextCheckTime = GetTime(NULL, interval);
				InterlockedExchange(
					reinterpret_cast<volatile LONG*>(&m_bForcedRefresh), FALSE);
				if (!prioritySet)
				{
					prioritySet = TRUE;
					SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
				}
			}
			Sleep(100);
		}

		if (m_hSoftControlThread != NULL)
		{
			WaitForSingleObject(m_hSoftControlThread, INFINITE);
			CloseHandle(m_hSoftControlThread);
			m_hSoftControlThread = NULL;
		}
	}
	RequestExit(2);
}

void CCore::Work()
{
	CConfig config;
	LONG configGeneration = 0;
	if (!GetConfigSnapshot(&config, &configGeneration))
	{
		return;
	}

	Update();
	BOOL configIsCurrent = FALSE;
	BOOL forcedCoolingActive = FALSE;
	BOOL forcedCoolingNeedsSet = FALSE;
	EnterCriticalSection(&m_csFanControl);
	configIsCurrent = IsCurrentConfigGeneration(configGeneration,
		InterlockedCompareExchange(&m_nConfigGeneration, 0, 0)) ? TRUE : FALSE;
	if (configIsCurrent && m_bForcedCooling)
	{
		if (!ShouldCompleteForcedCooling(m_bForcedCooling != FALSE,
			m_nCurTemp[0], m_nCurTemp[1], config.ForceTemp))
		{
			forcedCoolingActive = TRUE;
			if (m_nSetDuty[0] < 100 || m_nSetDuty[1] < 100)
			{
				m_nSetDuty[0] = 100;
				m_nSetDuty[1] = 100;
				m_nSetDutyLevel[0] = CurveLastLevel(config.CpuCurve);
				m_nSetDutyLevel[1] = CurveLastLevel(config.GpuCurve);
				for (int i = 0; i < 2; ++i)
				{
					m_nSoftTargetDuty[i] = m_nSetDuty[i];
					m_nSoftCurrentDuty[i] = m_nSetDuty[i];
				}
				forcedCoolingNeedsSet = TRUE;
			}
		}
		else
		{
			m_bForcedCooling = FALSE;
			InterlockedIncrement(&m_nForceCoolingCompletionSequence);
		}
	}
	LeaveCriticalSection(&m_csFanControl);
	if (!configIsCurrent)
	{
		return;
	}

	if (forcedCoolingActive)
	{
		if (forcedCoolingNeedsSet)
		{
			SetFanDuty();
		}
		return;
	}

	if (config.TakeOver)
	{
		Control(config);
	}
	else
	{
		ResetFan();
	}
}

void CCore::Update()
{
	if (m_hInstDLL == NULL || m_pfnGetTempFanDuty == NULL)
	{
		EnterCriticalSection(&m_csFanControl);
		m_bFanAvailable[0] = FALSE;
		m_bFanAvailable[1] = FALSE;
		LeaveCriticalSection(&m_csFanControl);
		return;
	}

	ECData data;
	int temperatureErrors = 0;
	for (int i = 0; i < 2; ++i)
	{
		data = m_pfnGetTempFanDuty(i + 1);
		EnterCriticalSection(&m_csFanControl);
		const int currentTemperature = m_nCurTemp[i];
		const BOOL updateRPM = m_bUpdateRPM;
		LeaveCriticalSection(&m_csFanControl);
		if (abs(static_cast<int>(data.Remote) - currentTemperature) > 30)
		{
			if (temperatureErrors++ == 0)
			{
				Sleep(1000);
				--i;
				continue;
			}
		}

		EnterCriticalSection(&m_csFanControl);
		m_nLastTemp[i] = m_nCurTemp[i];
		m_nCurTemp[i] = data.Remote;
		m_bFanAvailable[i] = TRUE;
		m_nCurDuty[i] = static_cast<int>(data.FanDuty * 100 / 255.0 + 0.5);
		if (updateRPM && m_pfnGetFANRPM[i] != NULL)
		{
			const int value = m_pfnGetFANRPM[i]();
			if (value == 0)
			{
				m_nCurRPM[i] = 0;
			}
			else if (value > 300 && value < 5000)
			{
				m_nCurRPM[i] = 2100000 / value;
			}
		}
		else
		{
			m_nCurRPM[i] = -1;
		}
		LeaveCriticalSection(&m_csFanControl);
		temperatureErrors = 0;
	}

}

void CCore::Control()
{
	CConfig config;
	if (GetConfigSnapshot(&config))
	{
		Control(config);
	}
}

void CCore::Control(const CConfig& config)
{
	if (config.Linear)
	{
		CalcLinearDuty(config);
	}
	else
	{
		CalcStdDuty(config);
	}

	if (config.SoftControl)
	{
		EnterCriticalSection(&m_csFanControl);
		for (int i = 0; i < 2; ++i)
		{
			m_nSoftTargetDuty[i] = m_nSetDuty[i];
			if (m_nSoftCurrentDuty[i] <= 0)
			{
				m_nSoftCurrentDuty[i] = m_nCurDuty[i];
			}
		}
		LeaveCriticalSection(&m_csFanControl);
	}
	else
	{
		SetFanDuty();
	}
}

void CCore::CalcLinearDuty()
{
	CConfig config;
	if (GetConfigSnapshot(&config))
	{
		CalcLinearDuty(config);
	}
}

void CCore::CalcLinearDuty(const CConfig& config)
{
	EnterCriticalSection(&m_csFanControl);
	int transition = config.TransitionTemp;
	if (transition < 0)
	{
		transition = 0;
	}

	for (int i = 0; i < 2; ++i)
	{
		const FanCurvePoints& curve = i == 0 ? config.CpuCurve : config.GpuCurve;
		if (curve.empty())
		{
			m_nSetDuty[i] = 0;
			m_nSetDutyLevel[i] = 0;
			continue;
		}

		int effectiveTemperature = m_nLinearTemperature[i];
		if (effectiveTemperature < m_nCurTemp[i])
		{
			effectiveTemperature = m_nCurTemp[i];
		}
		const int maximumEffectiveTemperature = m_nCurTemp[i] + transition;
		if (effectiveTemperature > maximumEffectiveTemperature)
		{
			effectiveTemperature = maximumEffectiveTemperature;
		}
		m_nLinearTemperature[i] = effectiveTemperature;
		m_nSetDuty[i] = EvaluateLinearDuty(curve, effectiveTemperature);
		m_nSetDutyLevel[i] = CurveLevelAtTemperature(curve, effectiveTemperature);
	}
	LeaveCriticalSection(&m_csFanControl);
}

void CCore::CalcStdDuty()
{
	CConfig config;
	if (GetConfigSnapshot(&config))
	{
		CalcStdDuty(config);
	}
}

void CCore::CalcStdDuty(const CConfig& config)
{
	EnterCriticalSection(&m_csFanControl);
	int transition = config.TransitionTemp;
	if (transition < 0)
	{
		transition = 0;
	}

	for (int i = 0; i < 2; ++i)
	{
		const FanCurvePoints& curve = i == 0 ? config.CpuCurve : config.GpuCurve;
		int selectedIndex = m_nSelectedCurvePoint[i];
		m_nSetDuty[i] = EvaluateStepDuty(curve, m_nCurTemp[i], transition, &selectedIndex);
		if (selectedIndex < 0)
		{
			selectedIndex = 0;
		}
		if (!curve.empty() && selectedIndex >= static_cast<int>(curve.size()))
		{
			selectedIndex = static_cast<int>(curve.size()) - 1;
		}
		m_nSelectedCurvePoint[i] = selectedIndex;
		m_nSetDutyLevel[i] = curve.empty() ? 0 : selectedIndex + 1;
	}
	LeaveCriticalSection(&m_csFanControl);
}

void CCore::ResetFan()
{
	EnterCriticalSection(&m_csFanControl);
	if (!m_bTakeOverStatus)
	{
		LeaveCriticalSection(&m_csFanControl);
		return;
	}

	for (int i = 0; i < 2; ++i)
	{
		m_nSetDuty[i] = 0;
		m_nSetDutyLevel[i] = 0;
		m_nSoftTargetDuty[i] = 0;
		m_nSoftCurrentDuty[i] = 0;
	}
	if (m_hInstDLL != NULL && m_pfnSetFANDutyAuto != NULL)
	{
		m_pfnSetFANDutyAuto(1);
		m_pfnSetFANDutyAuto(2);
		m_pfnSetFANDutyAuto(3);
	}
	m_bTakeOverStatus = FALSE;
	LeaveCriticalSection(&m_csFanControl);
}

void CCore::SetFanDuty()
{
	if (m_hInstDLL == NULL || m_pfnSetFanDuty == NULL)
	{
		return;
	}

	EnterCriticalSection(&m_csFanControl);
	for (int i = 0; i < 2; ++i)
	{
		if (m_nCurDuty[i] == m_nSetDuty[i])
		{
			continue;
		}
		int duty = m_nSetDuty[i];
		if (duty < 0)
		{
			duty = 0;
		}
		if (duty > 100)
		{
			duty = 100;
		}
		const int hardwareDuty = static_cast<int>(duty * 255.0 / 100.0 + 0.5);
		m_pfnSetFanDuty(i + 1, hardwareDuty);
		if (i == 1)
		{
			m_pfnSetFanDuty(i + 2, hardwareDuty);
		}
	}
	m_bTakeOverStatus = TRUE;
	LeaveCriticalSection(&m_csFanControl);
}

void CCore::SoftControlDuty()
{
	EnterCriticalSection(&m_csFanControl);
	BOOL changed = FALSE;
	for (int i = 0; i < 2; ++i)
	{
		if (m_nSoftCurrentDuty[i] != m_nSoftTargetDuty[i])
		{
			changed = TRUE;
			if (m_nSoftCurrentDuty[i] < m_nSoftTargetDuty[i])
			{
				++m_nSoftCurrentDuty[i];
			}
			else
			{
				--m_nSoftCurrentDuty[i];
			}
			m_nSetDuty[i] = m_nSoftCurrentDuty[i];
		}
	}
	LeaveCriticalSection(&m_csFanControl);

	if (changed)
	{
		SetFanDuty();
	}
}
