#include "FanConfig.h"

namespace
{
bool Fail(std::string* error, const std::string& message)
{
	if (error != nullptr)
	{
		*error = message;
	}
	return false;
}
}

FanConfig::FanConfig()
{
	LoadDefault();
}

void FanConfig::LoadDefault()
{
	CpuCurve = MakeDefaultFanCurve();
	GpuCurve = MakeDefaultFanCurve();
	TransitionTemp = 3;
	UpdateInterval = 2;
	ForceTemp = 50;
	UiFontSize = FAN_UI_FONT_SIZE_DEFAULT;
	Linear = false;
	TakeOver = false;
	ForceCooling = false;
	SoftControl = false;
	AutoRun = false;
	CloseToTray = false;
}

bool FanConfig::Validate(std::string* error) const
{
	if (error != nullptr)
	{
		error->clear();
	}

	std::string curveError;
	if (!ValidateFanCurve(CpuCurve, &curveError))
	{
		return Fail(error, "CPU curve: " + curveError);
	}
	if (!ValidateFanCurve(GpuCurve, &curveError))
	{
		return Fail(error, "GPU curve: " + curveError);
	}
	if (UpdateInterval < 1 || UpdateInterval > 5)
	{
		return Fail(error, "update interval must be between 1 and 5");
	}
	if (TransitionTemp < 0 || TransitionTemp > 10)
	{
		return Fail(error, "transition temperature must be between 0 and 10");
	}
	if (ForceTemp < 0 || ForceTemp > 100)
	{
		return Fail(error, "force temperature must be between 0 and 100");
	}
	if (UiFontSize < FAN_UI_FONT_SIZE_MIN || UiFontSize > FAN_UI_FONT_SIZE_MAX)
	{
		return Fail(error, "UI font size must be between 8 and 16");
	}

	return true;
}
