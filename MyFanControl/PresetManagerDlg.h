#pragma once

#include "PresetStore.h"
#include "Resource.h"
#include "afxcmn.h"
#include "afxwin.h"

class CPresetManagerDlg : public CDialogEx
{
public:
	CPresetManagerDlg(const PresetCollection& collection, int activeIndex,
		const FanConfig& currentConfig, HFONT ownerFont, CWnd* parent = NULL);

	enum { IDD = IDD_PRESET_MANAGER_DIALOG };

	const PresetCollection& GetCollection() const;
	int GetActiveIndex() const;
	BOOL IsApplyRequested() const;
	int GetApplyIndex() const;

protected:
	virtual void DoDataExchange(CDataExchange* pDX);
	virtual BOOL OnInitDialog();
	virtual void OnOK();
	virtual void OnCancel();

	afx_msg void OnSize(UINT nType, int cx, int cy);
	afx_msg void OnGetMinMaxInfo(MINMAXINFO* lpMMI);
	afx_msg void OnLvnItemchangedPresetList(NMHDR* pNMHDR, LRESULT* pResult);
	afx_msg void OnBnClickedPresetNew();
	afx_msg void OnBnClickedPresetSaveRule();
	afx_msg void OnBnClickedPresetDelete();
	afx_msg void OnBnClickedPresetMoveUp();
	afx_msg void OnBnClickedPresetMoveDown();
	afx_msg void OnBnClickedPresetUse();

	DECLARE_MESSAGE_MAP()

private:
	PresetCollection m_collection;
	FanConfig m_currentConfig;
	HFONT m_ownerFont;
	int m_activeIndex;
	int m_selectedIndex;
	BOOL m_applyRequested;
	int m_applyIndex;

	CListCtrl m_presetList;
	CEdit m_presetName;
	CEdit m_presetPattern;
	CButton m_presetNew;
	CButton m_presetSaveRule;
	CButton m_presetDelete;
	CButton m_presetMoveUp;
	CButton m_presetMoveDown;
	CButton m_presetUse;

	void ApplyOwnerFont();
	HFONT ActiveFont() const;
	CSize MeasureText(LPCTSTR text) const;
	int TextHeight() const;
	void LayoutControls(int cx, int cy);
	void MoveControl(int id, int left, int top, int width, int height);
	void RefreshList();
	void SelectRow(int index);
	void SyncEditorFromSelection();
	void UpdateCommandState();
	BOOL ValidateCandidate(const PresetCollection& candidate) const;
	BOOL ReadSelectedRule(FanPreset* rule) const;
	BOOL CommitSelectedRule(PresetCollection* candidate) const;
	BOOL CommitPendingEdits(PresetCollection* candidate) const;
	CString ValidationText(const std::string& error) const;
};
