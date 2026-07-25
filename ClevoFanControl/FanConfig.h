#ifndef FAN_CONFIG_H
#define FAN_CONFIG_H

#include "FanCurveModel.h"

#include <string>

static const int FAN_UI_FONT_SIZE_MIN = 8;
static const int FAN_UI_FONT_SIZE_MAX = 16;
static const int FAN_UI_FONT_SIZE_DEFAULT = 10;

struct FanConfig
{
	FanCurvePoints CpuCurve;
	FanCurvePoints GpuCurve;
	int TransitionTemp;
	int UpdateInterval;
	int ForceTemp;
	int UiFontSize;
	bool Linear;
	bool TakeOver;
	bool ForceCooling;
	bool SoftControl;
	bool AutoRun;
	bool CloseToTray;

	FanConfig();
	void LoadDefault();
	bool Validate(std::string* error) const;
};

#endif
