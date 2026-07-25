#include "FanControlLogic.h"

bool ShouldCompleteForcedCooling(bool enabled, int cpuTemperature,
	int gpuTemperature, int threshold)
{
	return enabled && cpuTemperature < threshold && gpuTemperature < threshold;
}
