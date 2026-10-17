#include "SensorReports.hpp"
#include "ReportsInternal.hpp"
#include "Modules/ModulePad.hpp"
#include "Modules/ModuleLights.hpp"

#include <string.h>

PadConfigurationFeatureHIDReport::PadConfigurationFeatureHIDReport()
{
    // A release threshold of 0 is valid; dividing by it traps on the ESP32.
    const uint16_t release = configuration.padConfiguration.sensors[0].releaseThreshold;
    reportConfiguration.releaseMultiplier =
        release == 0 ? 0 : configuration.padConfiguration.sensors[0].threshold / release;
		
    for (int s = 0; s < SENSOR_COUNT_V1; s++) {
        reportConfiguration.sensorThresholds[s] = configuration.padConfiguration.sensors[s].threshold;
        reportConfiguration.sensorToButtonMapping[s] = configuration.padConfiguration.sensors[s].buttonMapping;
    }

    ModuleLightsInstance.Clear();
}

void PadConfigurationFeatureHIDReport::Process()
{
    for (int s = 0; s < SENSOR_COUNT_V1; s++) {
        configuration.padConfiguration.sensors[s].threshold = reportConfiguration.sensorThresholds[s];
        configuration.padConfiguration.sensors[s].releaseThreshold = reportConfiguration.sensorThresholds[s] * reportConfiguration.releaseMultiplier;
        configuration.padConfiguration.sensors[s].buttonMapping = reportConfiguration.sensorToButtonMapping[s];
    }

    ModuleLightsInstance.Clear();
}

SensorHIDReport::SensorHIDReport()
{
    index = ModulePadInstance.selectedSensorIndex;
    if (index < SENSOR_COUNT)
        memcpy(&sensor, &configuration.padConfiguration.sensors[index], sizeof(SensorConfigV13));
    else
        memset(&sensor, 0, sizeof(SensorConfigV13));
}

void SensorHIDReport::Process()
{
    if (index < SENSOR_COUNT) {
        SensorConfig* sc = &configuration.padConfiguration.sensors[index];
        
        sc->threshold = sensor.threshold;
        sc->releaseThreshold = sensor.releaseThreshold;
        sc->buttonMapping = sensor.buttonMapping;
        sc->resistorValue = sensor.resistorValue;
        sc->flags = sensor.flags;
    }

    ModuleLightsInstance.Clear();
}

void RegisterSensorReports()
{
    REGISTER_REPORT(PAD_CONFIGURATION_REPORT_ID, PadConfigurationFeatureHIDReport)
    REGISTER_REPORT(SENSOR_REPORT_ID, SensorHIDReport)
}
