#pragma once

#include "Core.h"
#include "FanCurveCtrl.h"
#include "PresetStore.h"
#include "afxcmn.h"
#include "afxwin.h"

#include <string>

class CClevoFanControlDlg : public CDialogEx
{
public:
	CClevoFanControlDlg(CWnd* pParent = NULL);
	~CClevoFanControlDlg();

	enum { IDD = IDD_CLEVOFANCONTROL_DIALOG };

	HICON m_hIcon;
	BOOL m_bForceHideWindow;
	CCore m_core;
	HANDLE m_hCoreThread;
	DWORD m_dwCoreThreadId;
	UINT_PTR m_nUiTimerId;
	int m_nLastCoreUpdateTime;
	BOOL m_bWindowVisible;
	BOOL m_bTrayAdded;
	BOOL m_bStartupPending;
	BOOL m_bShuttingDown;
	int m_dpiX;
	int m_dpiY;
	int m_uiFontDpiY;
	int m_uiFontPointSize;
	BOOL m_bSyncingControls;
	CFont m_uiFont;

	CListCtrl m_ctlStatus;
	CButton m_ctlTakeOver;
	CButton m_ctlForcedCooling;
	CButton m_ctlLinear;
	CButton m_ctlSoftControl;
	CButton m_ctlAutorun;
	CEdit m_ctlInterval;
	CEdit m_ctlTransition;
	CEdit m_ctlForceTemp;
	CEdit m_fontSize;
	CSpinButtonCtrl m_fontSizeSpin;
	CComboBox m_curveSelector;
	CComboBox m_closeBehavior;
	CButton m_autoSwitch;
	CStatic m_activePresetStatus;
	CEdit m_nodeIndex;
	CEdit m_nodeTemperature;
	CEdit m_nodeDuty;
	CFanCurveCtrl m_cpuCurveCtrl;
	CFanCurveCtrl m_gpuCurveCtrl;

	CConfig m_draft;
	FanConfig m_globalConfig;
	PresetCollection m_presets;
	int m_nActivePreset;
	ULONGLONG m_lastPresetScanTick;
	BOOL m_bPresetStoreDirty;
	BOOL m_bDraftDirty;
	BOOL m_bSavedAutoRun;
	int m_nSelectedCurve;

	static unsigned __stdcall CoreThread(void* lParam);

protected:
	virtual void DoDataExchange(CDataExchange* pDX);
	virtual BOOL OnInitDialog();
	virtual void OnOK();
	virtual void OnCancel();

	afx_msg void OnSysCommand(UINT nID, LPARAM lParam);
	afx_msg void OnPaint();
	afx_msg HCURSOR OnQueryDragIcon();
	afx_msg void OnWindowPosChanging(WINDOWPOS* lpwndpos);
	afx_msg void OnTimer(UINT_PTR nIDEvent);
	afx_msg void OnSize(UINT nType, int cx, int cy);
	afx_msg void OnGetMinMaxInfo(MINMAXINFO* lpMMI);

	afx_msg void OnBnClickedButtonSave();
	afx_msg void OnBnClickedButtonReset();
	afx_msg void OnBnClickedButtonLoad();
	afx_msg void OnBnClickedCheckTakeover();
	afx_msg void OnBnClickedCheckForce();
	afx_msg void OnBnClickedCheckLinear();
	afx_msg void OnBnClickedCheckSoftControl();
	afx_msg void OnBnClickedCheckAutorun();
	afx_msg void OnBnClickedAutoSwitch();
	afx_msg void OnBnClickedManagePresets();
	afx_msg void OnBnClickedCurveAdd();
	afx_msg void OnBnClickedCurveDelete();
	afx_msg void OnBnClickedCurveReset();
	afx_msg void OnCbnSelchangeCurve();
	afx_msg void OnCbnSelchangeCloseBehavior();
	afx_msg void OnConfigurationEditChanged();
	afx_msg void OnFontSizeChanged();
	afx_msg void OnNodeEditKillFocus();
	afx_msg LRESULT OnFanCurveChanged(WPARAM wParam, LPARAM lParam);
	afx_msg LRESULT OnShowTask(WPARAM wParam, LPARAM lParam);
	afx_msg LRESULT OnTaskbarCreated(WPARAM wParam, LPARAM lParam);

	DECLARE_MESSAGE_MAP()

	void StopCoreThread();
	void LayoutControls(int cx, int cy);
	void MoveControl(int id, int left, int top, int width, int height, BOOL show = TRUE);
	void InitializeStatusColumns();
	void ResizeStatusColumns(int listWidth);
	void UpdateDpi();
	void ApplyUIFont();
	int MeasureUiTextWidth(LPCTSTR text) const;
	int GetUiTextHeight() const;
	void SetInitialWindowSize();
	int ScaleX(int value) const;
	int ScaleY(int value) const;
	void RefreshStatus();
	void RefreshCurves();
	void LoadDraftFromCore();
	BOOL LoadDraftFromFile(BOOL showWarning);
	BOOL LoadPresetCollection(BOOL showWarning);
	void SyncDraftToControls();
	void SyncPresetControls();
	void UpdateActivePresetStatus();
	void SyncSelectedNodeToControls();
	void SetDraftDirty(BOOL dirty);
	BOOL ApplyNodeEditor(BOOL showError);
	BOOL ReadDraftFromControls();
	BOOL ValidateDraft(CString* error) const;
	BOOL SaveDraft();
	BOOL SavePresetCollection(const PresetCollection& collection, CString* error);
	BOOL ApplyPresetAt(int index, BOOL showError, BOOL automatic);
	BOOL ApplyGlobalConfiguration(BOOL showError = TRUE);
	void ScanPresetProcesses();

	FanCurvePoints* SelectedCurve();
	const FanCurvePoints* SelectedCurve() const;
	std::string ConfigurationPath() const;
	std::string PresetConfigurationPath() const;
	void SetEditInteger(CEdit& edit, int value);
	BOOL ParseEditInteger(const CEdit& edit, int* value) const;

	void SetTray(PCSTR string);
	void ShowWindowFromTray();
	void HideWindowToTray();
	BOOL SaveBeforeClose();
	BOOL SetAutorunReg(BOOL bWrite = FALSE, BOOL bAutorun = FALSE);
	BOOL SetAutorunTask(BOOL bWrite = FALSE, BOOL bAutorun = FALSE);
	CString ExecuteCmd(CString str);
	BOOL CreateTaskXml(PCSTR strXmlPath, PCSTR strTargetPath);
};
