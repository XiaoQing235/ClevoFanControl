#include "stdafx.h"
#include "FanCurveCtrl.h"

#include <algorithm>

#ifdef _DEBUG
#define new DEBUG_NEW
#endif

namespace
{
const int kPointHitRadius = 8;
}

BEGIN_MESSAGE_MAP(CFanCurveCtrl, CWnd)
	ON_WM_ERASEBKGND()
	ON_WM_PAINT()
	ON_WM_SIZE()
	ON_WM_LBUTTONDOWN()
	ON_WM_LBUTTONUP()
	ON_WM_LBUTTONDBLCLK()
	ON_WM_MOUSEMOVE()
	ON_WM_KEYDOWN()
	ON_WM_CAPTURECHANGED()
	ON_WM_CANCELMODE()
	ON_WM_DESTROY()
END_MESSAGE_MAP()

CFanCurveCtrl::CFanCurveCtrl()
	: m_selectedIndex(-1)
	, m_curveId(0)
	, m_dragging(false)
	, m_dragIndex(-1)
	, m_currentTemperature(-1)
{
}

CFanCurveCtrl::~CFanCurveCtrl()
{
}

BOOL CFanCurveCtrl::Create(DWORD dwStyle, const RECT& rect, CWnd* pParentWnd, UINT nID)
{
	LPCTSTR className = AfxRegisterWndClass(
		CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS,
		::LoadCursor(NULL, IDC_ARROW),
		reinterpret_cast<HBRUSH>(static_cast<INT_PTR>(COLOR_WINDOW + 1)),
		NULL);
	if (className == NULL)
	{
		return FALSE;
	}

	return CWnd::Create(className, _T(""), dwStyle, rect, pParentWnd, nID);
}

BOOL CFanCurveCtrl::Create(const RECT& rect, CWnd* pParentWnd, UINT nID, DWORD dwStyle)
{
	return Create(dwStyle, rect, pParentWnd, nID);
}

void CFanCurveCtrl::SetCurve(const FanCurvePoints& points)
{
	const int previousIndex = m_selectedIndex;
	m_curve = points;
	SetSelectedIndex(previousIndex);
	m_dragging = false;
	m_dragIndex = -1;
	ReleaseMouseCapture();
	if (GetSafeHwnd() != NULL)
	{
		Invalidate(FALSE);
	}
}

FanCurvePoints CFanCurveCtrl::GetCurve() const
{
	return m_curve;
}

void CFanCurveCtrl::SetSelectedIndex(int index)
{
	if (m_curve.empty() || index < 0)
	{
		m_selectedIndex = -1;
	}
	else
	{
		m_selectedIndex = (std::min)(index, static_cast<int>(m_curve.size()) - 1);
	}
	if (GetSafeHwnd() != NULL)
	{
		Invalidate(FALSE);
	}
}

void CFanCurveCtrl::SetCurrentTemperature(int temperature)
{
	m_currentTemperature = temperature;
	if (GetSafeHwnd() != NULL)
	{
		Invalidate(FALSE);
	}
}

int CFanCurveCtrl::GetSelectedIndex() const
{
	return m_selectedIndex;
}

void CFanCurveCtrl::SetCurveId(int curveId)
{
	m_curveId = curveId;
}

BOOL CFanCurveCtrl::OnEraseBkgnd(CDC* /*pDC*/)
{
	return TRUE;
}

void CFanCurveCtrl::OnPaint()
{
	CPaintDC dc(this);
	CRect clientRect;
	GetClientRect(&clientRect);
	if (clientRect.IsRectEmpty())
	{
		return;
	}

	CDC memoryDC;
	if (!memoryDC.CreateCompatibleDC(&dc))
	{
		return;
	}
	CBitmap memoryBitmap;
	if (!memoryBitmap.CreateCompatibleBitmap(&dc,
		clientRect.Width(), clientRect.Height()))
	{
		return;
	}
	CBitmap* oldBitmap = memoryDC.SelectObject(&memoryBitmap);
	if (oldBitmap == NULL)
	{
		return;
	}

	CBrush backgroundBrush(RGB(255, 255, 255));
	memoryDC.FillRect(&clientRect, &backgroundBrush);
	CBrush* oldBrush = memoryDC.SelectObject(&backgroundBrush);
	memoryDC.SetBkMode(TRANSPARENT);

	CFont* chartFont = GetFont();
	CFont* oldFont = chartFont != NULL
		? memoryDC.SelectObject(chartFont)
		: memoryDC.SelectObject(CFont::FromHandle(static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT))));
	int textHeight = 0;
	int dutyWidth = 0;
	int temperatureWidth = 0;
	GetChartTextMetrics(memoryDC, &textHeight, &dutyWidth, &temperatureWidth, NULL);
	const int textGap = (std::max)(4, textHeight / 3);
	const CRect graphRect = GetGraphRect(clientRect);
	CPen gridPen(PS_SOLID, 1, RGB(225, 225, 225));
	CPen axisPen(PS_SOLID, 1, RGB(70, 70, 70));
	CPen* oldPen = memoryDC.SelectObject(&gridPen);

	for (int temperature = FAN_CURVE_MIN_TEMP;
		temperature <= FAN_CURVE_MAX_TEMP;
		temperature += 10)
	{
		const int x = TemperatureToX(graphRect, temperature);
		memoryDC.MoveTo(x, graphRect.top);
		memoryDC.LineTo(x, graphRect.bottom);
	}
	for (int duty = FAN_CURVE_MIN_DUTY;
		duty <= FAN_CURVE_MAX_DUTY;
		duty += 10)
	{
		const int y = DutyToY(graphRect, duty);
		memoryDC.MoveTo(graphRect.left, y);
		memoryDC.LineTo(graphRect.right, y);
	}

	memoryDC.SelectObject(&axisPen);
	memoryDC.MoveTo(graphRect.left, graphRect.top);
	memoryDC.LineTo(graphRect.left, graphRect.bottom);
	memoryDC.LineTo(graphRect.right, graphRect.bottom);

	memoryDC.SetTextColor(RGB(70, 70, 70));
	for (int temperature = FAN_CURVE_MIN_TEMP;
		temperature <= FAN_CURVE_MAX_TEMP;
		temperature += 10)
	{
		CString label;
		label.Format(_T("%d"), temperature);
		const int x = TemperatureToX(graphRect, temperature);
		const int labelWidth = memoryDC.GetTextExtent(label).cx;
		memoryDC.TextOut(x - labelWidth / 2, graphRect.bottom + textGap, label);
	}
	for (int duty = FAN_CURVE_MIN_DUTY;
		duty <= FAN_CURVE_MAX_DUTY;
		duty += 10)
	{
		CString label;
		label.Format(_T("%d"), duty);
		const int y = DutyToY(graphRect, duty);
		const int labelWidth = memoryDC.GetTextExtent(label).cx;
		memoryDC.TextOut(graphRect.left - textGap - labelWidth,
			y - textHeight / 2, label);
	}
	memoryDC.TextOut(graphRect.left + (graphRect.Width() - temperatureWidth) / 2,
		graphRect.bottom + textGap + textHeight + textGap, _T("Temperature (C)"));
	memoryDC.TextOut(graphRect.left - textGap - dutyWidth,
		graphRect.top - textGap - textHeight, _T("Duty (%)"));

	if (m_currentTemperature >= FAN_CURVE_MIN_TEMP &&
		m_currentTemperature <= FAN_CURVE_MAX_TEMP)
	{
		CPen currentPen(PS_DASH, 1, RGB(180, 80, 80));
		memoryDC.SelectObject(&currentPen);
		const int x = TemperatureToX(graphRect, m_currentTemperature);
		memoryDC.MoveTo(x, graphRect.top);
		memoryDC.LineTo(x, graphRect.bottom);
	}

	if (m_curve.size() >= 2)
	{
		CPen curvePen(PS_SOLID, 2, RGB(35, 95, 155));
		memoryDC.SelectObject(&curvePen);
		for (size_t i = 1; i < m_curve.size(); ++i)
		{
			memoryDC.MoveTo(TemperatureToX(graphRect, m_curve[i - 1].temperature),
				DutyToY(graphRect, m_curve[i - 1].duty));
			memoryDC.LineTo(TemperatureToX(graphRect, m_curve[i].temperature),
				DutyToY(graphRect, m_curve[i].duty));
		}
	}

	CBrush normalBrush(RGB(35, 95, 155));
	CBrush selectedBrush(RGB(220, 80, 60));
	CPen normalPointPen(PS_SOLID, 1, RGB(20, 60, 105));
	CPen selectedPointPen(PS_SOLID, 2, RGB(150, 40, 30));
	for (size_t i = 0; i < m_curve.size(); ++i)
	{
		const bool selected = static_cast<int>(i) == m_selectedIndex;
		const int x = TemperatureToX(graphRect, m_curve[i].temperature);
		const int y = DutyToY(graphRect, m_curve[i].duty);
		memoryDC.SelectObject(selected ? &selectedPointPen : &normalPointPen);
		memoryDC.SelectObject(selected ? &selectedBrush : &normalBrush);
		memoryDC.Ellipse(x - 5, y - 5, x + 6, y + 6);
	}

	memoryDC.SelectObject(oldFont);
	memoryDC.SelectObject(oldBrush);
	memoryDC.SelectObject(oldPen);
	dc.BitBlt(clientRect.left, clientRect.top,
		clientRect.Width(), clientRect.Height(), &memoryDC, 0, 0, SRCCOPY);
	memoryDC.SelectObject(oldBitmap);
}

void CFanCurveCtrl::OnSize(UINT nType, int cx, int cy)
{
	CWnd::OnSize(nType, cx, cy);
	Invalidate(FALSE);
}

void CFanCurveCtrl::OnLButtonDown(UINT /*nFlags*/, CPoint point)
{
	SetFocus();
	const int previousSelectedIndex = m_selectedIndex;
	const int hitIndex = HitTest(point);
	if (hitIndex >= 0)
	{
		m_selectedIndex = hitIndex;
		m_dragIndex = hitIndex;
		m_dragging = true;
		SetCapture();
	}
	else
	{
		m_selectedIndex = -1;
		m_dragIndex = -1;
		m_dragging = false;
	}
	if (m_selectedIndex != previousSelectedIndex)
	{
		NotifyCurveChanged();
	}
	Invalidate(FALSE);
}

void CFanCurveCtrl::OnLButtonUp(UINT nFlags, CPoint point)
{
	ReleaseMouseCapture();
	CWnd::OnLButtonUp(nFlags, point);
}

void CFanCurveCtrl::OnLButtonDblClk(UINT /*nFlags*/, CPoint point)
{
	SetFocus();
	if (HitTest(point) < 0)
	{
		InsertPointAt(point);
	}
}

void CFanCurveCtrl::OnMouseMove(UINT nFlags, CPoint point)
{
	if (m_dragging && m_dragIndex >= 0 && GetCapture() == this)
	{
		const CRect clientRect = [&]()
		{
			CRect rect;
			GetClientRect(&rect);
			return rect;
		}();
		const CRect graphRect = GetGraphRect(clientRect);
		const int temperature = ClampTemperature(
			m_dragIndex, XToTemperature(graphRect, point.x));
		const int duty = ClampDuty(YToDuty(graphRect, point.y));
		ApplyPointEdit(m_dragIndex, temperature, duty);
	}
	CWnd::OnMouseMove(nFlags, point);
}

void CFanCurveCtrl::OnKeyDown(UINT nChar, UINT /*nRepCnt*/, UINT /*nFlags*/)
{
	if (nChar == VK_DELETE)
	{
		DeleteSelectedPoint();
		return;
	}

	if (m_selectedIndex < 0 || m_selectedIndex >= static_cast<int>(m_curve.size()))
	{
		CWnd::OnKeyDown(nChar, 1, 0);
		return;
	}

	const int step = (GetKeyState(VK_SHIFT) & 0x8000) != 0 ? 5 : 1;
	int temperature = m_curve[m_selectedIndex].temperature;
	int duty = m_curve[m_selectedIndex].duty;
	switch (nChar)
	{
	case VK_LEFT:
		temperature -= step;
		break;
	case VK_RIGHT:
		temperature += step;
		break;
	case VK_UP:
		duty += step;
		break;
	case VK_DOWN:
		duty -= step;
		break;
	default:
		CWnd::OnKeyDown(nChar, 1, 0);
		return;
	}

	ApplyPointEdit(m_selectedIndex,
		ClampTemperature(m_selectedIndex, temperature),
		ClampDuty(duty));
}

void CFanCurveCtrl::OnCaptureChanged(CWnd* pWnd)
{
	m_dragging = false;
	m_dragIndex = -1;
	CWnd::OnCaptureChanged(pWnd);
}

void CFanCurveCtrl::OnCancelMode()
{
	ReleaseMouseCapture();
	CWnd::OnCancelMode();
}

void CFanCurveCtrl::OnDestroy()
{
	ReleaseMouseCapture();
	CWnd::OnDestroy();
}

void CFanCurveCtrl::GetChartTextMetrics(CDC& dc, int* textHeight,
	int* dutyWidth, int* temperatureWidth, int* tickWidth) const
{
	CFont* chartFont = GetFont();
	CFont* oldFont = chartFont != NULL
		? dc.SelectObject(chartFont)
		: dc.SelectObject(CFont::FromHandle(static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT))));

	TEXTMETRIC metrics = {};
	dc.GetTextMetrics(&metrics);
	const int measuredTextHeight = (std::max)(1, static_cast<int>(metrics.tmHeight));
	const int measuredDutyWidth = dc.GetTextExtent(_T("Duty (%)")).cx;
	const int measuredTemperatureWidth = dc.GetTextExtent(_T("Temperature (C)")).cx;
	const int measuredTickWidth = dc.GetTextExtent(_T("100")).cx;
	if (textHeight != NULL)
	{
		*textHeight = measuredTextHeight;
	}
	if (dutyWidth != NULL)
	{
		*dutyWidth = measuredDutyWidth;
	}
	if (temperatureWidth != NULL)
	{
		*temperatureWidth = measuredTemperatureWidth;
	}
	if (tickWidth != NULL)
	{
		*tickWidth = measuredTickWidth;
	}

	if (oldFont != NULL)
	{
		dc.SelectObject(oldFont);
	}
}

CRect CFanCurveCtrl::GetGraphRect(const CRect& clientRect) const
{
	CClientDC dc(const_cast<CFanCurveCtrl*>(this));
	int textHeight = 0;
	int dutyWidth = 0;
	int tickWidth = 0;
	GetChartTextMetrics(dc, &textHeight, &dutyWidth, NULL, &tickWidth);
	const int textGap = (std::max)(4, textHeight / 3);
	const int leftMargin = (std::max)(dutyWidth, tickWidth) + textGap;
	const int topMargin = textHeight + textGap + textHeight / 2;
	const int rightMargin = (std::max)(8, (tickWidth + 1) / 2 + textGap);
	const int bottomMargin = textHeight * 2 + textGap * 2 + 4;
	CRect graphRect(clientRect);
	graphRect.left += leftMargin;
	graphRect.top += topMargin;
	graphRect.right -= rightMargin;
	graphRect.bottom -= bottomMargin;
	if (graphRect.right <= graphRect.left)
	{
		graphRect.right = graphRect.left + 1;
	}
	if (graphRect.bottom <= graphRect.top)
	{
		graphRect.bottom = graphRect.top + 1;
	}
	return graphRect;
}

int CFanCurveCtrl::TemperatureToX(const CRect& graphRect, int temperature) const
{
	const int boundedTemperature = (std::max)(FAN_CURVE_MIN_TEMP,
		(std::min)(FAN_CURVE_MAX_TEMP, temperature));
	const long width = graphRect.Width();
	return graphRect.left + static_cast<int>(
		(static_cast<long long>(boundedTemperature - FAN_CURVE_MIN_TEMP) * width) /
		(FAN_CURVE_MAX_TEMP - FAN_CURVE_MIN_TEMP));
}

int CFanCurveCtrl::DutyToY(const CRect& graphRect, int duty) const
{
	const int boundedDuty = (std::max)(FAN_CURVE_MIN_DUTY,
		(std::min)(FAN_CURVE_MAX_DUTY, duty));
	const long height = graphRect.Height();
	return graphRect.bottom - static_cast<int>(
		(static_cast<long long>(boundedDuty - FAN_CURVE_MIN_DUTY) * height) /
		(FAN_CURVE_MAX_DUTY - FAN_CURVE_MIN_DUTY));
}

int CFanCurveCtrl::XToTemperature(const CRect& graphRect, int x) const
{
	const int boundedX = (std::max)(static_cast<int>(graphRect.left),
		(std::min)(static_cast<int>(graphRect.right), x));
	const long width = (std::max)(1L, static_cast<long>(graphRect.Width()));
	const long long temperature = FAN_CURVE_MIN_TEMP +
		(static_cast<long long>(boundedX - graphRect.left) *
		(FAN_CURVE_MAX_TEMP - FAN_CURVE_MIN_TEMP) + width / 2) / width;
	return (std::max)(FAN_CURVE_MIN_TEMP,
		(std::min)(FAN_CURVE_MAX_TEMP, static_cast<int>(temperature)));
}

int CFanCurveCtrl::YToDuty(const CRect& graphRect, int y) const
{
	const int boundedY = (std::max)(static_cast<int>(graphRect.top),
		(std::min)(static_cast<int>(graphRect.bottom), y));
	const long height = (std::max)(1L, static_cast<long>(graphRect.Height()));
	const long long duty = FAN_CURVE_MIN_DUTY +
		(static_cast<long long>(graphRect.bottom - boundedY) *
		(FAN_CURVE_MAX_DUTY - FAN_CURVE_MIN_DUTY) + height / 2) / height;
	return ClampDuty(static_cast<int>(duty));
}

int CFanCurveCtrl::HitTest(CPoint point) const
{
	CRect clientRect;
	GetClientRect(&clientRect);
	const CRect graphRect = GetGraphRect(clientRect);
	for (size_t i = 0; i < m_curve.size(); ++i)
	{
		const int dx = point.x - TemperatureToX(graphRect, m_curve[i].temperature);
		const int dy = point.y - DutyToY(graphRect, m_curve[i].duty);
		if (dx * dx + dy * dy <= kPointHitRadius * kPointHitRadius)
		{
			return static_cast<int>(i);
		}
	}
	return -1;
}

int CFanCurveCtrl::ClampTemperature(int index, int temperature) const
{
	int lower = FAN_CURVE_MIN_TEMP;
	int upper = FAN_CURVE_MAX_TEMP;
	if (index > 0 && index < static_cast<int>(m_curve.size()))
	{
		lower = m_curve[index - 1].temperature + 1;
	}
	if (index >= 0 && index + 1 < static_cast<int>(m_curve.size()))
	{
		upper = m_curve[index + 1].temperature - 1;
	}
	if (lower > upper)
	{
		return m_curve[index].temperature;
	}
	return (std::max)(lower, (std::min)(upper, temperature));
}

int CFanCurveCtrl::ClampDuty(int duty) const
{
	return (std::max)(FAN_CURVE_MIN_DUTY, (std::min)(FAN_CURVE_MAX_DUTY, duty));
}

bool CFanCurveCtrl::IsInGraph(CPoint point) const
{
	CRect clientRect;
	GetClientRect(&clientRect);
	const CRect graphRect = GetGraphRect(clientRect);
	return point.x >= graphRect.left && point.x <= graphRect.right &&
		point.y >= graphRect.top && point.y <= graphRect.bottom;
}

bool CFanCurveCtrl::ApplyPointEdit(int index, int temperature, int duty)
{
	if (index < 0 || index >= static_cast<int>(m_curve.size()))
	{
		return false;
	}

	const FanCurvePoint oldPoint = m_curve[index];
	if (oldPoint.temperature == temperature && oldPoint.duty == duty)
	{
		return false;
	}

	std::string error;
	if (!TrySetFanCurvePoint(&m_curve, static_cast<size_t>(index),
		temperature, duty, &error))
	{
		return false;
	}

	NotifyCurveChanged();
	Invalidate(FALSE);
	return true;
}

bool CFanCurveCtrl::InsertPointAt(CPoint point)
{
	if (!IsInGraph(point) || m_curve.size() >= FAN_CURVE_MAX_POINTS)
	{
		return false;
	}

	CRect clientRect;
	GetClientRect(&clientRect);
	const CRect graphRect = GetGraphRect(clientRect);
	const int requestedTemperature = XToTemperature(graphRect, point.x);
	const int duty = YToDuty(graphRect, point.y);

	for (int offset = 0;
		offset <= FAN_CURVE_MAX_TEMP - FAN_CURVE_MIN_TEMP;
		++offset)
	{
		const int candidates[2] = {
			requestedTemperature - offset,
			requestedTemperature + offset
		};
		const int candidateCount = offset == 0 ? 1 : 2;
		for (int candidateIndex = 0; candidateIndex < candidateCount; ++candidateIndex)
		{
			const int temperature = candidates[candidateIndex];
			if (temperature < FAN_CURVE_MIN_TEMP || temperature > FAN_CURVE_MAX_TEMP)
			{
				continue;
			}

			std::string error;
			if (TryInsertFanCurvePoint(&m_curve, temperature, duty, &error))
			{
				for (size_t i = 0; i < m_curve.size(); ++i)
				{
					if (m_curve[i].temperature == temperature)
					{
						m_selectedIndex = static_cast<int>(i);
						break;
					}
				}
				NotifyCurveChanged();
				Invalidate(FALSE);
				return true;
			}
		}
	}
	return false;
}

bool CFanCurveCtrl::DeleteSelectedPoint()
{
	if (m_selectedIndex < 0 || m_selectedIndex >= static_cast<int>(m_curve.size()))
	{
		return false;
	}

	std::string error;
	if (!TryDeleteFanCurvePoint(&m_curve, static_cast<size_t>(m_selectedIndex), &error))
	{
		return false;
	}

	if (m_curve.empty())
	{
		m_selectedIndex = -1;
	}
	else if (m_selectedIndex >= static_cast<int>(m_curve.size()))
	{
		m_selectedIndex = static_cast<int>(m_curve.size()) - 1;
	}
	m_dragIndex = -1;
	NotifyCurveChanged();
	Invalidate(FALSE);
	return true;
}

void CFanCurveCtrl::NotifyCurveChanged()
{
	CWnd* parent = GetParent();
	if (parent != NULL && parent->GetSafeHwnd() != NULL)
	{
		parent->SendMessage(WM_FAN_CURVE_CHANGED,
			static_cast<WPARAM>(m_curveId),
			static_cast<LPARAM>(m_selectedIndex));
	}
}

void CFanCurveCtrl::ReleaseMouseCapture()
{
	if (GetCapture() == this)
	{
		ReleaseCapture();
	}
	m_dragging = false;
	m_dragIndex = -1;
}
