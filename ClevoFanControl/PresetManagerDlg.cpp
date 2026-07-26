#include "stdafx.h"
#include "PresetManagerDlg.h"
#include "UnicodeUtil.h"

#include <algorithm>
#include <string>

namespace
{
const UINT kFontRedraw = TRUE;

CString Utf8Text(const std::string& value)
{
	std::wstring wide;
	return Utf8ToWide(value, &wide) ? CString(wide.c_str()) : CString(_T(""));
}

int SafeTextHeight(int height)
{
	return (std::max)(1, height);
}
}

BEGIN_MESSAGE_MAP(CPresetManagerDlg, CDialogEx)
	ON_WM_SIZE()
	ON_WM_GETMINMAXINFO()
	ON_NOTIFY(LVN_ITEMCHANGED, IDC_LIST_PRESETS, &CPresetManagerDlg::OnLvnItemchangedPresetList)
	ON_BN_CLICKED(IDC_BUTTON_PRESET_NEW, &CPresetManagerDlg::OnBnClickedPresetNew)
	ON_BN_CLICKED(IDC_BUTTON_PRESET_SAVE_RULE, &CPresetManagerDlg::OnBnClickedPresetSaveRule)
	ON_BN_CLICKED(IDC_BUTTON_PRESET_DELETE, &CPresetManagerDlg::OnBnClickedPresetDelete)
	ON_BN_CLICKED(IDC_BUTTON_PRESET_UP, &CPresetManagerDlg::OnBnClickedPresetMoveUp)
	ON_BN_CLICKED(IDC_BUTTON_PRESET_DOWN, &CPresetManagerDlg::OnBnClickedPresetMoveDown)
	ON_BN_CLICKED(IDC_BUTTON_PRESET_USE, &CPresetManagerDlg::OnBnClickedPresetUse)
END_MESSAGE_MAP()

CPresetManagerDlg::CPresetManagerDlg(const PresetCollection& collection, int activeIndex,
	const FanConfig& currentConfig, HFONT ownerFont, CWnd* parent)
	: CDialogEx(IDD_PRESET_MANAGER_DIALOG, parent),
	  m_collection(collection),
	  m_currentConfig(currentConfig),
	  m_ownerFont(ownerFont),
	  m_activeIndex(activeIndex),
	  m_selectedIndex(-1),
	  m_applyRequested(FALSE),
	  m_applyIndex(-1)
{
	if (m_activeIndex < 0 || m_activeIndex >= static_cast<int>(m_collection.presets.size()))
	{
		m_activeIndex = -1;
	}
}

const PresetCollection& CPresetManagerDlg::GetCollection() const
{
	return m_collection;
}

int CPresetManagerDlg::GetActiveIndex() const
{
	return m_activeIndex;
}

BOOL CPresetManagerDlg::IsApplyRequested() const
{
	return m_applyRequested;
}

int CPresetManagerDlg::GetApplyIndex() const
{
	return m_applyIndex;
}

void CPresetManagerDlg::DoDataExchange(CDataExchange* pDX)
{
	CDialogEx::DoDataExchange(pDX);
	DDX_Control(pDX, IDC_LIST_PRESETS, m_presetList);
	DDX_Control(pDX, IDC_EDIT_PRESET_NAME, m_presetName);
	DDX_Control(pDX, IDC_EDIT_PRESET_PATTERN, m_presetPattern);
	DDX_Control(pDX, IDC_BUTTON_PRESET_NEW, m_presetNew);
	DDX_Control(pDX, IDC_BUTTON_PRESET_SAVE_RULE, m_presetSaveRule);
	DDX_Control(pDX, IDC_BUTTON_PRESET_DELETE, m_presetDelete);
	DDX_Control(pDX, IDC_BUTTON_PRESET_UP, m_presetMoveUp);
	DDX_Control(pDX, IDC_BUTTON_PRESET_DOWN, m_presetMoveDown);
	DDX_Control(pDX, IDC_BUTTON_PRESET_USE, m_presetUse);
}

BOOL CPresetManagerDlg::OnInitDialog()
{
	CDialogEx::OnInitDialog();
	ApplyOwnerFont();

	m_presetList.SetExtendedStyle(m_presetList.GetExtendedStyle() |
		LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_DOUBLEBUFFER);
	m_presetList.InsertColumn(0, _T("Preset"), LVCFMT_LEFT, 0);
	m_presetList.InsertColumn(1, _T("Process name"), LVCFMT_LEFT, 0);
	m_selectedIndex = m_collection.presets.empty() ? -1 : 0;
	RefreshList();
	SelectRow(m_selectedIndex);

	CRect clientRect;
	GetClientRect(&clientRect);
	LayoutControls(clientRect.Width(), clientRect.Height());
	return TRUE;
}

void CPresetManagerDlg::ApplyOwnerFont()
{
	HFONT font = ActiveFont();
	if (font == NULL)
	{
		return;
	}

	SendMessage(WM_SETFONT, reinterpret_cast<WPARAM>(font), kFontRedraw);
	for (CWnd* child = GetWindow(GW_CHILD); child != NULL; child = child->GetWindow(GW_HWNDNEXT))
	{
		child->SendMessage(WM_SETFONT, reinterpret_cast<WPARAM>(font), kFontRedraw);
	}
}

HFONT CPresetManagerDlg::ActiveFont() const
{
	if (m_ownerFont != NULL)
	{
		return m_ownerFont;
	}
	if (m_hWnd != NULL && ::IsWindow(m_hWnd))
	{
		return reinterpret_cast<HFONT>(::SendMessage(m_hWnd, WM_GETFONT, 0, 0));
	}
	return static_cast<HFONT>(::GetStockObject(DEFAULT_GUI_FONT));
}

CSize CPresetManagerDlg::MeasureText(LPCTSTR text) const
{
	if (text == NULL)
	{
		return CSize(0, 0);
	}

	CClientDC dc(const_cast<CPresetManagerDlg*>(this));
	HFONT font = ActiveFont();
	HGDIOBJ oldFont = NULL;
	if (font != NULL)
	{
		oldFont = ::SelectObject(dc.GetSafeHdc(), font);
	}
	CSize size = dc.GetTextExtent(text, static_cast<int>(_tcslen(text)));
	if (oldFont != NULL)
	{
		::SelectObject(dc.GetSafeHdc(), oldFont);
	}
	return size;
}

int CPresetManagerDlg::TextHeight() const
{
	const CSize size = MeasureText(_T("Ag"));
	return SafeTextHeight(size.cy);
}

void CPresetManagerDlg::MoveControl(int id, int left, int top, int width, int height)
{
	CWnd* control = GetDlgItem(id);
	if (control == NULL || control->GetSafeHwnd() == NULL)
	{
		return;
	}
	control->SetWindowPos(NULL, left, top, (std::max)(1, width), (std::max)(1, height),
		SWP_NOZORDER | SWP_NOACTIVATE);
}

void CPresetManagerDlg::LayoutControls(int cx, int cy)
{
	if (cx <= 0 || cy <= 0 || m_presetList.GetSafeHwnd() == NULL)
	{
		return;
	}

	const int textHeight = TextHeight();
	const int padding = (std::max)(6, textHeight / 2);
	const int gap = (std::max)(4, textHeight / 2);
	const int rowHeight = (std::max)(textHeight + textHeight / 2, textHeight + 6);
	const int labelHeight = textHeight + 2;
	const int buttonHeight = rowHeight;
	const int availableWidth = (std::max)(1, cx - padding * 2);
	const int buttonPadding = (std::max)(8, textHeight / 2 + 4);

	const LPCTSTR managementLabels[] = {
		_T("New from current"), _T("Save rule"), _T("Delete"),
		_T("Move up"), _T("Move down"), _T("Use selected")
	};
	const int managementIds[] = {
		IDC_BUTTON_PRESET_NEW, IDC_BUTTON_PRESET_SAVE_RULE, IDC_BUTTON_PRESET_DELETE,
		IDC_BUTTON_PRESET_UP, IDC_BUTTON_PRESET_DOWN, IDC_BUTTON_PRESET_USE
	};
	int managementWidths[_countof(managementLabels)] = {};
	int managementTotal = 0;
	for (int i = 0; i < static_cast<int>(_countof(managementLabels)); ++i)
	{
		managementWidths[i] = MeasureText(managementLabels[i]).cx + buttonPadding * 2;
		managementTotal += managementWidths[i];
	}
	const int oneRowWidth = managementTotal + gap * (static_cast<int>(_countof(managementLabels)) - 1);
	const int managementRows = oneRowWidth <= availableWidth ? 1 : 2;

	const int okWidth = MeasureText(_T("OK")).cx + buttonPadding * 2;
	const int cancelWidth = MeasureText(_T("Cancel")).cx + buttonPadding * 2;
	const int commandRowsHeight = managementRows * buttonHeight + (managementRows - 1) * gap;
	const int bottomY = cy - padding;
	const int okY = bottomY - buttonHeight;
	const int managementBottom = okY - gap;
	const int managementTop = managementBottom - commandRowsHeight;
	const int patternTop = managementTop - gap - rowHeight;
	const int nameTop = patternTop - gap - rowHeight;
	const int listTop = padding;
	const int listBottom = (std::max)(listTop + textHeight * 2, nameTop - gap);
	const int listHeight = (std::max)(textHeight * 2, listBottom - listTop);

	MoveControl(IDC_LIST_PRESETS, padding, listTop, availableWidth, listHeight);

	const int nameLabelWidth = MeasureText(_T("Process name")).cx + gap;
	const int editLeft = padding + nameLabelWidth;
	const int editWidth = (std::max)(1, cx - editLeft - padding);
	MoveControl(IDC_STATIC_PRESET_NAME_LABEL, padding, nameTop + (rowHeight - labelHeight) / 2,
		nameLabelWidth - gap, labelHeight);
	MoveControl(IDC_EDIT_PRESET_NAME, editLeft, nameTop, editWidth, rowHeight);
	MoveControl(IDC_STATIC_PRESET_PATTERN_LABEL, padding, patternTop + (rowHeight - labelHeight) / 2,
		nameLabelWidth - gap, labelHeight);
	MoveControl(IDC_EDIT_PRESET_PATTERN, editLeft, patternTop, editWidth, rowHeight);

	for (int row = 0; row < managementRows; ++row)
	{
		const int first = row == 0 ? 0 : 3;
		const int count = managementRows == 1 ? 6 : 3;
		int rowWidth = 0;
		for (int i = 0; i < count; ++i)
		{
			rowWidth += managementWidths[first + i];
		}
		rowWidth += gap * (count - 1);
		const bool fitPreferred = rowWidth <= availableWidth;
		const int equalWidth = (std::max)(1, (availableWidth - gap * (count - 1)) / count);
		int left = padding;
		const int top = managementTop + row * (buttonHeight + gap);
		for (int i = 0; i < count; ++i)
		{
			const int index = first + i;
			const int width = fitPreferred ? managementWidths[index] : equalWidth;
			MoveControl(managementIds[index], left, top, width, buttonHeight);
			left += width + gap;
		}
	}

	const int okCancelGap = gap;
	const int okCancelWidth = okWidth + okCancelGap + cancelWidth;
	const int actionLeft = (std::max)(padding, cx - padding - okCancelWidth);
	MoveControl(IDOK, actionLeft, okY, okWidth, buttonHeight);
	MoveControl(IDCANCEL, actionLeft + okWidth + okCancelGap, okY, cancelWidth, buttonHeight);

	if (m_presetList.GetSafeHwnd() != NULL)
	{
		const int listWidth = (std::max)(1, availableWidth);
		const int presetWidth = listWidth * 35 / 100;
		m_presetList.SetColumnWidth(0, presetWidth);
		m_presetList.SetColumnWidth(1, (std::max)(1, listWidth - presetWidth));
	}
}

void CPresetManagerDlg::RefreshList()
{
	if (m_presetList.GetSafeHwnd() == NULL)
	{
		return;
	}

	m_presetList.DeleteAllItems();
	for (int i = 0; i < static_cast<int>(m_collection.presets.size()); ++i)
	{
		const FanPreset& preset = m_collection.presets[static_cast<size_t>(i)];
		const int item = m_presetList.InsertItem(i, Utf8Text(preset.name));
		m_presetList.SetItemText(item, 1, Utf8Text(preset.processPattern));
	}
	UpdateCommandState();
}

void CPresetManagerDlg::SelectRow(int index)
{
	if (index < 0 || index >= static_cast<int>(m_collection.presets.size()))
	{
		index = -1;
	}
	m_selectedIndex = index;
	for (int i = 0; i < m_presetList.GetItemCount(); ++i)
	{
		const UINT state = i == m_selectedIndex ? LVIS_SELECTED | LVIS_FOCUSED : 0;
		m_presetList.SetItemState(i, state, LVIS_SELECTED | LVIS_FOCUSED);
	}
	if (m_selectedIndex >= 0)
	{
		m_presetList.EnsureVisible(m_selectedIndex, FALSE);
	}
	SyncEditorFromSelection();
	UpdateCommandState();
}

void CPresetManagerDlg::SyncEditorFromSelection()
{
	if (m_selectedIndex >= 0 && m_selectedIndex < static_cast<int>(m_collection.presets.size()))
	{
		const FanPreset& preset = m_collection.presets[static_cast<size_t>(m_selectedIndex)];
		m_presetName.SetWindowText(Utf8Text(preset.name));
		m_presetPattern.SetWindowText(Utf8Text(preset.processPattern));
	}
	else
	{
		m_presetName.SetWindowText(_T(""));
		m_presetPattern.SetWindowText(_T(""));
	}
}

void CPresetManagerDlg::UpdateCommandState()
{
	const bool hasSelection = m_selectedIndex >= 0 &&
		m_selectedIndex < static_cast<int>(m_collection.presets.size());
	m_presetNew.EnableWindow(m_collection.presets.size() < 32U);
	m_presetSaveRule.EnableWindow(hasSelection);
	m_presetDelete.EnableWindow(hasSelection);
	m_presetMoveUp.EnableWindow(hasSelection && m_selectedIndex > 0);
	m_presetMoveDown.EnableWindow(hasSelection &&
		m_selectedIndex + 1 < static_cast<int>(m_collection.presets.size()));
	m_presetUse.EnableWindow(hasSelection);
}

CString CPresetManagerDlg::ValidationText(const std::string& error) const
{
	return error.empty() ? CString(_T("The preset collection is invalid.")) : Utf8Text(error);
}

BOOL CPresetManagerDlg::ValidateCandidate(const PresetCollection& candidate) const
{
	std::string error;
	if (candidate.Validate(&error))
	{
		return TRUE;
	}
	AfxMessageBox(ValidationText(error), MB_ICONWARNING);
	return FALSE;
}

BOOL CPresetManagerDlg::ReadSelectedRule(FanPreset* rule) const
{
	if (rule == NULL || m_selectedIndex < 0 ||
		m_selectedIndex >= static_cast<int>(m_collection.presets.size()))
	{
		return FALSE;
	}

	CString name;
	CString pattern;
	m_presetName.GetWindowText(name);
	m_presetPattern.GetWindowText(pattern);
	std::string nameUtf8;
	std::string patternUtf8;
	if (!WideToUtf8(std::wstring(name.GetString()), &nameUtf8) ||
		!WideToUtf8(std::wstring(pattern.GetString()), &patternUtf8))
	{
		return FALSE;
	}
	rule->name.swap(nameUtf8);
	rule->processPattern.swap(patternUtf8);
	return TRUE;
}

BOOL CPresetManagerDlg::CommitSelectedRule(PresetCollection* candidate) const
{
	if (candidate == NULL)
	{
		return FALSE;
	}
	FanPreset rule;
	if (!ReadSelectedRule(&rule))
	{
		AfxMessageBox(_T("Select a preset before editing its rule."), MB_ICONWARNING);
		return FALSE;
	}
	rule.config = candidate->presets[static_cast<size_t>(m_selectedIndex)].config;
	PresetCollection updated = *candidate;
	updated.presets[static_cast<size_t>(m_selectedIndex)] = rule;
	if (!ValidateCandidate(updated))
	{
		return FALSE;
	}
	*candidate = updated;
	return TRUE;
}

BOOL CPresetManagerDlg::CommitPendingEdits(PresetCollection* candidate) const
{
	if (candidate == NULL)
	{
		return FALSE;
	}
	if (m_selectedIndex >= 0)
	{
		return CommitSelectedRule(candidate);
	}
	return ValidateCandidate(*candidate);
}

void CPresetManagerDlg::OnSize(UINT nType, int cx, int cy)
{
	CDialogEx::OnSize(nType, cx, cy);
	LayoutControls(cx, cy);
}

void CPresetManagerDlg::OnGetMinMaxInfo(MINMAXINFO* lpMMI)
{
	CDialogEx::OnGetMinMaxInfo(lpMMI);

	const int textHeight = TextHeight();
	const int padding = (std::max)(6, textHeight / 2);
	const int gap = (std::max)(4, textHeight / 2);
	const int rowHeight = (std::max)(textHeight + textHeight / 2, textHeight + 6);
	const int buttonPadding = (std::max)(8, textHeight / 2 + 4);
	const LPCTSTR labels[] = {
		_T("New from current"), _T("Save rule"), _T("Delete"),
		_T("Move up"), _T("Move down"), _T("Use selected")
	};
	int widths[_countof(labels)] = {};
	for (int i = 0; i < static_cast<int>(_countof(labels)); ++i)
	{
		widths[i] = MeasureText(labels[i]).cx + buttonPadding * 2;
	}
	const int firstManagementRowWidth = widths[0] + widths[1] + widths[2] + gap * 2;
	const int secondManagementRowWidth = widths[3] + widths[4] + widths[5] + gap * 2;
	const int labelWidth = MeasureText(_T("Process name")).cx + gap;
	const int editWidth = MeasureText(_T("Preset process name")).cx + buttonPadding * 2;
	const int okCancelWidth = MeasureText(_T("OK")).cx + buttonPadding * 2 + gap +
		MeasureText(_T("Cancel")).cx + buttonPadding * 2;
	const int clientWidth = (std::max)((std::max)(firstManagementRowWidth, secondManagementRowWidth),
		(std::max)(padding * 2 + labelWidth + editWidth, okCancelWidth));
	const int managementRowsHeight = rowHeight * 2 + gap;
	const int clientHeight = padding + textHeight * 2 + gap + rowHeight + gap + rowHeight +
		gap + managementRowsHeight + gap + rowHeight + padding;

	CRect windowRect;
	CRect clientRect;
	GetWindowRect(&windowRect);
	GetClientRect(&clientRect);
	const int nonClientWidth = (std::max)(0, windowRect.Width() - clientRect.Width());
	const int nonClientHeight = (std::max)(0, windowRect.Height() - clientRect.Height());
	lpMMI->ptMinTrackSize.x = (std::max)(lpMMI->ptMinTrackSize.x,
		static_cast<LONG>(clientWidth + nonClientWidth));
	lpMMI->ptMinTrackSize.y = (std::max)(lpMMI->ptMinTrackSize.y,
		static_cast<LONG>(clientHeight + nonClientHeight));
}

void CPresetManagerDlg::OnLvnItemchangedPresetList(NMHDR*, LRESULT* pResult)
{
	if (pResult != NULL)
	{
		*pResult = 0;
	}
	const int selected = m_presetList.GetNextItem(-1, LVNI_SELECTED);
	if (selected != m_selectedIndex)
	{
		m_selectedIndex = selected;
		SyncEditorFromSelection();
		UpdateCommandState();
	}
}

void CPresetManagerDlg::OnBnClickedPresetNew()
{
	if (m_collection.presets.size() >= 32U)
	{
		AfxMessageBox(_T("A preset collection can contain at most 32 presets."), MB_ICONWARNING);
		return;
	}

	PresetCollection candidate = m_collection;
	FanPreset preset;
	FanConfig config;
	config.LoadDefault();
	CopyControlSettings(m_currentConfig, &config);
	preset.config = config;
	preset.processPattern = "*";
	for (int suffix = 1; ; ++suffix)
	{
		CString name;
		name.Format(_T("Preset %d"), suffix);
		std::string candidateName;
		if (!WideToUtf8(std::wstring(name.GetString()), &candidateName))
		{
			return;
		}
		bool exists = false;
		for (size_t i = 0; i < candidate.presets.size(); ++i)
		{
			if (candidate.presets[i].name == candidateName)
			{
				exists = true;
				break;
			}
		}
		if (!exists)
		{
			preset.name = candidateName;
			break;
		}
	}
	candidate.presets.push_back(preset);
	if (!ValidateCandidate(candidate))
	{
		return;
	}
	m_collection = candidate;
	RefreshList();
	SelectRow(static_cast<int>(m_collection.presets.size()) - 1);
	m_presetName.SetFocus();
}

void CPresetManagerDlg::OnBnClickedPresetSaveRule()
{
	PresetCollection candidate = m_collection;
	if (!CommitSelectedRule(&candidate))
	{
		return;
	}
	m_collection = candidate;
	RefreshList();
	SelectRow(m_selectedIndex);
}

void CPresetManagerDlg::OnBnClickedPresetDelete()
{
	if (m_selectedIndex < 0 || m_selectedIndex >= static_cast<int>(m_collection.presets.size()))
	{
		return;
	}
	const int deletedIndex = m_selectedIndex;
	PresetCollection candidate = m_collection;
	candidate.presets.erase(candidate.presets.begin() + deletedIndex);
	if (!ValidateCandidate(candidate))
	{
		return;
	}
	m_collection = candidate;
	if (deletedIndex == m_activeIndex)
	{
		m_activeIndex = -1;
	}
	else if (deletedIndex < m_activeIndex)
	{
		--m_activeIndex;
	}
	RefreshList();
	SelectRow((std::min)(deletedIndex, static_cast<int>(m_collection.presets.size()) - 1));
}

void CPresetManagerDlg::OnBnClickedPresetMoveUp()
{
	if (m_selectedIndex <= 0 || m_selectedIndex >= static_cast<int>(m_collection.presets.size()))
	{
		return;
	}
	const int selected = m_selectedIndex;
	PresetCollection candidate = m_collection;
	std::swap(candidate.presets[static_cast<size_t>(selected)],
		candidate.presets[static_cast<size_t>(selected - 1)]);
	if (!ValidateCandidate(candidate))
	{
		return;
	}
	m_collection = candidate;
	if (m_activeIndex == selected)
	{
		m_activeIndex = selected - 1;
	}
	else if (m_activeIndex == selected - 1)
	{
		m_activeIndex = selected;
	}
	RefreshList();
	SelectRow(selected - 1);
}

void CPresetManagerDlg::OnBnClickedPresetMoveDown()
{
	if (m_selectedIndex < 0 || m_selectedIndex + 1 >= static_cast<int>(m_collection.presets.size()))
	{
		return;
	}
	const int selected = m_selectedIndex;
	PresetCollection candidate = m_collection;
	std::swap(candidate.presets[static_cast<size_t>(selected)],
		candidate.presets[static_cast<size_t>(selected + 1)]);
	if (!ValidateCandidate(candidate))
	{
		return;
	}
	m_collection = candidate;
	if (m_activeIndex == selected)
	{
		m_activeIndex = selected + 1;
	}
	else if (m_activeIndex == selected + 1)
	{
		m_activeIndex = selected;
	}
	RefreshList();
	SelectRow(selected + 1);
}

void CPresetManagerDlg::OnBnClickedPresetUse()
{
	if (m_selectedIndex < 0 || m_selectedIndex >= static_cast<int>(m_collection.presets.size()))
	{
		AfxMessageBox(_T("Select a preset to apply."), MB_ICONWARNING);
		return;
	}
	PresetCollection candidate = m_collection;
	if (!CommitPendingEdits(&candidate))
	{
		return;
	}
	m_collection = candidate;
	m_activeIndex = m_selectedIndex;
	m_applyRequested = TRUE;
	m_applyIndex = m_selectedIndex;
	EndDialog(IDOK);
}

void CPresetManagerDlg::OnOK()
{
	PresetCollection candidate = m_collection;
	if (!CommitPendingEdits(&candidate))
	{
		return;
	}
	m_collection = candidate;
	m_applyRequested = FALSE;
	m_applyIndex = -1;
	CDialogEx::OnOK();
}

void CPresetManagerDlg::OnCancel()
{
	CDialogEx::OnCancel();
}
