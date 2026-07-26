#include "FanControlLogic.h"

#include <cstdlib>

bool ShouldCompleteForcedCooling(bool enabled, int cpuTemperature,
	int gpuTemperature, int threshold)
{
	return enabled && cpuTemperature < threshold && gpuTemperature < threshold;
}

bool IsCurrentConfigGeneration(long capturedGeneration, long activeGeneration)
{
	return capturedGeneration == activeGeneration;
}

bool ShouldRestartUpdateTimer(int activeInterval, int configuredInterval)
{
	return activeInterval != configuredInterval;
}

bool ShouldRetryTemperatureSample(bool hasBaseline, int currentTemperature,
	int sampledTemperature)
{
	return hasBaseline && std::abs(sampledTemperature - currentTemperature) > 30;
}

int DecodeFanRpmCounter(int counter)
{
	if (counter == 0)
	{
		return 0;
	}
	return counter > 300 && counter < 5000 ? 2100000 / counter : -1;
}

int NormalizeFanCount(int reportedFanCount)
{
	return reportedFanCount >= 1 && reportedFanCount <= 3 ? reportedFanCount : 1;
}
