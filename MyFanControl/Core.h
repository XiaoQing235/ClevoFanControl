#pragma once

#include <mmsystem.h>
#pragma comment(lib, "winmm.lib")

#include "ConfigStore.h"
#include "FanConfig.h"

typedef FanConfig CConfig;

int GetTime(tm* pt = 0, int offset = 0);
int GetTimeInterval(int a, int b, int* p = 0);
CString GetExePath();

struct ECData
{
	byte Remote;
	byte Local;
	byte FanDuty;
	byte Reserve;
};

typedef BOOL(InitIo)(void);
typedef void(SetFanDuty)(int fan_id, int duty);
typedef int(SetFANDutyAuto)(int fan_id);
typedef ECData(GetTempFanDuty)(int fan_id);
typedef int(GetFANCounter)(void);
typedef const char* (GetECVersion)(void);
typedef int(GetFanRpm)(void);

struct CCoreStatusSnapshot
{
	BOOL ecReady;
	int currentTemperature[2];
	int currentDuty[2];
	int currentRPM[2];
	int targetDuty[2];
	int targetCurveLevel[2];
	int lastUpdateTime;
	BOOL forcedCooling;
	BOOL gpuAvailable;
};

class CCore
{
public:
	CCore();
	~CCore();

	void SetParentDialog(class CMyFanControlDlg* pDlg);
protected:
	InitIo* m_pfnInitIo;
	SetFanDuty* m_pfnSetFanDuty;
	SetFANDutyAuto* m_pfnSetFANDutyAuto;
	GetTempFanDuty* m_pfnGetTempFanDuty;
	GetFANCounter* m_pfnGetFANCounter;
	GetECVersion* m_pfnGetECVersion;
	GetFanRpm* m_pfnGetFANRPM[2];

public:
	BOOL m_nInit;
	int m_nExit;
	HINSTANCE m_hInstDLL;
	CConfig m_config;
	int m_nCurTemp[2];
	int m_nLastTemp[2];
	int m_nSetDuty[2];
	int m_nSetDutyLevel[2];
	int m_nLinearTemperature[2];
	int m_nSelectedCurvePoint[2];
	int m_nCurDuty[2];
	int m_nCurRPM[2];
	BOOL m_bFanAvailable[2];
	BOOL m_bUpdateRPM;
	int m_nLastUpdateTime;
	BOOL m_bForcedCooling;
	BOOL m_bTakeOverStatus;
	BOOL m_bForcedRefresh;

	int m_nSoftTargetDuty[2];
	int m_nSoftCurrentDuty[2];

	UINT m_nTimerID;
	static void CALLBACK TimerCallback(UINT uTimerID, UINT uMsg, DWORD_PTR dwUser, DWORD_PTR dw1, DWORD_PTR dw2);

	HANDLE m_hSoftControlThread;
	static DWORD WINAPI SoftControlThreadProc(LPVOID lpParam);
	mutable CRITICAL_SECTION m_csFanControl;
	mutable CRITICAL_SECTION m_csConfig;

	class CMyFanControlDlg* m_pParentDlg;

public:
	BOOL Init();
	void Uninit();
	BOOL LoadConfiguration(CString* warning = NULL);
	BOOL GetConfigSnapshot(CConfig* output);
	BOOL ApplyConfig(const CConfig& config);
	BOOL GetStatusSnapshot(CCoreStatusSnapshot* output) const;
	void SetUpdateRPM(BOOL enabled);
	void SetForcedCooling(BOOL enabled);
	BOOL GetForcedCooling() const;
	BOOL SetSoftControlTargets(const int* currentDuty, const int* targetDuty);
	void RequestExit(int state = 1);
	int GetExitState() const;
	void Run();
	void RunOriginal();
	void Work();
	void Update();
	void Control();
	void Control(const CConfig& config);
	void CalcLinearDuty();
	void CalcLinearDuty(const CConfig& config);
	void CalcStdDuty();
	void CalcStdDuty(const CConfig& config);
	void ResetFan();
	void SetFanDuty();
	void SoftControlDuty();

protected:
	void ClearEcApi();
	void ResetCurveState();
};
