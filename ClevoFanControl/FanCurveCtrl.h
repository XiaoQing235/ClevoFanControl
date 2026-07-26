#pragma once

#include <afxwin.h>

#include "FanCurveModel.h"

#ifndef WM_FAN_CURVE_CHANGED
#define WM_FAN_CURVE_CHANGED (WM_APP + 40)
#endif

class CFanCurveCtrl : public CWnd
{
public:
	CFanCurveCtrl();
	virtual ~CFanCurveCtrl();

	BOOL Create(DWORD dwStyle, const RECT& rect, CWnd* pParentWnd, UINT nID);
	BOOL Create(const RECT& rect, CWnd* pParentWnd, UINT nID,
		DWORD dwStyle = WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_CLIPSIBLINGS);

	void SetCurve(const FanCurvePoints& points);
	FanCurvePoints GetCurve() const;
	void SetSelectedIndex(int index);
	void SetCurrentTemperature(int temperature);
	int GetSelectedIndex() const;
	void SetCurveId(int curveId);

protected:
	virtual BOOL OnEraseBkgnd(CDC* pDC);
	virtual void OnPaint();
	virtual void OnSize(UINT nType, int cx, int cy);
	virtual void OnLButtonDown(UINT nFlags, CPoint point);
	virtual void OnLButtonUp(UINT nFlags, CPoint point);
	virtual void OnLButtonDblClk(UINT nFlags, CPoint point);
	virtual void OnMouseMove(UINT nFlags, CPoint point);
	virtual UINT OnGetDlgCode();
	virtual void OnKeyDown(UINT nChar, UINT nRepCnt, UINT nFlags);
	virtual void OnCaptureChanged(CWnd* pWnd);
	virtual void OnCancelMode();
	virtual void OnDestroy();

	void GetChartTextMetrics(CDC& dc, int* textHeight, int* dutyWidth,
		int* temperatureWidth, int* tickWidth) const;
	CRect GetGraphRect(const CRect& clientRect) const;
	int TemperatureToX(const CRect& graphRect, int temperature) const;
	int DutyToY(const CRect& graphRect, int duty) const;
	int XToTemperature(const CRect& graphRect, int x) const;
	int YToDuty(const CRect& graphRect, int y) const;
	int HitTest(CPoint point) const;
	int ClampTemperature(int index, int temperature) const;
	int ClampDuty(int duty) const;
	bool IsInGraph(CPoint point) const;
	bool ApplyPointEdit(int index, int temperature, int duty);
	bool InsertPointAt(CPoint point);
	bool DeleteSelectedPoint();
	void NotifyCurveChanged();
	void ReleaseMouseCapture();

	FanCurvePoints m_curve;
	int m_selectedIndex;
	int m_curveId;
	bool m_dragging;
	int m_dragIndex;
	int m_currentTemperature;

	DECLARE_MESSAGE_MAP()
};
