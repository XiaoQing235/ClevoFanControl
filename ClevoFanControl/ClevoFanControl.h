
// ClevoFanControl.h : PROJECT_NAME Ӧ�ó������ͷ�ļ�
//

#pragma once

#ifndef __AFXWIN_H__
	#error "�ڰ������ļ�֮ǰ������stdafx.h�������� PCH �ļ�"
#endif

#include "resource.h"		// ������


// CClevoFanControlApp:
// �йش����ʵ�֣������ ClevoFanControl.cpp
//

class CClevoFanControlApp : public CWinApp
{
public:
	CClevoFanControlApp();

// ��д
public:
	virtual BOOL InitInstance();

// ʵ��

	DECLARE_MESSAGE_MAP()

};

extern CClevoFanControlApp theApp;