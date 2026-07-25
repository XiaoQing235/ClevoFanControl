#include "FanCurveModel.h"

#include <algorithm>
#include <cmath>

namespace
{
bool Fail(std::string* error, const char* message)
{
	if (error != nullptr)
	{
		*error = message;
	}
	return false;
}

void ClearError(std::string* error)
{
	if (error != nullptr)
	{
		error->clear();
	}
}

bool IsPointInRange(const FanCurvePoint& point)
{
	return point.temperature >= FAN_CURVE_MIN_TEMP &&
		point.temperature <= FAN_CURVE_MAX_TEMP &&
		point.duty >= FAN_CURVE_MIN_DUTY &&
		point.duty <= FAN_CURVE_MAX_DUTY;
}
}

bool ValidateFanCurve(const FanCurvePoints& points, std::string* error)
{
	ClearError(error);

	if (points.size() < FAN_CURVE_MIN_POINTS || points.size() > FAN_CURVE_MAX_POINTS)
	{
		return Fail(error, "fan curve must contain between 2 and 16 points");
	}

	for (size_t i = 0; i < points.size(); ++i)
	{
		if (!IsPointInRange(points[i]))
		{
			return Fail(error, "fan curve point is outside the allowed range");
		}

		if (i > 0 && points[i - 1].temperature >= points[i].temperature)
		{
			return Fail(error, "fan curve temperatures must be strictly increasing");
		}
	}

	return true;
}

int EvaluateLinearDuty(const FanCurvePoints& points, int temperature)
{
	if (!ValidateFanCurve(points, nullptr))
	{
		return 0;
	}

	if (temperature <= points.front().temperature)
	{
		return points.front().duty;
	}

	if (temperature >= points.back().temperature)
	{
		return points.back().duty;
	}

	for (size_t i = 1; i < points.size(); ++i)
	{
		if (temperature <= points[i].temperature)
		{
			const FanCurvePoint& lower = points[i - 1];
			const FanCurvePoint& upper = points[i];
			const long long lowerTemperature = lower.temperature;
			const long long upperTemperature = upper.temperature;
			const long long requestedTemperature = temperature;
			const long long temperatureOffset = requestedTemperature - lowerTemperature;
			const long long temperatureSpan = upperTemperature - lowerTemperature;
			const long long dutyDelta = static_cast<long long>(upper.duty) - lower.duty;
			const double fraction = static_cast<double>(temperatureOffset) /
				static_cast<double>(temperatureSpan);
			const double duty = static_cast<double>(lower.duty) +
				static_cast<double>(dutyDelta) * fraction;
			return static_cast<int>(std::lround(duty));
		}
	}

	return points.back().duty;
}

int EvaluateStepDuty(const FanCurvePoints& points, int temperature, int transitionTemp, int* selectedIndex)
{
	if (!ValidateFanCurve(points, nullptr))
	{
		if (selectedIndex != nullptr)
		{
			*selectedIndex = 0;
		}
		return 0;
	}

	int index = selectedIndex != nullptr ? *selectedIndex : 0;
	if (index < 0 || index >= static_cast<int>(points.size()))
	{
		index = 0;
	}

	const long long hysteresis = transitionTemp < 0 ? 0 : transitionTemp;
	if (index + 1 < static_cast<int>(points.size()) &&
		temperature >= points[index + 1].temperature)
	{
		while (index + 1 < static_cast<int>(points.size()) &&
			temperature >= points[index + 1].temperature)
		{
			++index;
		}
	}
	else
	{
		while (index > 0 &&
			static_cast<long long>(temperature) <
				static_cast<long long>(points[index].temperature) - hysteresis)
		{
			--index;
		}
	}

	if (selectedIndex != nullptr)
	{
		*selectedIndex = index;
	}
	return points[index].duty;
}

bool TrySetFanCurvePoint(FanCurvePoints* points, size_t index, int temperature, int duty, std::string* error)
{
	ClearError(error);

	if (points == nullptr)
	{
		return Fail(error, "fan curve pointer is null");
	}
	if (!ValidateFanCurve(*points, error))
	{
		return false;
	}
	if (index >= points->size())
	{
		return Fail(error, "fan curve point index is out of range");
	}

	FanCurvePoints candidate(*points);
	candidate[index] = FanCurvePoint{temperature, duty};
	if (!ValidateFanCurve(candidate, error))
	{
		return false;
	}

	points->swap(candidate);
	return true;
}

bool TryInsertFanCurvePoint(FanCurvePoints* points, int temperature, int duty, std::string* error)
{
	ClearError(error);

	if (points == nullptr)
	{
		return Fail(error, "fan curve pointer is null");
	}
	if (!ValidateFanCurve(*points, error))
	{
		return false;
	}
	if (points->size() >= FAN_CURVE_MAX_POINTS)
	{
		return Fail(error, "fan curve cannot contain more than 16 points");
	}

	const FanCurvePoints::const_iterator insertion = std::lower_bound(
		points->begin(), points->end(), temperature,
		[](const FanCurvePoint& point, int value) { return point.temperature < value; });
	if (insertion != points->end() && insertion->temperature == temperature)
	{
		return Fail(error, "fan curve temperature already exists");
	}

	FanCurvePoints candidate(*points);
	const FanCurvePoints::difference_type insertionIndex = insertion - points->cbegin();
	candidate.insert(candidate.begin() + insertionIndex, FanCurvePoint{temperature, duty});
	if (!ValidateFanCurve(candidate, error))
	{
		return false;
	}

	points->swap(candidate);
	return true;
}

bool TryDeleteFanCurvePoint(FanCurvePoints* points, size_t index, std::string* error)
{
	ClearError(error);

	if (points == nullptr)
	{
		return Fail(error, "fan curve pointer is null");
	}
	if (!ValidateFanCurve(*points, error))
	{
		return false;
	}
	if (index >= points->size())
	{
		return Fail(error, "fan curve point index is out of range");
	}
	if (points->size() <= FAN_CURVE_MIN_POINTS)
	{
		return Fail(error, "fan curve must retain at least 2 points");
	}

	FanCurvePoints candidate(*points);
	candidate.erase(candidate.begin() + static_cast<FanCurvePoints::difference_type>(index));
	if (!ValidateFanCurve(candidate, error))
	{
		return false;
	}

	points->swap(candidate);
	return true;
}

FanCurvePoints MakeDefaultFanCurve()
{
	return FanCurvePoints{
		FanCurvePoint{45, 18},
		FanCurvePoint{50, 18},
		FanCurvePoint{55, 18},
		FanCurvePoint{60, 25},
		FanCurvePoint{65, 30},
		FanCurvePoint{70, 35},
		FanCurvePoint{75, 55},
		FanCurvePoint{80, 70},
		FanCurvePoint{85, 80},
		FanCurvePoint{90, 95}
	};
}
