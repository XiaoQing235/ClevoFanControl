#pragma once

bool ShouldCompleteForcedCooling(bool enabled, int cpuTemperature,
	int gpuTemperature, int threshold);
