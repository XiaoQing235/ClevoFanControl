#include "stdafx.h"
#include "ClevoFanControl.h"
#include "ClevoFanControlDlg.h"
#include "PresetMatcher.h"
#include "PresetManagerDlg.h"
#include "afxdialogex.h"

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <process.h>

#ifdef max
#undef max
#endif
#ifdef min
#undef min
#endif

static UINT WM_TASKBARCREATED = ::RegisterWindowMessage(_T("TaskbarCreated"));

#ifdef _DEBUG
#define new DEBUG_NEW
#endif

#define WM_SHOWTASK (WM_USER + 1)
#define IDR_SHOW 11
#define IDR_EXIT 12

namespace
{
const UINT_PTR kUiTimerId = 1;

int CALLBACK FontFamilyProbe(const LOGFONT*, const TEXTMETRIC*, DWORD, LPARAM lParam)
{
	BOOL* found = reinterpret_cast<BOOL*>(lParam);
	if (found != NULL)
	{
		*found = TRUE;
	}
	return 0;
}

BOOL IsFontFamilyAvailable(LPCTSTR faceName)
{
	if (faceName == NULL || faceName[0] == _T('\0'))
	{
		return FALSE;
	}
	HDC dc = ::GetDC(NULL);
	if (dc == NULL)
	{
		return FALSE;
	}
	LOGFONT logFont = {};
	_tcsncpy_s(logFont.lfFaceName, _countof(logFont.lfFaceName), faceName, _TRUNCATE);
	BOOL found = FALSE;
	::EnumFontFamiliesEx(
		dc,
		&logFont,
		FontFamilyProbe,
		reinterpret_cast<LPARAM>(&found),
		0);
	::ReleaseDC(NULL, dc);
	return found;
}
}

namespace
{
class CAboutDlg : public CDialogEx
{
public:
	CAboutDlg() : CDialogEx(CAboutDlg::IDD) {}
	enum { IDD = IDD_ABOUTBOX };

protected:
	virtual void DoDataExchange(CDataExchange* pDX)
	{
		CDialogEx::DoDataExchange(pDX);
	}
	DECLARE_MESSAGE_MAP()
};

BEGIN_MESSAGE_MAP(CAboutDlg, CDialogEx)
END_MESSAGE_MAP()

CString DiagnosticText(const std::string& diagnostic)
{
	return CString(CStringA(diagnostic.c_str()));
}

bool SameCurve(const FanCurvePoints& left, const FanCurvePoints& right)
{
	if (left.size() != right.size())
	{
		return false;
	}
	for (size_t i = 0; i < left.size(); ++i)
	{
		if (left[i].temperature != right[i].temperature || left[i].duty != right[i].duty)
		{
			return false;
		}
	}
	return true;
}
}

CClevoFanControlDlg::CClevoFanControlDlg(CWnd* pParent)
	: CDialogEx(CClevoFanControlDlg::IDD, pParent)
{
	m_hIcon = AfxGetApp()->LoadIcon(IDR_MAINFRAME);
	m_bForceHideWindow = TRUE;
	m_hCoreThread = NULL;
	m_dwCoreThreadId = 0;
	m_nUiTimerId = 0;
	m_nLastCoreUpdateTime = -1;
	m_bWindowVisible = FALSE;
	m_bTrayAdded = FALSE;
	m_bStartupPending = TRUE;
	m_bShuttingDown = FALSE;
	m_dpiX = 96;
	m_dpiY = 96;
	m_uiFontDpiY = 0;
	m_uiFontPointSize = 0;
	m_bSyncingControls = FALSE;
	m_nActivePreset = -1;
	m_lastPresetScanTick = 0;
	m_bPresetStoreDirty = FALSE;
	m_bDraftDirty = FALSE;
	m_bSavedAutoRun = FALSE;
	m_nSelectedCurve = 0;
}

CClevoFanControlDlg::~CClevoFanControlDlg()
{
	StopCoreThread();
	if (m_hWnd != NULL && ::IsWindow(m_hWnd))
	{
		DestroyWindow();
	}
}

void CClevoFanControlDlg::DoDataExchange(CDataExchange* pDX)
{
	CDialogEx::DoDataExchange(pDX);
	DDX_Control(pDX, IDC_LIST_STATUS, m_ctlStatus);
	DDX_Control(pDX, IDC_CHECK_TAKEOVER, m_ctlTakeOver);
	DDX_Control(pDX, IDC_CHECK_FORCE, m_ctlForcedCooling);
	DDX_Control(pDX, IDC_CHECK_LINEAR, m_ctlLinear);
	DDX_Control(pDX, IDC_CHECK_SOFTCONTROL, m_ctlSoftControl);
	DDX_Control(pDX, IDC_CHECK_AUTORUN, m_ctlAutorun);
	DDX_Control(pDX, IDC_EDIT_INTERVAL, m_ctlInterval);
	DDX_Control(pDX, IDC_EDIT_TREANSITION, m_ctlTransition);
	DDX_Control(pDX, IDC_EDIT_FORCE_TEMP, m_ctlForceTemp);
	DDX_Control(pDX, IDC_EDIT_FONT_SIZE, m_fontSize);
	DDX_Control(pDX, IDC_SPIN_FONT_SIZE, m_fontSizeSpin);
	DDX_Control(pDX, IDC_COMBO_CURVE, m_curveSelector);
	DDX_Control(pDX, IDC_COMBO_CLOSE_BEHAVIOR, m_closeBehavior);
	DDX_Control(pDX, IDC_CHECK_AUTO_SWITCH, m_autoSwitch);
	DDX_Control(pDX, IDC_STATIC_ACTIVE_PRESET, m_activePresetStatus);
	DDX_Control(pDX, IDC_EDIT_NODE_INDEX, m_nodeIndex);
	DDX_Control(pDX, IDC_EDIT_NODE_TEMP, m_nodeTemperature);
	DDX_Control(pDX, IDC_EDIT_NODE_DUTY, m_nodeDuty);
}

BEGIN_MESSAGE_MAP(CClevoFanControlDlg, CDialogEx)
	ON_WM_SYSCOMMAND()
	ON_WM_PAINT()
	ON_WM_QUERYDRAGICON()
	ON_WM_WINDOWPOSCHANGING()
	ON_WM_TIMER()
	ON_WM_SIZE()
	ON_WM_GETMINMAXINFO()
	ON_BN_CLICKED(IDC_BUTTON_SAVE, &CClevoFanControlDlg::OnBnClickedButtonSave)
	ON_BN_CLICKED(IDC_BUTTON_RESET, &CClevoFanControlDlg::OnBnClickedButtonReset)
	ON_BN_CLICKED(IDC_BUTTON_LOAD, &CClevoFanControlDlg::OnBnClickedButtonLoad)
	ON_BN_CLICKED(IDC_CHECK_TAKEOVER, &CClevoFanControlDlg::OnBnClickedCheckTakeover)
	ON_BN_CLICKED(IDC_CHECK_FORCE, &CClevoFanControlDlg::OnBnClickedCheckForce)
	ON_BN_CLICKED(IDC_CHECK_LINEAR, &CClevoFanControlDlg::OnBnClickedCheckLinear)
	ON_BN_CLICKED(IDC_CHECK_SOFTCONTROL, &CClevoFanControlDlg::OnBnClickedCheckSoftControl)
	ON_BN_CLICKED(IDC_CHECK_AUTORUN, &CClevoFanControlDlg::OnBnClickedCheckAutorun)
	ON_BN_CLICKED(IDC_CHECK_AUTO_SWITCH, &CClevoFanControlDlg::OnBnClickedAutoSwitch)
	ON_BN_CLICKED(IDC_BUTTON_MANAGE_PRESETS, &CClevoFanControlDlg::OnBnClickedManagePresets)
	ON_BN_CLICKED(IDC_BUTTON_CURVE_ADD, &CClevoFanControlDlg::OnBnClickedCurveAdd)
	ON_BN_CLICKED(IDC_BUTTON_CURVE_DELETE, &CClevoFanControlDlg::OnBnClickedCurveDelete)
	ON_BN_CLICKED(IDC_BUTTON_CURVE_RESET, &CClevoFanControlDlg::OnBnClickedCurveReset)
	ON_CBN_SELCHANGE(IDC_COMBO_CURVE, &CClevoFanControlDlg::OnCbnSelchangeCurve)
	ON_CBN_SELCHANGE(IDC_COMBO_CLOSE_BEHAVIOR, &CClevoFanControlDlg::OnCbnSelchangeCloseBehavior)
	ON_CONTROL(EN_CHANGE, IDC_EDIT_INTERVAL, &CClevoFanControlDlg::OnConfigurationEditChanged)
	ON_CONTROL(EN_CHANGE, IDC_EDIT_TREANSITION, &CClevoFanControlDlg::OnConfigurationEditChanged)
	ON_CONTROL(EN_CHANGE, IDC_EDIT_FORCE_TEMP, &CClevoFanControlDlg::OnConfigurationEditChanged)
	ON_CONTROL(EN_CHANGE, IDC_EDIT_FONT_SIZE, &CClevoFanControlDlg::OnFontSizeChanged)
	ON_CONTROL(EN_KILLFOCUS, IDC_EDIT_NODE_INDEX, &CClevoFanControlDlg::OnNodeEditKillFocus)
	ON_CONTROL(EN_KILLFOCUS, IDC_EDIT_NODE_TEMP, &CClevoFanControlDlg::OnNodeEditKillFocus)
	ON_CONTROL(EN_KILLFOCUS, IDC_EDIT_NODE_DUTY, &CClevoFanControlDlg::OnNodeEditKillFocus)
	ON_MESSAGE(WM_FAN_CURVE_CHANGED, &CClevoFanControlDlg::OnFanCurveChanged)
	ON_MESSAGE(WM_SHOWTASK, &CClevoFanControlDlg::OnShowTask)
	ON_REGISTERED_MESSAGE(WM_TASKBARCREATED, &CClevoFanControlDlg::OnTaskbarCreated)
END_MESSAGE_MAP()

BOOL CClevoFanControlDlg::OnInitDialog()
{
	CDialogEx::OnInitDialog();
	UpdateDpi();
	if (m_fontSizeSpin.GetSafeHwnd() != NULL)
	{
		m_fontSizeSpin.SetRange(FAN_UI_FONT_SIZE_MIN, FAN_UI_FONT_SIZE_MAX);
		if (m_fontSize.GetSafeHwnd() != NULL)
		{
			m_fontSizeSpin.SetBuddy(&m_fontSize);
		}
	}

	ASSERT((IDM_ABOUTBOX & 0xFFF0) == IDM_ABOUTBOX);
	ASSERT(IDM_ABOUTBOX < 0xF000);
	CMenu* pSysMenu = GetSystemMenu(FALSE);
	if (pSysMenu != NULL)
	{
		CString about;
		if (about.LoadString(IDS_ABOUTBOX) && !about.IsEmpty())
		{
			pSysMenu->AppendMenu(MF_SEPARATOR);
			pSysMenu->AppendMenu(MF_STRING, IDM_ABOUTBOX, about);
		}
	}

	SetIcon(m_hIcon, TRUE);
	SetIcon(m_hIcon, FALSE);
	LoadDraftFromFile(TRUE);
	LoadPresetCollection(TRUE);
	// Read the persisted draft here; querying Task Scheduler would block the UI thread during startup.
	m_bSavedAutoRun = m_draft.AutoRun ? TRUE : FALSE;
	m_bDraftDirty = FALSE;

	CRect rect;
	CWnd* placeholder = GetDlgItem(IDC_STATIC_CPU_CURVE);
	if (placeholder != NULL)
	{
		placeholder->GetWindowRect(&rect);
		ScreenToClient(&rect);
		m_cpuCurveCtrl.Create(WS_CHILD | WS_VISIBLE | WS_TABSTOP, rect, this, IDC_CURVE_CPU);
		placeholder->ShowWindow(SW_HIDE);
	}
	placeholder = GetDlgItem(IDC_STATIC_GPU_CURVE);
	if (placeholder != NULL)
	{
		placeholder->GetWindowRect(&rect);
		ScreenToClient(&rect);
		m_gpuCurveCtrl.Create(WS_CHILD | WS_VISIBLE | WS_TABSTOP, rect, this, IDC_CURVE_GPU);
		placeholder->ShowWindow(SW_HIDE);
	}
	m_cpuCurveCtrl.SetCurveId(0);
	m_gpuCurveCtrl.SetCurveId(1);
	ApplyUIFont();

	m_curveSelector.AddString(_T("CPU"));
	m_curveSelector.AddString(_T("GPU"));
	m_curveSelector.SendMessage(CB_SETMINVISIBLE, 2, 0);
	m_closeBehavior.AddString(_T("Exit program"));
	m_closeBehavior.AddString(_T("Minimize to tray"));
	m_closeBehavior.SendMessage(CB_SETMINVISIBLE, 2, 0);
	InitializeStatusColumns();
	SyncDraftToControls();
	SyncPresetControls();

	m_core.SetParentDialog(this);
	m_core.SetUpdateRPM(TRUE);
	unsigned threadId = 0;
	m_hCoreThread = reinterpret_cast<HANDLE>(_beginthreadex(
		NULL, 0, &CClevoFanControlDlg::CoreThread, this, 0, &threadId));
	m_dwCoreThreadId = static_cast<DWORD>(threadId);
	if (m_hCoreThread == NULL)
	{
		const int crtError = errno;
		const DWORD win32Error = GetLastError();
		CString message;
		message.Format(_T("Unable to start the fan-control worker thread. (CRT error %d, Win32 error %lu)"),
			crtError, static_cast<unsigned long>(win32Error));
		AfxMessageBox(message, MB_ICONERROR);
	}
	m_nUiTimerId = SetTimer(kUiTimerId, 100, NULL);
	SetTray("ClevoFanControl");
	SetInitialWindowSize();
	CRect initialClientRect;
	GetClientRect(&initialClientRect);
	LayoutControls(initialClientRect.Width(), initialClientRect.Height());
	return TRUE;
}

void CClevoFanControlDlg::OnSysCommand(UINT nID, LPARAM lParam)
{
	if ((nID & 0xFFF0) == SC_CLOSE)
	{
		if (m_draft.CloseToTray)
		{
			HideWindowToTray();
		}
		else
		{
			OnOK();
		}
	}
	else if ((nID & 0xFFF0) == IDM_ABOUTBOX)
	{
		CAboutDlg dlgAbout;
		dlgAbout.DoModal();
	}
	else
	{
		CDialogEx::OnSysCommand(nID, lParam);
	}
}

void CClevoFanControlDlg::OnPaint()
{
	if (IsIconic())
	{
		CPaintDC dc(this);
		SendMessage(WM_ICONERASEBKGND, reinterpret_cast<WPARAM>(dc.GetSafeHdc()), 0);
		CRect rect;
		GetClientRect(&rect);
		const int cxIcon = GetSystemMetrics(SM_CXICON);
		const int cyIcon = GetSystemMetrics(SM_CYICON);
		dc.DrawIcon((rect.Width() - cxIcon + 1) / 2, (rect.Height() - cyIcon + 1) / 2, m_hIcon);
	}
	else
	{
		CDialogEx::OnPaint();
	}
}

HCURSOR CClevoFanControlDlg::OnQueryDragIcon()
{
	return static_cast<HCURSOR>(m_hIcon);
}

unsigned __stdcall CClevoFanControlDlg::CoreThread(void* lParam)
{
	CClevoFanControlDlg* dialog = reinterpret_cast<CClevoFanControlDlg*>(lParam);
	if (dialog != NULL)
	{
		dialog->m_core.Run();
	}
	return 0;
}

void CClevoFanControlDlg::OnWindowPosChanging(WINDOWPOS* lpwndpos)
{
	if (m_bForceHideWindow)
	{
		lpwndpos->flags &= ~SWP_SHOWWINDOW;
	}
	CDialogEx::OnWindowPosChanging(lpwndpos);
}

void CClevoFanControlDlg::OnGetMinMaxInfo(MINMAXINFO* lpMMI)
{
	CDialogEx::OnGetMinMaxInfo(lpMMI);
	lpMMI->ptMinTrackSize.x = ScaleX(860);

	const int margin = ScaleY(14);
	const int gap = ScaleY(12);
	const int textHeight = (std::max)(ScaleY(16), GetUiTextHeight());
	const int labelHeight = (std::max)(ScaleY(18), textHeight + ScaleY(2));
	const int editHeight = (std::max)(ScaleY(22), textHeight + ScaleY(6));
	const int buttonHeight = (std::max)(ScaleY(28), textHeight + ScaleY(10));

	int statusHeaderHeight = ScaleY(20);
	CHeaderCtrl* statusHeader = m_ctlStatus.GetHeaderCtrl();
	if (statusHeader != NULL && statusHeader->GetSafeHwnd() != NULL)
	{
		CRect headerRect;
		statusHeader->GetClientRect(&headerRect);
		statusHeaderHeight = (std::max)(statusHeaderHeight, headerRect.Height());
	}
	int statusItemHeight = (std::max)(ScaleY(20), textHeight + ScaleY(4));
	CRect statusItemRect;
	if (m_ctlStatus.GetItemCount() > 0 &&
		m_ctlStatus.GetItemRect(0, &statusItemRect, LVIR_BOUNDS))
	{
		statusItemHeight = (std::max)(statusItemHeight, statusItemRect.Height());
	}
	const int statusHeight = (std::max)(ScaleY(154),
		ScaleY(20) + statusHeaderHeight + statusItemHeight * 5 + ScaleY(8) + ScaleY(4));

	const int presetsHeight = (std::max)(ScaleY(92),
		ScaleY(20) + (std::max)(std::max(ScaleY(20), textHeight + ScaleY(2)), buttonHeight) +
			ScaleY(6) + labelHeight + ScaleY(8));
	const int nodeGroupHeight = (std::max)(ScaleY(62),
		ScaleY(46) + editHeight * 2 + buttonHeight);

	const int checkHeight = (std::max)(ScaleY(20), textHeight + ScaleY(2));
	const int checkRowGap = ScaleY(8);
	const int settingsTopOffset = ScaleY(22);
	const int checksTopOffset = settingsTopOffset + editHeight * 2 + ScaleY(6) + ScaleY(8);
	const int closeTopOffset = checksTopOffset + checkHeight * 3 + checkRowGap * 2 + ScaleY(10);
	const int fontTopOffset = closeTopOffset + editHeight + ScaleY(6);
	const int settingsHeight = (std::max)(ScaleY(208),
		fontTopOffset + editHeight + ScaleY(12));
	const int requiredClientHeight = margin + statusHeight + gap + presetsHeight + gap +
		nodeGroupHeight + ScaleY(8) + settingsHeight + ScaleY(8) +
		buttonHeight + ScaleY(14);
	lpMMI->ptMinTrackSize.y = (std::max)(ScaleY(640), requiredClientHeight);
}

void CClevoFanControlDlg::OnSize(UINT nType, int cx, int cy)
{
	CDialogEx::OnSize(nType, cx, cy);
	if (nType == SIZE_MINIMIZED && m_draft.CloseToTray && !m_bShuttingDown)
	{
		HideWindowToTray();
		return;
	}
	UpdateDpi();
	ApplyUIFont();
	LayoutControls(cx, cy);
}

void CClevoFanControlDlg::LayoutControls(int cx, int cy)
{
	if (cx <= 0 || cy <= 0 || m_ctlStatus.GetSafeHwnd() == NULL)
	{
		return;
	}

	const int margin = ScaleX(14);
	const int gap = ScaleX(12);
	const int availableWidth = std::max(1, cx - margin * 2 - gap);
	const int textHeight = std::max(ScaleY(16), GetUiTextHeight());
	const int labelHeight = std::max(ScaleY(18), textHeight + ScaleY(2));
	const int editHeight = std::max(ScaleY(22), textHeight + ScaleY(6));
	const int buttonHeight = std::max(ScaleY(28), textHeight + ScaleY(10));
	const int contentPadding = ScaleX(12);
	const int labelGap = ScaleX(6);
	const int columnGap = ScaleX(12);
	const auto fitLabelWidth = [this](LPCTSTR text) {
		return MeasureUiTextWidth(text) + ScaleX(4);
	};
	const auto fitButtonWidth = [this](LPCTSTR text, int baseWidth) {
		return (std::max)(ScaleX(baseWidth), MeasureUiTextWidth(text) + ScaleX(18));
	};

	const int nodeIndexLabelWidth = fitLabelWidth(_T("Index"));
	const int nodeTemperatureLabelWidth = fitLabelWidth(_T("Temperature"));
	const int nodeDutyLabelWidth = fitLabelWidth(_T("Duty %"));
	const int nodeFieldWidth = (std::max)(ScaleX(44),
		MeasureUiTextWidth(_T("100")) + ScaleX(14));
	const int nodeContentWidth = contentPadding * 2 +
		nodeIndexLabelWidth + labelGap + nodeFieldWidth + labelGap +
		nodeTemperatureLabelWidth + labelGap + nodeFieldWidth + labelGap +
		nodeDutyLabelWidth + labelGap + nodeFieldWidth;

	const int curveLabelWidth = fitLabelWidth(_T("Curve"));
	const int curveAddWidth = fitButtonWidth(_T("Add point"), 92);
	const int curveDeleteWidth = fitButtonWidth(_T("Delete point"), 108);
	const int curveResetWidth = fitButtonWidth(_T("Reset curve"), 108);
	const int curveButtonGap = ScaleX(8);
	const int curveButtonsWidth = contentPadding * 2 + curveAddWidth + curveButtonGap +
		curveDeleteWidth + curveButtonGap + curveResetWidth;

	const int intervalLabelWidth = fitLabelWidth(_T("Update interval"));
	const int transitionLabelWidth = fitLabelWidth(_T("Transition temp"));
	const int forceLabelWidth = fitLabelWidth(_T("Force temp"));
	const int closeLabelWidth = fitLabelWidth(_T("Close button behavior"));
	const int fontLabelWidth = fitLabelWidth(_T("Font size"));
	const int fontEditWidth = (std::max)(ScaleX(44),
		MeasureUiTextWidth(_T("16")) + ScaleX(14));
	const int fontSpinWidth = ScaleX(12);
	const int intervalColumnWidth = intervalLabelWidth + labelGap + ScaleX(44);
	const int transitionColumnWidth = transitionLabelWidth + labelGap + ScaleX(44);
	const int settingsColumnsWidth = contentPadding * 2 + intervalColumnWidth +
		columnGap + transitionColumnWidth;
	const int closeComboMinimumWidth = ScaleX(180);
	const int closeRowWidth = contentPadding * 2 + closeLabelWidth + labelGap + closeComboMinimumWidth;
	const int fontRowWidth = contentPadding * 2 + fontLabelWidth + labelGap +
		fontEditWidth + fontSpinWidth;

	const int takeOverWidth = MeasureUiTextWidth(_T("Take over")) + ScaleX(28);
	const int forceCoolingWidth = MeasureUiTextWidth(_T("Force cooling")) + ScaleX(28);
	const int linearWidth = MeasureUiTextWidth(_T("Linear mode")) + ScaleX(28);
	const int softControlWidth = MeasureUiTextWidth(_T("Soft control")) + ScaleX(28);
	const int autorunWidth = MeasureUiTextWidth(_T("Run at startup")) + ScaleX(28);
	const int checkColumnWidth = (std::max)((std::max)(takeOverWidth, linearWidth), autorunWidth);
	const int secondCheckColumnWidth = (std::max)(forceCoolingWidth, softControlWidth);
	const int checksWidth = contentPadding * 2 + checkColumnWidth + labelGap + secondCheckColumnWidth;
	const int autoSwitchWidth = MeasureUiTextWidth(_T("Auto switch presets")) + ScaleX(28);
	const int managePresetsWidth = fitButtonWidth(_T("Manage presets..."), 152);
	const int presetsRowWidth = contentPadding * 2 + autoSwitchWidth + labelGap + managePresetsWidth;

	const int actionGap = ScaleX(6);
	const int actionButtonMinimumWidth = (std::max)(
		(std::max)(MeasureUiTextWidth(_T("Load")), MeasureUiTextWidth(_T("Reset"))),
		(std::max)(MeasureUiTextWidth(_T("Save")), MeasureUiTextWidth(_T("Exit")))) + ScaleX(24);
	const int actionButtonsWidth = actionButtonMinimumWidth * 4 + actionGap * 3;
	const int desiredRightWidth = (std::max)(ScaleX(360),
		(std::max)((std::max)(nodeContentWidth, curveButtonsWidth),
			(std::max)((std::max)(settingsColumnsWidth, closeRowWidth),
				(std::max)(fontRowWidth,
				(std::max)((std::max)(checksWidth, actionButtonsWidth), presetsRowWidth)))));
	const int minimumLeftWidth = ScaleX(420);
	int rightWidth = (std::max)(desiredRightWidth, availableWidth / 3);
	if (availableWidth - rightWidth < minimumLeftWidth)
	{
		rightWidth = (std::max)(desiredRightWidth, availableWidth - minimumLeftWidth);
	}
	rightWidth = (std::min)(rightWidth, (std::max)(1, availableWidth - 1));
	const int leftWidth = std::max(1, availableWidth - rightWidth);
	const int rightLeft = margin + leftWidth + gap;
	const int titleHeight = labelHeight;
	const int graphGap = ScaleY(12);
	const int buttonBottom = ScaleY(14);
	const int buttonTop = std::max(margin, cy - buttonBottom - buttonHeight);
	const int graphBottom = std::max(margin, cy - ScaleY(14));
	const int graphHeight = std::max(ScaleY(180),
		(graphBottom - margin - ScaleY(8) - titleHeight * 2 - graphGap) / 2);
	const int firstGraphTop = margin + titleHeight;
	const int secondTitleTop = firstGraphTop + graphHeight + graphGap;
	const int secondGraphTop = secondTitleTop + titleHeight;

	MoveControl(IDC_STATIC_CPU_TITLE, margin, margin, leftWidth, titleHeight);
	MoveControl(IDC_STATIC_GPU_TITLE, margin, secondTitleTop, leftWidth, titleHeight);
	m_cpuCurveCtrl.MoveWindow(margin, firstGraphTop, leftWidth, graphHeight);
	m_gpuCurveCtrl.MoveWindow(margin, secondGraphTop, leftWidth, graphHeight);

	const int statusTop = margin;
	const int statusRowHeight = std::max(ScaleY(20), textHeight + ScaleY(4));
	int statusHeaderHeight = ScaleY(20);
	CHeaderCtrl* statusHeader = m_ctlStatus.GetHeaderCtrl();
	if (statusHeader != NULL && statusHeader->GetSafeHwnd() != NULL)
	{
		CRect statusHeaderRect;
		statusHeader->GetClientRect(&statusHeaderRect);
		statusHeaderHeight = (std::max)(statusHeaderHeight, statusHeaderRect.Height());
	}
	int statusItemHeight = statusRowHeight;
	CRect statusItemRect;
	if (m_ctlStatus.GetItemCount() > 0 &&
		m_ctlStatus.GetItemRect(0, &statusItemRect, LVIR_BOUNDS))
	{
		statusItemHeight = (std::max)(statusItemHeight, statusItemRect.Height());
	}
	const int statusGroupTopPadding = ScaleY(20);
	const int statusGroupBottomPadding = ScaleY(8);
	const int statusListContentHeight = statusHeaderHeight + statusItemHeight * 5;
	const int statusHeight = (std::max)(ScaleY(154),
		statusGroupTopPadding + statusListContentHeight + statusGroupBottomPadding + ScaleY(4));
	const int statusListTop = statusTop + statusGroupTopPadding;
	MoveControl(IDC_STATIC_STATUS_GROUP, rightLeft, statusTop, rightWidth, statusHeight);
	MoveControl(IDC_LIST_STATUS, rightLeft + ScaleX(8), statusListTop,
		rightWidth - ScaleX(16), statusHeight - statusGroupTopPadding - statusGroupBottomPadding);
	CRect statusClientRect;
	m_ctlStatus.GetClientRect(&statusClientRect);
	ResizeStatusColumns(statusClientRect.Width());

	const int presetsTop = statusTop + statusHeight + ScaleY(12);
	const int presetsGroupTopPadding = ScaleY(20);
	const int presetsGroupBottomPadding = ScaleY(8);
	const int presetsRowGap = ScaleY(6);
	const int presetsCheckHeight = std::max(ScaleY(20), textHeight + ScaleY(2));
	const int presetsFirstRowTop = presetsTop + presetsGroupTopPadding;
	const int presetsSecondRowTop = presetsFirstRowTop +
		(std::max)(presetsCheckHeight, buttonHeight) + presetsRowGap;
	const int presetsHeight = (std::max)(ScaleY(92),
		presetsSecondRowTop + labelHeight + presetsGroupBottomPadding - presetsTop);
	const int presetsLeft = rightLeft + contentPadding;
	const int presetsButtonLeft = rightLeft + rightWidth - contentPadding - managePresetsWidth;
	const int presetsAutoWidth = (std::max)(1,
		presetsButtonLeft - presetsLeft - labelGap);
	MoveControl(IDC_STATIC_PRESETS_GROUP, rightLeft, presetsTop, rightWidth, presetsHeight);
	MoveControl(IDC_CHECK_AUTO_SWITCH, presetsLeft, presetsFirstRowTop,
		(std::min)(autoSwitchWidth, presetsAutoWidth), presetsCheckHeight);
	MoveControl(IDC_BUTTON_MANAGE_PRESETS, presetsButtonLeft, presetsFirstRowTop,
		managePresetsWidth, buttonHeight);
	MoveControl(IDC_STATIC_ACTIVE_PRESET, presetsLeft, presetsSecondRowTop,
		rightWidth - contentPadding * 2, labelHeight);

	const int editorTop = presetsTop + presetsHeight + ScaleY(12);
	const int nodeContentTop = editorTop + ScaleY(22);
	const int curveToolsTop = nodeContentTop + editHeight + ScaleY(8);
	const int curveButtonTop = curveToolsTop + editHeight + ScaleY(8);
	const int nodeGroupHeight = (std::max)(
		ScaleY(62),
		curveButtonTop + buttonHeight + ScaleY(8) - editorTop);
	int nodeLeft = rightLeft + contentPadding;
	MoveControl(IDC_STATIC_NODE_GROUP, rightLeft, editorTop, rightWidth, nodeGroupHeight);
	MoveControl(IDC_STATIC_NODE_INDEX_LABEL, nodeLeft, nodeContentTop,
		nodeIndexLabelWidth, labelHeight);
	nodeLeft += nodeIndexLabelWidth + labelGap;
	m_nodeIndex.MoveWindow(nodeLeft, nodeContentTop - ScaleY(3), nodeFieldWidth, editHeight);
	nodeLeft += nodeFieldWidth + labelGap;
	MoveControl(IDC_STATIC_NODE_TEMP_LABEL, nodeLeft, nodeContentTop,
		nodeTemperatureLabelWidth, labelHeight);
	nodeLeft += nodeTemperatureLabelWidth + labelGap;
	m_nodeTemperature.MoveWindow(nodeLeft, nodeContentTop - ScaleY(3), nodeFieldWidth, editHeight);
	nodeLeft += nodeFieldWidth + labelGap;
	MoveControl(IDC_STATIC_NODE_DUTY_LABEL, nodeLeft, nodeContentTop,
		nodeDutyLabelWidth, labelHeight);
	nodeLeft += nodeDutyLabelWidth + labelGap;
	m_nodeDuty.MoveWindow(nodeLeft, nodeContentTop - ScaleY(3), nodeFieldWidth, editHeight);

	const int curveLabelTop = curveToolsTop + (editHeight - labelHeight) / 2;
	const int curveComboLeft = rightLeft + contentPadding + curveLabelWidth + labelGap;
	const int curveComboWidth = (std::max)(ScaleX(120),
		rightWidth - (curveComboLeft - rightLeft) - contentPadding);
	MoveControl(IDC_STATIC_CURVE_LABEL, rightLeft + contentPadding, curveLabelTop,
		curveLabelWidth, labelHeight);
	m_curveSelector.MoveWindow(curveComboLeft, curveToolsTop, curveComboWidth, editHeight);
	m_curveSelector.SendMessage(CB_SETMINVISIBLE, 2, 0);
	m_curveSelector.SetDroppedWidth(curveComboWidth);
	const int curveButtonsLeft = rightLeft + contentPadding;
	MoveControl(IDC_BUTTON_CURVE_ADD, curveButtonsLeft, curveButtonTop, curveAddWidth, buttonHeight);
	MoveControl(IDC_BUTTON_CURVE_DELETE, curveButtonsLeft + curveAddWidth + curveButtonGap, curveButtonTop,
		curveDeleteWidth, buttonHeight);
	MoveControl(IDC_BUTTON_CURVE_RESET,
		curveButtonsLeft + curveAddWidth + curveButtonGap + curveDeleteWidth + curveButtonGap,
		curveButtonTop, curveResetWidth, buttonHeight);

	const int settingsTop = editorTop + nodeGroupHeight + ScaleY(8);
	const int settingsContentTop = settingsTop + ScaleY(22);
	const int rowGap = ScaleY(6);
	const int settingsRow2Top = settingsContentTop + editHeight + rowGap;
	const int checksTop = settingsRow2Top + editHeight + ScaleY(8);
	const int checkHeight = std::max(ScaleY(20), textHeight + ScaleY(2));
	const int checkRowGap = ScaleY(8);
	const int closeTop = checksTop + checkHeight * 3 + checkRowGap * 2 + ScaleY(10);
	const int fontTop = closeTop + editHeight + rowGap;
	const int settingsHeight = (std::max)(ScaleY(208),
		fontTop - settingsTop + editHeight + ScaleY(12));
	MoveControl(IDC_STATIC_SETTINGS_GROUP, rightLeft, settingsTop, rightWidth, settingsHeight);
	const int firstColumnLeft = rightLeft + contentPadding;
	const int firstFieldLeft = firstColumnLeft + intervalLabelWidth + labelGap;
	const int secondColumnLeft = firstFieldLeft + ScaleX(44) + columnGap;
	const int secondFieldLeft = secondColumnLeft + transitionLabelWidth + labelGap;
	MoveControl(IDC_STATIC_INTERVAL_LABEL, firstColumnLeft, settingsContentTop,
		intervalLabelWidth, labelHeight);
	m_ctlInterval.MoveWindow(firstFieldLeft, settingsContentTop - ScaleY(3), ScaleX(44), editHeight);
	MoveControl(IDC_STATIC_TRANSITION_LABEL, secondColumnLeft, settingsContentTop,
		transitionLabelWidth, labelHeight);
	m_ctlTransition.MoveWindow(secondFieldLeft, settingsContentTop - ScaleY(3), ScaleX(44), editHeight);
	MoveControl(IDC_STATIC_FORCE_LABEL, firstColumnLeft, settingsRow2Top,
		forceLabelWidth, labelHeight);
	m_ctlForceTemp.MoveWindow(firstColumnLeft + forceLabelWidth + labelGap,
		settingsRow2Top - ScaleY(3), ScaleX(44), editHeight);

	const int secondCheckLeft = firstColumnLeft + checkColumnWidth + labelGap;
	MoveControl(IDC_CHECK_TAKEOVER, firstColumnLeft, checksTop, checkColumnWidth, checkHeight);
	MoveControl(IDC_CHECK_FORCE, secondCheckLeft, checksTop, secondCheckColumnWidth, checkHeight);
	MoveControl(IDC_CHECK_LINEAR, firstColumnLeft, checksTop + checkHeight + checkRowGap,
		checkColumnWidth, checkHeight);
	MoveControl(IDC_CHECK_SOFTCONTROL, secondCheckLeft, checksTop + checkHeight + checkRowGap,
		secondCheckColumnWidth, checkHeight);
	MoveControl(IDC_CHECK_AUTORUN, firstColumnLeft, checksTop + (checkHeight + checkRowGap) * 2,
		checkColumnWidth, checkHeight);
	const int closeLabelTop = closeTop + (editHeight - labelHeight) / 2;
	const int closeComboLeft = firstColumnLeft + closeLabelWidth + labelGap;
	const int closeComboWidth = (std::max)(ScaleX(120),
		rightWidth - (closeComboLeft - rightLeft) - contentPadding);
	MoveControl(IDC_STATIC_CLOSE_BEHAVIOR_LABEL, firstColumnLeft, closeLabelTop,
		closeLabelWidth, labelHeight);
	m_closeBehavior.MoveWindow(closeComboLeft, closeTop, closeComboWidth, editHeight);
	m_closeBehavior.SendMessage(CB_SETMINVISIBLE, 2, 0);
	m_closeBehavior.SetDroppedWidth(closeComboWidth);
	const int fontLabelTop = fontTop + (editHeight - labelHeight) / 2;
	const int fontEditLeft = firstColumnLeft + fontLabelWidth + labelGap;
	const int fontSpinLeft = fontEditLeft + fontEditWidth;
	MoveControl(IDC_STATIC_FONT_SIZE_LABEL, firstColumnLeft, fontLabelTop,
		fontLabelWidth, labelHeight);
	m_fontSize.MoveWindow(fontEditLeft, fontTop - ScaleY(3), fontEditWidth, editHeight);
	m_fontSizeSpin.MoveWindow(fontSpinLeft, fontTop - ScaleY(3), fontSpinWidth, editHeight);

	const int actionTop = buttonTop;
	const int actionWidth = std::max(1, (rightWidth - actionGap * 3) / 4);
	const int actionsLeft = rightLeft;
	for (int i = 0; i < 4; ++i)
	{
		const int left = actionsLeft + i * (actionWidth + actionGap);
		MoveControl(i == 0 ? IDC_BUTTON_LOAD : i == 1 ? IDC_BUTTON_RESET : i == 2 ? IDC_BUTTON_SAVE : IDOK,
			left, actionTop, actionWidth, buttonHeight);
	}

	RedrawWindow(NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN | RDW_UPDATENOW);
}

void CClevoFanControlDlg::MoveControl(int id, int left, int top, int width, int height, BOOL show)
{
	CWnd* control = GetDlgItem(id);
	if (control == NULL || control->GetSafeHwnd() == NULL)
	{
		return;
	}
	CRect oldRect;
	control->GetWindowRect(&oldRect);
	ScreenToClient(&oldRect);
	const CRect newRect(left, top, left + std::max(1, width), top + std::max(1, height));
	control->MoveWindow(newRect, TRUE);
	control->ShowWindow(show ? SW_SHOW : SW_HIDE);
	CRect dirtyRect;
	dirtyRect.UnionRect(&oldRect, &newRect);
	InvalidateRect(&dirtyRect, TRUE);
}

void CClevoFanControlDlg::InitializeStatusColumns()
{
	if (m_ctlStatus.GetSafeHwnd() == NULL)
	{
		return;
	}
	m_ctlStatus.SetExtendedStyle(m_ctlStatus.GetExtendedStyle() | LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);
	while (m_ctlStatus.DeleteColumn(0)) {}
	m_ctlStatus.DeleteAllItems();
	m_ctlStatus.InsertColumn(0, _T("Metric"), LVCFMT_LEFT, 0);
	m_ctlStatus.InsertColumn(1, _T("CPU"), LVCFMT_CENTER, 0);
	m_ctlStatus.InsertColumn(2, _T("GPU"), LVCFMT_CENTER, 0);
	const LPCTSTR rows[] = { _T("Temperature"), _T("Curve node"), _T("Target duty"), _T("Current duty"), _T("Fan RPM") };
	for (int i = 0; i < 5; ++i)
	{
		m_ctlStatus.InsertItem(i, rows[i]);
	}
}

void CClevoFanControlDlg::ResizeStatusColumns(int listWidth)
{
	if (m_ctlStatus.GetSafeHwnd() == NULL || listWidth <= 0)
	{
		return;
	}

	const int metricWidth = std::max(1, listWidth * 42 / 100);
	const int fanWidth = std::max(1, (listWidth - metricWidth) / 2);
	m_ctlStatus.SetColumnWidth(0, metricWidth);
	m_ctlStatus.SetColumnWidth(1, fanWidth);
	m_ctlStatus.SetColumnWidth(2, std::max(1, listWidth - metricWidth - fanWidth));
}

void CClevoFanControlDlg::UpdateDpi()
{
	if (m_hWnd == NULL || !::IsWindow(m_hWnd))
	{
		return;
	}

	HDC dc = ::GetDC(m_hWnd);
	if (dc != NULL)
	{
		const int dpiX = GetDeviceCaps(dc, LOGPIXELSX);
		const int dpiY = GetDeviceCaps(dc, LOGPIXELSY);
		m_dpiX = dpiX > 0 ? dpiX : 96;
		m_dpiY = dpiY > 0 ? dpiY : 96;
		::ReleaseDC(m_hWnd, dc);
	}
}

void CClevoFanControlDlg::ApplyUIFont()
{
	if (m_hWnd == NULL || !::IsWindow(m_hWnd))
	{
		return;
	}

	int pointSize = m_draft.UiFontSize;
	if (pointSize < FAN_UI_FONT_SIZE_MIN || pointSize > FAN_UI_FONT_SIZE_MAX)
	{
		pointSize = FAN_UI_FONT_SIZE_DEFAULT;
	}

	if (m_uiFont.GetSafeHandle() == NULL || m_uiFontPointSize != pointSize || m_uiFontDpiY != m_dpiY)
	{
		LOGFONT logFont = {};
		HFONT defaultFont = static_cast<HFONT>(::GetStockObject(DEFAULT_GUI_FONT));
		if (defaultFont == NULL || ::GetObject(defaultFont, sizeof(logFont), &logFont) == 0)
		{
			return;
		}
		const LPCTSTR preferredFace = _T("Segoe UI Variable");
		const LPCTSTR fallbackFace = _T("Segoe UI");
		const LPCTSTR faceName = IsFontFamilyAvailable(preferredFace) ? preferredFace : fallbackFace;
		_tcsncpy_s(logFont.lfFaceName, _countof(logFont.lfFaceName), faceName, _TRUNCATE);
		logFont.lfHeight = -MulDiv(pointSize, m_dpiY, 72);
		logFont.lfWidth = 0;

		CFont replacement;
		if (!replacement.CreateFontIndirect(&logFont))
		{
			return;
		}
		HFONT oldFont = static_cast<HFONT>(m_uiFont.Detach());
		m_uiFont.Attach(replacement.Detach());
		if (oldFont != NULL)
		{
			::DeleteObject(oldFont);
		}
		m_uiFontDpiY = m_dpiY;
		m_uiFontPointSize = pointSize;
	}

	SetFont(&m_uiFont, TRUE);
	for (HWND child = ::GetWindow(m_hWnd, GW_CHILD);
		child != NULL;
		child = ::GetWindow(child, GW_HWNDNEXT))
	{
		::SendMessage(child, WM_SETFONT,
			reinterpret_cast<WPARAM>(m_uiFont.GetSafeHandle()), TRUE);
	}
}

int CClevoFanControlDlg::MeasureUiTextWidth(LPCTSTR text) const
{
	if (text == NULL || m_hWnd == NULL || !::IsWindow(m_hWnd))
	{
		return 0;
	}
	CClientDC dc(const_cast<CClevoFanControlDlg*>(this));
	CFont* oldFont = NULL;
	if (m_uiFont.GetSafeHandle() != NULL)
	{
		oldFont = dc.SelectObject(const_cast<CFont*>(&m_uiFont));
	}
	const int width = dc.GetTextExtent(text).cx;
	if (oldFont != NULL)
	{
		dc.SelectObject(oldFont);
	}
	return width;
}

int CClevoFanControlDlg::GetUiTextHeight() const
{
	if (m_hWnd == NULL || !::IsWindow(m_hWnd))
	{
		return 0;
	}
	CClientDC dc(const_cast<CClevoFanControlDlg*>(this));
	CFont* oldFont = NULL;
	if (m_uiFont.GetSafeHandle() != NULL)
	{
		oldFont = dc.SelectObject(const_cast<CFont*>(&m_uiFont));
	}
	TEXTMETRIC metrics = {};
	dc.GetTextMetrics(&metrics);
	if (oldFont != NULL)
	{
		dc.SelectObject(oldFont);
	}
	return metrics.tmHeight;
}

int CClevoFanControlDlg::ScaleX(int value) const
{
	return std::max(1, MulDiv(value, m_dpiX, 96));
}

int CClevoFanControlDlg::ScaleY(int value) const
{
	return std::max(1, MulDiv(value, m_dpiY, 96));
}

void CClevoFanControlDlg::SetInitialWindowSize()
{
	if (m_hWnd == NULL || !::IsWindow(m_hWnd))
	{
		return;
	}

	CRect workArea;
	if (!::SystemParametersInfo(SPI_GETWORKAREA, 0, &workArea, 0))
	{
		return;
	}

	const int preferredWindowWidth = ScaleX(1400);
	const int preferredWindowHeight = ScaleY(1000);
	const int availableWindowWidth = std::max(1, workArea.Width() - ScaleX(32));
	const int availableWindowHeight = std::max(1, workArea.Height() - ScaleY(48));
	const int windowWidth = std::min(preferredWindowWidth, availableWindowWidth);
	const int windowHeight = std::min(preferredWindowHeight, availableWindowHeight);
	const int left = workArea.left + std::max(0, (workArea.Width() - windowWidth) / 2);
	const int top = workArea.top + std::max(0, (workArea.Height() - windowHeight) / 2);

	SetWindowPos(NULL, left, top, windowWidth, windowHeight,
		SWP_NOZORDER | SWP_NOACTIVATE);
}

void CClevoFanControlDlg::RefreshStatus()
{
	if (m_ctlStatus.GetSafeHwnd() == NULL)
	{
		return;
	}
	CCoreStatusSnapshot status = {};
	if (!m_core.GetStatusSnapshot(&status))
	{
		return;
	}
	const BOOL gpuAvailable = status.gpuAvailable;
	const BOOL ecReady = status.ecReady;
	for (int i = 0; i < 2; ++i)
	{
		const BOOL available = ecReady && (i == 0 || gpuAvailable);
		CString text;
		if (available)
		{
			text.Format(_T("%d C"), status.currentTemperature[i]);
		}
		else
		{
			text = _T("--");
		}
		m_ctlStatus.SetItemText(0, i + 1, text);
		text.Format(_T("%d"), status.targetCurveLevel[i]);
		m_ctlStatus.SetItemText(1, i + 1, available ? text : _T("--"));
		text.Format(_T("%d%%"), status.targetDuty[i]);
		m_ctlStatus.SetItemText(2, i + 1, available ? text : _T("--"));
		text.Format(_T("%d%%"), status.currentDuty[i]);
		m_ctlStatus.SetItemText(3, i + 1, available ? text : _T("--"));
		if (available && status.currentRPM[i] >= 0)
		{
			text.Format(_T("%d"), status.currentRPM[i]);
		}
		else
		{
			text = _T("--");
		}
		m_ctlStatus.SetItemText(4, i + 1, text);
	}
	m_cpuCurveCtrl.SetCurrentTemperature(status.ecReady ? status.currentTemperature[0] : -1);
	m_gpuCurveCtrl.SetCurrentTemperature(status.ecReady && status.gpuAvailable ? status.currentTemperature[1] : -1);
}

void CClevoFanControlDlg::OnTimer(UINT_PTR nIDEvent)
{
	CDialogEx::OnTimer(nIDEvent);
	if (m_nUiTimerId == 0 || nIDEvent != m_nUiTimerId)
	{
		return;
	}
	m_bWindowVisible = IsWindowVisible();
	if (m_bStartupPending)
	{
		m_bStartupPending = FALSE;
		ShowWindowFromTray();
	}
	// Keep telemetry current while the window is hidden in the notification area.
	m_core.SetUpdateRPM(TRUE);
	ScanPresetProcesses();
	CCoreStatusSnapshot status = {};
	if (!m_core.GetStatusSnapshot(&status))
	{
		return;
	}
	if (status.lastUpdateTime != m_nLastCoreUpdateTime)
	{
		m_nLastCoreUpdateTime = status.lastUpdateTime;
		if (m_bTrayAdded)
		{
			char tip[128];
			if (status.ecReady && status.gpuAvailable)
			{
				sprintf_s(tip, sizeof(tip), "CPU %d C %d%% | GPU %d C %d%%", status.currentTemperature[0], status.currentDuty[0], status.currentTemperature[1], status.currentDuty[1]);
			}
			else if (status.ecReady)
			{
				sprintf_s(tip, sizeof(tip), "CPU %d C %d%% | GPU --", status.currentTemperature[0], status.currentDuty[0]);
			}
			else
			{
				strcpy_s(tip, sizeof(tip), "CPU -- | GPU --");
			}
			SetTray(tip);
		}
	}
	RefreshStatus();
}

void CClevoFanControlDlg::StopCoreThread()
{
	if (m_hCoreThread != NULL)
	{
		m_core.RequestExit(1);
		WaitForSingleObject(m_hCoreThread, INFINITE);
		CloseHandle(m_hCoreThread);
		m_hCoreThread = NULL;
	}
	m_core.SetParentDialog(NULL);
	if (m_hWnd != NULL && ::IsWindow(m_hWnd))
	{
		if (m_nUiTimerId != 0)
		{
			KillTimer(m_nUiTimerId);
			m_nUiTimerId = 0;
		}
	}
}

std::string CClevoFanControlDlg::ConfigurationPath() const
{
	CStringA path(GetExePath());
	path += "\\";
	path += ConfigStore::FileName();
	return std::string(path.GetString());
}

std::string CClevoFanControlDlg::PresetConfigurationPath() const
{
	CStringA path(GetExePath());
	path += "\\";
	path += PresetStore::FileName();
	return std::string(path.GetString());
}

BOOL CClevoFanControlDlg::SavePresetCollection(const PresetCollection& collection, CString* error)
{
	std::string diagnostic;
	if (PresetStore::Save(PresetConfigurationPath(), collection, &diagnostic))
	{
		if (error != NULL)
		{
			error->Empty();
		}
		return TRUE;
	}
	if (error != NULL)
	{
		*error = DiagnosticText(diagnostic);
	}
	return FALSE;
}

BOOL CClevoFanControlDlg::ApplyPresetAt(int index, BOOL showError, BOOL automatic)
{
	if (index < 0 || index >= static_cast<int>(m_presets.presets.size()))
	{
		if (showError)
		{
			AfxMessageBox(_T("The selected preset is no longer available."), MB_ICONWARNING);
		}
		return FALSE;
	}

	FanConfig candidate = m_globalConfig;
	CopyControlSettings(m_presets.presets[index].config, &candidate);
	std::string validationError;
	if (!candidate.Validate(&validationError))
	{
		if (showError)
		{
			AfxMessageBox(DiagnosticText(validationError), MB_ICONWARNING);
		}
		return FALSE;
	}
	if (!m_core.ApplyConfig(candidate))
	{
		if (showError)
		{
			AfxMessageBox(_T("The selected preset could not be applied to the worker."), MB_ICONWARNING);
		}
		else if (automatic)
		{
			static ULONGLONG lastTraceTick = 0;
			const ULONGLONG now = GetTickCount64();
			if (lastTraceTick == 0 || now - lastTraceTick >= 5000)
			{
				TRACE("Automatic preset application failed for index %d\n", index);
				lastTraceTick = now;
			}
		}
		return FALSE;
	}

	m_draft = candidate;
	m_nActivePreset = index;
	m_nSelectedCurve = 0;
	SetDraftDirty(FALSE);
	SyncDraftToControls();
	SyncPresetControls();
	RefreshStatus();
	return TRUE;
}

BOOL CClevoFanControlDlg::ApplyGlobalConfiguration(BOOL showError)
{
	FanConfig candidate = m_globalConfig;
	std::string validationError;
	if (!candidate.Validate(&validationError))
	{
		if (showError)
		{
			AfxMessageBox(DiagnosticText(validationError), MB_ICONWARNING);
		}
		return FALSE;
	}
	if (!m_core.ApplyConfig(candidate))
	{
		if (showError)
		{
			AfxMessageBox(_T("The global configuration could not be applied to the worker."), MB_ICONWARNING);
		}
		return FALSE;
	}
	m_draft = candidate;
	m_nActivePreset = -1;
	m_nSelectedCurve = 0;
	SetDraftDirty(FALSE);
	SyncDraftToControls();
	SyncPresetControls();
	RefreshStatus();
	return TRUE;
}

void CClevoFanControlDlg::ScanPresetProcesses()
{
	if (!m_presets.autoSwitch)
	{
		return;
	}
	const ULONGLONG now = GetTickCount64();
	if (m_lastPresetScanTick != 0 && now - m_lastPresetScanTick < 500)
	{
		return;
	}
	m_lastPresetScanTick = now;

	std::vector<std::string> processNames;
	std::string diagnostic;
	if (!CollectRunningProcessNames(&processNames, &diagnostic))
	{
		static ULONGLONG lastTraceTick = 0;
		if (lastTraceTick == 0 || now - lastTraceTick >= 5000)
		{
			TRACE("Preset process scan failed: %s\n", diagnostic.c_str());
			lastTraceTick = now;
		}
		return;
	}

	int selectedIndex = -1;
	if (FindMatchingPresetIndex(m_presets, processNames, &selectedIndex) &&
		selectedIndex != m_nActivePreset)
	{
		ApplyPresetAt(selectedIndex, FALSE, TRUE);
	}
}

FanCurvePoints* CClevoFanControlDlg::SelectedCurve()
{
	return m_nSelectedCurve == 0 ? &m_draft.CpuCurve : &m_draft.GpuCurve;
}

const FanCurvePoints* CClevoFanControlDlg::SelectedCurve() const
{
	return m_nSelectedCurve == 0 ? &m_draft.CpuCurve : &m_draft.GpuCurve;
}

void CClevoFanControlDlg::LoadDraftFromCore()
{
	if (!m_core.GetConfigSnapshot(&m_draft))
	{
		m_draft.LoadDefault();
	}
	m_bDraftDirty = FALSE;
	SyncDraftToControls();
}

BOOL CClevoFanControlDlg::LoadDraftFromFile(BOOL showWarning)
{
	FanConfig loaded;
	std::string diagnostic;
	const ConfigLoadStatus status = ConfigStore::Load(ConfigurationPath(), &loaded, &diagnostic);
	if (status == ConfigLoadStatus::Loaded || status == ConfigLoadStatus::Missing)
	{
		m_globalConfig = loaded;
		m_draft = loaded;
		return TRUE;
	}
	if (showWarning && !diagnostic.empty())
	{
		AfxMessageBox(DiagnosticText(diagnostic), MB_ICONWARNING);
	}
	return FALSE;
}

BOOL CClevoFanControlDlg::LoadPresetCollection(BOOL showWarning)
{
	PresetCollection loaded;
	std::string diagnostic;
	const PresetLoadStatus status = PresetStore::Load(
		PresetConfigurationPath(), &loaded, &diagnostic);
	m_presets = loaded;
	m_bPresetStoreDirty = FALSE;
	if (status == PresetLoadStatus::Loaded || status == PresetLoadStatus::Missing)
	{
		return TRUE;
	}
	if (showWarning && !diagnostic.empty())
	{
		AfxMessageBox(DiagnosticText(diagnostic), MB_ICONWARNING);
	}
	return FALSE;
}

void CClevoFanControlDlg::SetEditInteger(CEdit& edit, int value)
{
	CString text;
	text.Format(_T("%d"), value);
	edit.SetWindowText(text);
}

BOOL CClevoFanControlDlg::ParseEditInteger(const CEdit& edit, int* value) const
{
	if (value == NULL || edit.GetSafeHwnd() == NULL)
	{
		return FALSE;
	}
	CString text;
	edit.GetWindowText(text);
	text.Trim();
	if (text.IsEmpty())
	{
		return FALSE;
	}
	const char* start = text.GetString();
	char* end = NULL;
	errno = 0;
	const long parsed = strtol(start, &end, 10);
	if (errno == ERANGE || end == start || *end != '\0' || parsed < INT_MIN || parsed > INT_MAX)
	{
		return FALSE;
	}
	*value = static_cast<int>(parsed);
	return TRUE;
}

void CClevoFanControlDlg::SyncSelectedNodeToControls()
{
	FanCurvePoints* curve = SelectedCurve();
	if (curve == NULL || curve->empty())
	{
		m_nodeIndex.SetWindowText(_T(""));
		m_nodeTemperature.SetWindowText(_T(""));
		m_nodeDuty.SetWindowText(_T(""));
		return;
	}
	int selected = m_nSelectedCurve == 0 ? m_cpuCurveCtrl.GetSelectedIndex() : m_gpuCurveCtrl.GetSelectedIndex();
	if (selected < 0 || selected >= static_cast<int>(curve->size()))
	{
		selected = 0;
	}
	if (m_nSelectedCurve == 0)
	{
		m_cpuCurveCtrl.SetSelectedIndex(selected);
	}
	else
	{
		m_gpuCurveCtrl.SetSelectedIndex(selected);
	}
	SetEditInteger(m_nodeIndex, selected + 1);
	SetEditInteger(m_nodeTemperature, (*curve)[selected].temperature);
	SetEditInteger(m_nodeDuty, (*curve)[selected].duty);
}

void CClevoFanControlDlg::RefreshCurves()
{
	m_cpuCurveCtrl.SetCurve(m_draft.CpuCurve);
	m_gpuCurveCtrl.SetCurve(m_draft.GpuCurve);
	CCoreStatusSnapshot status = {};
	if (m_core.GetStatusSnapshot(&status))
	{
		m_cpuCurveCtrl.SetCurrentTemperature(status.ecReady ? status.currentTemperature[0] : -1);
		m_gpuCurveCtrl.SetCurrentTemperature(status.ecReady && status.gpuAvailable ? status.currentTemperature[1] : -1);
	}
	else
	{
		m_cpuCurveCtrl.SetCurrentTemperature(-1);
		m_gpuCurveCtrl.SetCurrentTemperature(-1);
	}
	SyncSelectedNodeToControls();
}

void CClevoFanControlDlg::SyncDraftToControls()
{
	m_bSyncingControls = TRUE;
	m_ctlTakeOver.SetCheck(m_draft.TakeOver ? BST_CHECKED : BST_UNCHECKED);
	m_ctlForcedCooling.SetCheck(m_draft.ForceCooling ? BST_CHECKED : BST_UNCHECKED);
	m_ctlLinear.SetCheck(m_draft.Linear ? BST_CHECKED : BST_UNCHECKED);
	m_ctlSoftControl.SetCheck(m_draft.SoftControl ? BST_CHECKED : BST_UNCHECKED);
	m_ctlAutorun.SetCheck(m_draft.AutoRun ? BST_CHECKED : BST_UNCHECKED);
	SetEditInteger(m_ctlInterval, m_draft.UpdateInterval);
	SetEditInteger(m_ctlTransition, m_draft.TransitionTemp);
	SetEditInteger(m_ctlForceTemp, m_draft.ForceTemp);
	SetEditInteger(m_fontSize, m_draft.UiFontSize);
	m_curveSelector.SetCurSel(m_nSelectedCurve);
	m_closeBehavior.SetCurSel(m_draft.CloseToTray ? 1 : 0);
	RefreshCurves();
	m_bSyncingControls = FALSE;
	ApplyUIFont();
}

void CClevoFanControlDlg::SyncPresetControls()
{
	m_bSyncingControls = TRUE;
	m_autoSwitch.SetCheck(m_presets.autoSwitch ? BST_CHECKED : BST_UNCHECKED);
	UpdateActivePresetStatus();
	m_bSyncingControls = FALSE;
}

void CClevoFanControlDlg::UpdateActivePresetStatus()
{
	if (m_activePresetStatus.GetSafeHwnd() == NULL)
	{
		return;
	}

	CString text;
	if (m_nActivePreset < 0 ||
		m_nActivePreset >= static_cast<int>(m_presets.presets.size()))
	{
		text = _T("Active preset: Global");
	}
	else
	{
		text = _T("Active preset: ");
		text += CString(CStringA(m_presets.presets[m_nActivePreset].name.c_str()));
	}

	CRect clientRect;
	m_activePresetStatus.GetClientRect(&clientRect);
	if (clientRect.Width() > 0)
	{
		CClientDC dc(&m_activePresetStatus);
		CFont* oldFont = NULL;
		if (m_uiFont.GetSafeHandle() != NULL)
		{
			oldFont = dc.SelectObject(&m_uiFont);
		}
		const int availableWidth = clientRect.Width();
		const CString ellipsis = _T("...");
		if (dc.GetTextExtent(text).cx > availableWidth)
		{
			while (text.GetLength() > ellipsis.GetLength() &&
				dc.GetTextExtent(text + ellipsis).cx > availableWidth)
			{
				text.Delete(text.GetLength() - 1, 1);
			}
			if (dc.GetTextExtent(text).cx > availableWidth)
			{
				text = ellipsis;
			}
			else if (dc.GetTextExtent(text + ellipsis).cx <= availableWidth)
			{
				text += ellipsis;
			}
		}
		if (oldFont != NULL)
		{
			dc.SelectObject(oldFont);
		}
	}
	m_activePresetStatus.SetWindowText(text);
}

void CClevoFanControlDlg::OnFontSizeChanged()
{
	if (m_bSyncingControls)
	{
		return;
	}

	int pointSize = 0;
	if (!ParseEditInteger(m_fontSize, &pointSize) ||
		pointSize < FAN_UI_FONT_SIZE_MIN || pointSize > FAN_UI_FONT_SIZE_MAX)
	{
		SetDraftDirty(TRUE);
		return;
	}

	m_draft.UiFontSize = pointSize;
	SetDraftDirty(TRUE);
	ApplyUIFont();

	CRect clientRect;
	GetClientRect(&clientRect);
	LayoutControls(clientRect.Width(), clientRect.Height());
}

void CClevoFanControlDlg::SetDraftDirty(BOOL dirty)
{
	m_bDraftDirty = dirty;
	SetWindowText((m_bDraftDirty || m_bPresetStoreDirty) ?
		_T("ClevoFanControl *") : _T("ClevoFanControl"));
}

BOOL CClevoFanControlDlg::ApplyNodeEditor(BOOL showError)
{
	FanCurvePoints* curve = SelectedCurve();
	if (curve == NULL || curve->empty())
	{
		return TRUE;
	}
	int index = 0;
	int temperature = 0;
	int duty = 0;
	if (!ParseEditInteger(m_nodeIndex, &index) || !ParseEditInteger(m_nodeTemperature, &temperature) || !ParseEditInteger(m_nodeDuty, &duty))
	{
		if (showError)
		{
			AfxMessageBox(_T("Node index, temperature and duty must be integers."), MB_ICONWARNING);
		}
		SyncSelectedNodeToControls();
		return FALSE;
	}
	--index;
	std::string error;
	if (index < 0 || index >= static_cast<int>(curve->size()))
	{
		if (showError)
		{
			AfxMessageBox(_T("The selected node index is out of range."), MB_ICONWARNING);
		}
		SyncSelectedNodeToControls();
		return FALSE;
	}
	const FanCurvePoint previousPoint = (*curve)[index];
	if (!TrySetFanCurvePoint(curve, static_cast<size_t>(index), temperature, duty, &error))
	{
		if (showError)
		{
			AfxMessageBox(DiagnosticText(error), MB_ICONWARNING);
		}
		SyncSelectedNodeToControls();
		return FALSE;
	}
	if (m_nSelectedCurve == 0)
	{
		m_cpuCurveCtrl.SetCurve(*curve);
		m_cpuCurveCtrl.SetSelectedIndex(index);
	}
	else
	{
		m_gpuCurveCtrl.SetCurve(*curve);
		m_gpuCurveCtrl.SetSelectedIndex(index);
	}
	if (previousPoint.temperature != (*curve)[index].temperature ||
		previousPoint.duty != (*curve)[index].duty)
	{
		SetDraftDirty(TRUE);
	}
	SyncSelectedNodeToControls();
	return TRUE;
}

BOOL CClevoFanControlDlg::ReadDraftFromControls()
{
	int value = 0;
	if (!ParseEditInteger(m_ctlInterval, &value))
	{
		AfxMessageBox(_T("Update interval must be an integer."), MB_ICONWARNING);
		return FALSE;
	}
	m_draft.UpdateInterval = value;
	if (!ParseEditInteger(m_ctlTransition, &value))
	{
		AfxMessageBox(_T("Transition temperature must be an integer."), MB_ICONWARNING);
		return FALSE;
	}
	m_draft.TransitionTemp = value;
	if (!ParseEditInteger(m_ctlForceTemp, &value))
	{
		AfxMessageBox(_T("Force-cooling temperature must be an integer."), MB_ICONWARNING);
		return FALSE;
	}
	m_draft.ForceTemp = value;
	if (!ParseEditInteger(m_fontSize, &value))
	{
		AfxMessageBox(_T("Font size must be an integer."), MB_ICONWARNING);
		return FALSE;
	}
	m_draft.UiFontSize = value;
	m_draft.TakeOver = m_ctlTakeOver.GetCheck() != 0;
	m_draft.ForceCooling = m_ctlForcedCooling.GetCheck() != 0;
	m_draft.Linear = m_ctlLinear.GetCheck() != 0;
	m_draft.SoftControl = m_ctlSoftControl.GetCheck() != 0;
	m_draft.AutoRun = m_ctlAutorun.GetCheck() != 0;
	m_draft.CloseToTray = m_closeBehavior.GetCurSel() == 1;
	m_draft.CpuCurve = m_cpuCurveCtrl.GetCurve();
	m_draft.GpuCurve = m_gpuCurveCtrl.GetCurve();
	return TRUE;
}

BOOL CClevoFanControlDlg::ValidateDraft(CString* error) const
{
	if (error != NULL)
	{
		error->Empty();
	}
	std::string validation;
	if (!m_draft.Validate(&validation))
	{
		if (error != NULL)
		{
			*error = DiagnosticText(validation);
		}
		return FALSE;
	}
	return TRUE;
}

BOOL CClevoFanControlDlg::SaveDraft()
{
	FanConfig oldAppliedConfig;
	if (!m_core.GetConfigSnapshot(&oldAppliedConfig))
	{
		oldAppliedConfig = m_draft;
	}
	const FanConfig oldGlobalConfig = m_globalConfig;

	if (!ApplyNodeEditor(TRUE) || !ReadDraftFromControls())
	{
		return FALSE;
	}
	CString validationError;
	if (!ValidateDraft(&validationError))
	{
		AfxMessageBox(validationError, MB_ICONWARNING);
		return FALSE;
	}

	const BOOL hasActivePreset = m_nActivePreset >= 0;
	if (hasActivePreset && m_nActivePreset >= static_cast<int>(m_presets.presets.size()))
	{
		AfxMessageBox(_T("The active preset is no longer available."), MB_ICONWARNING);
		return FALSE;
	}

	FanConfig candidateGlobal = m_globalConfig;
	PresetCollection candidatePresets = m_presets;
	if (hasActivePreset)
	{
		CopyControlSettings(m_draft, &candidatePresets.presets[m_nActivePreset].config);
		candidateGlobal.UiFontSize = m_draft.UiFontSize;
		candidateGlobal.AutoRun = m_draft.AutoRun;
		candidateGlobal.CloseToTray = m_draft.CloseToTray;
	}
	else
	{
		candidateGlobal = m_draft;
	}
	std::string candidateError;
	if (!candidateGlobal.Validate(&candidateError))
	{
		AfxMessageBox(DiagnosticText(candidateError), MB_ICONWARNING);
		return FALSE;
	}
	if (!candidatePresets.Validate(&candidateError))
	{
		AfxMessageBox(DiagnosticText(candidateError), MB_ICONWARNING);
		return FALSE;
	}

	const BOOL oldAutoRun = m_bSavedAutoRun;
	const BOOL newAutoRun = candidateGlobal.AutoRun;
	if (oldAutoRun != newAutoRun && !SetAutorunTask(TRUE, newAutoRun))
	{
		m_ctlAutorun.SetCheck(oldAutoRun ? BST_CHECKED : BST_UNCHECKED);
		m_draft.AutoRun = oldAutoRun != FALSE;
		return FALSE;
	}

	if (!m_core.ApplyConfig(m_draft))
	{
		if (oldAutoRun != newAutoRun)
		{
			SetAutorunTask(TRUE, oldAutoRun);
		}
		AfxMessageBox(_T("The configuration could not be applied to the worker; no configuration files were changed."), MB_ICONERROR);
		return FALSE;
	}

	std::string diagnostic;
	if (!ConfigStore::Save(ConfigurationPath(), candidateGlobal, &diagnostic))
	{
		m_core.ApplyConfig(oldAppliedConfig);
		if (oldAutoRun != newAutoRun)
		{
			SetAutorunTask(TRUE, oldAutoRun);
		}
		AfxMessageBox(DiagnosticText(diagnostic), MB_ICONERROR);
		return FALSE;
	}
	if (m_bPresetStoreDirty || hasActivePreset)
	{
		if (!PresetStore::Save(PresetConfigurationPath(), candidatePresets, &diagnostic))
		{
			m_core.ApplyConfig(oldAppliedConfig);
			std::string rollbackDiagnostic;
			if (!ConfigStore::Save(ConfigurationPath(), oldGlobalConfig, &rollbackDiagnostic))
			{
				TRACE("Unable to roll back global configuration: %s\n", rollbackDiagnostic.c_str());
			}
			if (oldAutoRun != newAutoRun)
			{
				SetAutorunTask(TRUE, oldAutoRun);
			}
			AfxMessageBox(DiagnosticText(diagnostic), MB_ICONERROR);
			return FALSE;
		}
	}
	m_globalConfig = candidateGlobal;
	m_presets = candidatePresets;
	m_bSavedAutoRun = newAutoRun;
	m_bPresetStoreDirty = FALSE;
	SetDraftDirty(FALSE);
	SyncPresetControls();
	return TRUE;
}

void CClevoFanControlDlg::OnBnClickedButtonSave()
{
	SaveDraft();
}

void CClevoFanControlDlg::OnBnClickedButtonReset()
{
	ApplyGlobalConfiguration(TRUE);
}

void CClevoFanControlDlg::OnBnClickedButtonLoad()
{
	const FanConfig previousGlobal = m_globalConfig;
	const FanConfig previousDraft = m_draft;
	const int previousActivePreset = m_nActivePreset;
	const BOOL previousDraftDirty = m_bDraftDirty;
	const BOOL previousSavedAutoRun = m_bSavedAutoRun;
	if (!LoadDraftFromFile(TRUE))
	{
		return;
	}
	m_bSavedAutoRun = m_globalConfig.AutoRun ? TRUE : FALSE;
	if (!ApplyGlobalConfiguration(TRUE))
	{
		m_globalConfig = previousGlobal;
		m_draft = previousDraft;
		m_nActivePreset = previousActivePreset;
		m_bSavedAutoRun = previousSavedAutoRun;
		m_bDraftDirty = previousDraftDirty;
		SyncDraftToControls();
		SyncPresetControls();
		SetDraftDirty(previousDraftDirty);
	}
}

void CClevoFanControlDlg::OnBnClickedCurveAdd()
{
	ApplyNodeEditor(FALSE);
	FanCurvePoints* curve = SelectedCurve();
	if (curve == NULL || curve->size() >= FAN_CURVE_MAX_POINTS)
	{
		AfxMessageBox(_T("A curve can contain at most 16 points."), MB_ICONWARNING);
		return;
	}
	int selected = m_nSelectedCurve == 0 ? m_cpuCurveCtrl.GetSelectedIndex() : m_gpuCurveCtrl.GetSelectedIndex();
	if (selected < 0 || selected >= static_cast<int>(curve->size()))
	{
		selected = static_cast<int>(curve->size()) - 1;
	}
	int temperature = 0;
	int duty = (*curve)[selected].duty;
	if (selected + 1 < static_cast<int>(curve->size()))
	{
		temperature = ((*curve)[selected].temperature + (*curve)[selected + 1].temperature) / 2;
		duty = ((*curve)[selected].duty + (*curve)[selected + 1].duty) / 2;
	}
	else
	{
		temperature = (*curve)[selected].temperature + 5;
	}
	std::string error;
	if (!TryInsertFanCurvePoint(curve, temperature, duty, &error))
	{
		AfxMessageBox(DiagnosticText(error), MB_ICONWARNING);
		return;
	}
	++selected;
	SetDraftDirty(TRUE);
	RefreshCurves();
	if (m_nSelectedCurve == 0)
	{
		m_cpuCurveCtrl.SetSelectedIndex(selected);
	}
	else
	{
		m_gpuCurveCtrl.SetSelectedIndex(selected);
	}
	SyncSelectedNodeToControls();
}

void CClevoFanControlDlg::OnBnClickedCurveDelete()
{
	ApplyNodeEditor(FALSE);
	FanCurvePoints* curve = SelectedCurve();
	const int selected = m_nSelectedCurve == 0 ? m_cpuCurveCtrl.GetSelectedIndex() : m_gpuCurveCtrl.GetSelectedIndex();
	std::string error;
	if (curve == NULL || selected < 0 || !TryDeleteFanCurvePoint(curve, static_cast<size_t>(selected), &error))
	{
		AfxMessageBox(DiagnosticText(error.empty() ? std::string("A curve must keep at least two points.") : error), MB_ICONWARNING);
		return;
	}
	SetDraftDirty(TRUE);
	RefreshCurves();
	SyncSelectedNodeToControls();
}

void CClevoFanControlDlg::OnBnClickedCurveReset()
{
	ApplyNodeEditor(FALSE);
	*SelectedCurve() = MakeDefaultFanCurve();
	SetDraftDirty(TRUE);
	RefreshCurves();
}

void CClevoFanControlDlg::OnCbnSelchangeCurve()
{
	ApplyNodeEditor(FALSE);
	const int selection = m_curveSelector.GetCurSel();
	m_nSelectedCurve = selection == 1 ? 1 : 0;
	SyncSelectedNodeToControls();
}

void CClevoFanControlDlg::OnNodeEditKillFocus()
{
	ApplyNodeEditor(FALSE);
}

LRESULT CClevoFanControlDlg::OnFanCurveChanged(WPARAM wParam, LPARAM lParam)
{
	const int curveId = static_cast<int>(wParam);
	const int selected = static_cast<int>(lParam);
	bool curveChanged = false;
	if (curveId == 0)
	{
		const FanCurvePoints curve = m_cpuCurveCtrl.GetCurve();
		curveChanged = !SameCurve(m_draft.CpuCurve, curve);
		m_draft.CpuCurve = curve;
	}
	else if (curveId == 1)
	{
		const FanCurvePoints curve = m_gpuCurveCtrl.GetCurve();
		curveChanged = !SameCurve(m_draft.GpuCurve, curve);
		m_draft.GpuCurve = curve;
	}
	else
	{
		return 0;
	}
	m_nSelectedCurve = curveId;
	m_curveSelector.SetCurSel(curveId);
	if (curveId == 0)
	{
		m_cpuCurveCtrl.SetSelectedIndex(selected);
	}
	else
	{
		m_gpuCurveCtrl.SetSelectedIndex(selected);
	}
	if (curveChanged)
	{
		SetDraftDirty(TRUE);
	}
	SyncSelectedNodeToControls();
	return 0;
}

void CClevoFanControlDlg::OnBnClickedCheckTakeover()
{
	const BOOL previous = m_draft.TakeOver ? TRUE : FALSE;
	m_draft.TakeOver = m_ctlTakeOver.GetCheck() != 0;
	if (!m_core.ApplyConfig(m_draft))
	{
		m_draft.TakeOver = previous ? true : false;
		m_ctlTakeOver.SetCheck(previous ? BST_CHECKED : BST_UNCHECKED);
		AfxMessageBox(_T("The takeover setting could not be applied."), MB_ICONWARNING);
		return;
	}
	SetDraftDirty(TRUE);
	RefreshStatus();
}

void CClevoFanControlDlg::OnBnClickedCheckForce()
{
	m_draft.ForceCooling = m_ctlForcedCooling.GetCheck() != 0;
	m_core.SetForcedCooling(m_draft.ForceCooling ? TRUE : FALSE);
	SetDraftDirty(TRUE);
}

void CClevoFanControlDlg::OnBnClickedCheckLinear()
{
	m_draft.Linear = m_ctlLinear.GetCheck() != 0;
	SetDraftDirty(TRUE);
}

void CClevoFanControlDlg::OnBnClickedCheckSoftControl()
{
	m_draft.SoftControl = m_ctlSoftControl.GetCheck() != 0;
	SetDraftDirty(TRUE);
	if (m_draft.SoftControl)
	{
		CCoreStatusSnapshot status = {};
		if (m_core.GetStatusSnapshot(&status))
		{
			m_core.SetSoftControlTargets(status.currentDuty, status.targetDuty);
		}
	}
}

void CClevoFanControlDlg::OnBnClickedCheckAutorun()
{
	m_draft.AutoRun = m_ctlAutorun.GetCheck() != 0;
	SetDraftDirty(TRUE);
}

void CClevoFanControlDlg::OnBnClickedAutoSwitch()
{
	if (m_bSyncingControls)
	{
		return;
	}
	m_presets.autoSwitch = m_autoSwitch.GetCheck() != 0;
	m_bPresetStoreDirty = TRUE;
	if (m_presets.autoSwitch)
	{
		m_lastPresetScanTick = 0;
	}
	SetDraftDirty(TRUE);
}

void CClevoFanControlDlg::OnBnClickedManagePresets()
{
	CPresetManagerDlg dialog(
		m_presets,
		m_nActivePreset,
		m_draft,
		static_cast<HFONT>(m_uiFont.GetSafeHandle()),
		this);
	if (dialog.DoModal() != IDOK)
	{
		return;
	}

	PresetCollection candidate = dialog.GetCollection();
	std::string validationError;
	if (!candidate.Validate(&validationError))
	{
		AfxMessageBox(DiagnosticText(validationError), MB_ICONWARNING);
		return;
	}
	CString saveError;
	if (!SavePresetCollection(candidate, &saveError))
	{
		AfxMessageBox(saveError, MB_ICONERROR);
		return;
	}

	m_presets = candidate;
	m_bPresetStoreDirty = FALSE;
	const int returnedActiveIndex = dialog.GetActiveIndex();
	if (dialog.IsApplyRequested())
	{
		if (!ApplyPresetAt(dialog.GetApplyIndex(), TRUE, FALSE))
		{
			SyncPresetControls();
			SetDraftDirty(m_bDraftDirty);
		}
		return;
	}

	if (returnedActiveIndex < 0 && m_nActivePreset >= 0)
	{
		ApplyGlobalConfiguration(TRUE);
		return;
	}
	m_nActivePreset = returnedActiveIndex;
	SyncPresetControls();
	SetDraftDirty(m_bDraftDirty);
}

void CClevoFanControlDlg::OnCbnSelchangeCloseBehavior()
{
	m_draft.CloseToTray = m_closeBehavior.GetCurSel() == 1;
	SetDraftDirty(TRUE);
}

void CClevoFanControlDlg::OnConfigurationEditChanged()
{
	if (m_bSyncingControls)
	{
		return;
	}
	SetDraftDirty(TRUE);
}

void CClevoFanControlDlg::OnOK()
{
	if (m_bShuttingDown || !SaveBeforeClose())
	{
		return;
	}
	m_bShuttingDown = TRUE;
	StopCoreThread();
	SetTray(NULL);
	CDialogEx::OnOK();
}

void CClevoFanControlDlg::OnCancel()
{
	if (m_bShuttingDown)
	{
		return;
	}
	if (m_draft.CloseToTray)
	{
		HideWindowToTray();
		return;
	}
	OnOK();
}

BOOL CClevoFanControlDlg::SaveBeforeClose()
{
	if (!m_bDraftDirty && !m_bPresetStoreDirty)
	{
		return TRUE;
	}
	const int choice = MessageBox(_T("Save the current configuration before closing?"),
		_T("ClevoFanControl"), MB_YESNOCANCEL | MB_ICONQUESTION);
	if (choice == IDCANCEL)
	{
		return FALSE;
	}
	return choice != IDYES || SaveDraft();
}

void CClevoFanControlDlg::ShowWindowFromTray()
{
	if (m_bShuttingDown || m_hWnd == NULL || !::IsWindow(m_hWnd))
	{
		return;
	}
	m_bWindowVisible = TRUE;
	ShowWindow(IsIconic() ? SW_RESTORE : SW_SHOW);
	SetForegroundWindow();
	UpdateWindow();
}

void CClevoFanControlDlg::HideWindowToTray()
{
	if (m_bShuttingDown || m_hWnd == NULL || !::IsWindow(m_hWnd))
	{
		return;
	}
	m_bWindowVisible = FALSE;
	ShowWindow(SW_HIDE);
}

void CClevoFanControlDlg::SetTray(PCSTR string)
{
	NOTIFYICONDATAA nid = {0};
	nid.cbSize = sizeof(nid);
	nid.hWnd = m_hWnd;
	nid.uID = IDR_MAINFRAME;
	nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
	nid.uCallbackMessage = WM_SHOWTASK;
	nid.hIcon = LoadIcon(AfxGetInstanceHandle(), MAKEINTRESOURCE(IDR_MAINFRAME));
	if (string != NULL)
	{
		strcpy_s(nid.szTip, sizeof(nid.szTip), string);
		if (!m_bTrayAdded)
		{
			m_bTrayAdded = Shell_NotifyIconA(NIM_ADD, &nid);
		}
		else
		{
			Shell_NotifyIconA(NIM_MODIFY, &nid);
		}
	}
	else
	{
		Shell_NotifyIconA(NIM_DELETE, &nid);
		m_bTrayAdded = FALSE;
	}
}

LRESULT CClevoFanControlDlg::OnShowTask(WPARAM wParam, LPARAM lParam)
{
	if (wParam != IDR_MAINFRAME)
	{
		return 1;
	}
	if (lParam == WM_LBUTTONUP || lParam == WM_LBUTTONDBLCLK)
	{
		ShowWindowFromTray();
	}
	else if (lParam == WM_RBUTTONUP)
	{
		CMenu menu;
		menu.CreatePopupMenu();
		menu.AppendMenu(MF_STRING, IDR_SHOW, IsWindowVisible() ? _T("Hide") : _T("Show"));
		menu.AppendMenu(MF_SEPARATOR);
		menu.AppendMenu(MF_STRING, IDR_EXIT, _T("Exit"));
		POINT point;
		GetCursorPos(&point);
		SetForegroundWindow();
		const int command = menu.TrackPopupMenu(TPM_RETURNCMD, point.x, point.y, this);
		if (command == IDR_SHOW)
		{
			if (IsWindowVisible())
			{
				HideWindowToTray();
			}
			else
			{
				ShowWindowFromTray();
			}
		}
		else if (command == IDR_EXIT)
		{
			OnOK();
		}
		PostMessage(WM_NULL);
	}
	return 0;
}

LRESULT CClevoFanControlDlg::OnTaskbarCreated(WPARAM, LPARAM)
{
	m_bTrayAdded = FALSE;
	SetTray("ClevoFanControl");
	return 0;
}

BOOL CClevoFanControlDlg::SetAutorunReg(BOOL, BOOL)
{
	return FALSE;
}

BOOL CClevoFanControlDlg::SetAutorunTask(BOOL bWrite, BOOL bAutorun)
{
	const CString taskName = _T("ClevoFanControl");
	const CString targetPath = GetExePath() + _T("\\ClevoFanControl.exe");
	const CString xmlPath = GetExePath() + _T("\\ClevoFanControl.task.xml");
	if (!bWrite)
	{
		CString output = ExecuteCmd(_T("SCHTASKS /Query /TN \"ClevoFanControl\""));
		return output.Find(taskName) >= 0;
	}
	if (bAutorun)
	{
		if (!CreateTaskXml(CStringA(xmlPath).GetString(), CStringA(targetPath).GetString()))
		{
			return FALSE;
		}
		CString command;
		command.Format(_T("SCHTASKS /Create /F /XML \"%s\" /TN \"%s\""), xmlPath.GetString(), taskName.GetString());
		ExecuteCmd(command);
		DeleteFile(xmlPath);
		return SetAutorunTask(FALSE, FALSE);
	}
	CString command;
	command.Format(_T("SCHTASKS /Delete /F /TN \"%s\""), taskName.GetString());
	ExecuteCmd(command);
	return !SetAutorunTask(FALSE, FALSE);
}

CString CClevoFanControlDlg::ExecuteCmd(CString command)
{
	SECURITY_ATTRIBUTES attributes = {sizeof(attributes), NULL, TRUE};
	HANDLE readHandle = NULL;
	HANDLE writeHandle = NULL;
	if (!CreatePipe(&readHandle, &writeHandle, &attributes, 0))
	{
		return _T("[failed]");
	}
	SetHandleInformation(readHandle, HANDLE_FLAG_INHERIT, 0);
	STARTUPINFO startup = {sizeof(startup)};
	PROCESS_INFORMATION process = {0};
	startup.dwFlags = STARTF_USESHOWWINDOW | STARTF_USESTDHANDLES;
	startup.wShowWindow = SW_HIDE;
	startup.hStdOutput = writeHandle;
	startup.hStdError = writeHandle;
	CStringA commandLine(command);
	char buffer[1024];
	strcpy_s(buffer, sizeof(buffer), commandLine.GetString());
	if (!CreateProcessA(NULL, buffer, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &startup, &process))
	{
		CloseHandle(readHandle);
		CloseHandle(writeHandle);
		return _T("[failed]");
	}
	CloseHandle(writeHandle);
	const DWORD waitResult = WaitForSingleObject(process.hProcess, 5000);
	if (waitResult == WAIT_TIMEOUT)
	{
		TerminateProcess(process.hProcess, ERROR_TIMEOUT);
		CloseHandle(readHandle);
		CloseHandle(process.hThread);
		CloseHandle(process.hProcess);
		return _T("[timeout]");
	}
	if (waitResult != WAIT_OBJECT_0)
	{
		CloseHandle(readHandle);
		CloseHandle(process.hThread);
		CloseHandle(process.hProcess);
		return _T("[failed]");
	}
	CStringA output;
	char outputBuffer[4096];
	DWORD bytesRead = 0;
	while (ReadFile(readHandle, outputBuffer, sizeof(outputBuffer) - 1, &bytesRead, NULL) && bytesRead != 0)
	{
		outputBuffer[bytesRead] = '\0';
		output += outputBuffer;
	}
	CloseHandle(readHandle);
	CloseHandle(process.hThread);
	CloseHandle(process.hProcess);
	return CString(output);
}

BOOL CClevoFanControlDlg::CreateTaskXml(PCSTR strXmlPath, PCSTR strTargetPath)
{
	if (strXmlPath == NULL || strTargetPath == NULL)
	{
		return FALSE;
	}
	const char* xml =
		"<?xml version=\"1.0\" encoding=\"UTF-16\"?>\r\n"
		"<Task version=\"1.2\" xmlns=\"http://schemas.microsoft.com/windows/2004/02/mit/task\">\r\n"
		"  <Triggers><LogonTrigger><Enabled>true</Enabled></LogonTrigger></Triggers>\r\n"
		"  <Principals><Principal id=\"Author\"><GroupId>S-1-5-32-545</GroupId><RunLevel>HighestAvailable</RunLevel></Principal></Principals>\r\n"
		"  <Settings><MultipleInstancesPolicy>StopExisting</MultipleInstancesPolicy><StartWhenAvailable>true</StartWhenAvailable><AllowStartOnDemand>true</AllowStartOnDemand><Enabled>true</Enabled></Settings>\r\n"
		"  <Actions Context=\"Author\"><Exec><Command>%s</Command></Exec></Actions>\r\n"
		"</Task>\r\n";
	char content[4096];
	sprintf_s(content, sizeof(content), xml, strTargetPath);
	FILE* file = NULL;
	if (fopen_s(&file, strXmlPath, "wt") != 0 || file == NULL)
	{
		return FALSE;
	}
	fputs(content, file);
	fclose(file);
	return TRUE;
}
