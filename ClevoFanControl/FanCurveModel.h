#ifndef FAN_CURVE_MODEL_H
#define FAN_CURVE_MODEL_H

#include <stddef.h>

#include <string>
#include <vector>

struct FanCurvePoint
{
	int temperature;
	int duty;
};

typedef std::vector<FanCurvePoint> FanCurvePoints;

static const size_t FAN_CURVE_MIN_POINTS = 2;
static const size_t FAN_CURVE_MAX_POINTS = 16;
static const int FAN_CURVE_MIN_TEMP = 30;
static const int FAN_CURVE_MAX_TEMP = 100;
static const int FAN_CURVE_MIN_DUTY = 0;
static const int FAN_CURVE_MAX_DUTY = 100;

bool ValidateFanCurve(const FanCurvePoints& points, std::string* error);
int EvaluateLinearDuty(const FanCurvePoints& points, int temperature);
int EvaluateStepDuty(const FanCurvePoints& points, int temperature, int transitionTemp, int* selectedIndex);
bool TrySetFanCurvePoint(FanCurvePoints* points, size_t index, int temperature, int duty, std::string* error);
bool TryInsertFanCurvePoint(FanCurvePoints* points, int temperature, int duty, std::string* error);
bool TryDeleteFanCurvePoint(FanCurvePoints* points, size_t index, std::string* error);
FanCurvePoints MakeDefaultFanCurve();

#endif
