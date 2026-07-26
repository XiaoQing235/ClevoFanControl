#pragma once

bool ShouldCompleteForcedCooling(bool enabled, int cpuTemperature,
	int gpuTemperature, int threshold);

bool IsCurrentConfigGeneration(long capturedGeneration, long activeGeneration);

bool ShouldRestartUpdateTimer(int activeInterval, int configuredInterval);
bool ShouldRetryTemperatureSample(bool hasBaseline, int currentTemperature,
	int sampledTemperature);
int DecodeFanRpmCounter(int counter);
int NormalizeFanCount(int reportedFanCount);
