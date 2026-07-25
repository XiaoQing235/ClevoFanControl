
// ClevoFanControl.h
//

#pragma once

#ifndef __AFXWIN_H__
	#error "Include stdafx.h before this header so the precompiled header is available"
#endif

#include "resource.h" // Resource identifiers


// CClevoFanControlApp:
// Implementation is in ClevoFanControl.cpp
//

class CClevoFanControlApp : public CWinApp
{
public:
	CClevoFanControlApp();

// Overrides
public:
	virtual BOOL InitInstance();

// Implementation

	DECLARE_MESSAGE_MAP()

};

extern CClevoFanControlApp theApp;
