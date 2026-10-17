#include "ModulePad.hpp"
#include "ModuleConfig.hpp"
#include "ModuleLights.hpp"
#include "hal/hal_ADC.hpp"
#include "hal/hal_USB.hpp"

ModulePad ModulePadInstance = ModulePad();

void ModulePad::Setup()
{
    HAL_ADC_Init();
}

void ModulePad::Update()
{
    // This happens in ModuleUSB
}

void ModulePad::UpdateStatus()
{
    for (int i = 0; i < SENSOR_COUNT; i++) {
        // ADC is noise while lights are updating
        if(ModuleLightsInstance.IsWriting())
            continue;
        
        sensorValues[i] = HAL_ADC_ReadSensor(i);
    }

    // Every mapped sensor's own state (for the lights), before the button loop
    // below stops at the first sensor that holds a button.
    for (int j = 0; j < SENSOR_COUNT; j++) {
        const SensorConfig& s = ModuleConfigInstance.GetSensorConfig(j);
        sensorStates[j] = s.buttonMapping >= 0 && s.buttonMapping < BUTTON_COUNT && sensorValues[j] > s.threshold;
    }

    for (int i = 0; i < BUTTON_COUNT; i++) {
        bool newButtonPressedState = false;

        for (int j = 0; j < SENSOR_COUNT; j++) {
			const SensorConfig& s = ModuleConfigInstance.GetSensorConfig(j);
            if(s.buttonMapping != i) {
                continue;
            }

            uint16_t sensorVal = sensorValues[j];

            if (buttonsPressed[i]) {
                if (sensorVal > s.releaseThreshold) {
                    newButtonPressedState = true;
                    break;
                }
            } else {
                if (sensorVal > s.threshold) {
                    newButtonPressedState = true;
                    break;
                }
            }
        }

        buttonsPressed[i] = newButtonPressedState;
    }
}
