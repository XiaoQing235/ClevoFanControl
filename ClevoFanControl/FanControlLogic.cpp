#include "FanControlLogic.h"

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
