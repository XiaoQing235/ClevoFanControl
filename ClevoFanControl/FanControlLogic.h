#pragma once

bool ShouldCompleteForcedCooling(bool enabled, int cpuTemperature,
	int gpuTemperature, int threshold);

bool IsCurrentConfigGeneration(long capturedGeneration, long activeGeneration);

bool ShouldRestartUpdateTimer(int activeInterval, int configuredInterval);
