/*
 Copyright (C) 2024  Greg Smith

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

        http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
 
*/

#include <stdio.h>
#include <string.h>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "sys/param.h"
#include "esp_log.h"
#include "driver/i2c.h"
#include "main.h"
#include "CH422G.h"
#include "control.h"
#include "task_priorities.h"

#define FOOTSWITCH_TASK_STACK_SIZE          (3 * 1024)
#define FOOTSWITCH_SAMPLE_COUNT             5       // 20 msec per sample

enum FootswitchStates
{
    FOOTSWITCH_IDLE,
    FOOTSWITCH_WAIT_RELEASE_1,
    FOOTSWITCH_WAIT_RELEASE_2
};

static const char *TAG = "app_footswitches";

typedef struct
{
    uint8_t state;
    uint32_t sample_counter;
} tFootswitchControl;

static tFootswitchControl FootswitchControl;

/****************************************************************************
* NAME:        
* DESCRIPTION: 
* PARAMETERS:  
* RETURN:      
* NOTES:       
*****************************************************************************/
static uint8_t read_footswitch_input(uint8_t number, uint8_t* switch_state)
{
    uint8_t result = false;

#if CONFIG_TONEX_CONTROLLER_DISPLAY_WAVESHARE_800_480
    // display board uses I2C IO expander
    uint8_t value;

    if (CH422G_read_input(number, &value) == ESP_OK)
    {
        result = true;
        *switch_state = value;
    }
#else
    // other boards can use direct IO pin
    *switch_state = gpio_get_level(number);

    result = true;
#endif

    return result;
}

/****************************************************************************
* NAME:        
* DESCRIPTION: 
* PARAMETERS:  
* RETURN:      
* NOTES:       
*****************************************************************************/
void footswitch_task(void *arg)
{
    uint8_t value;   
 
    ESP_LOGI(TAG, "Footswitch task start");

    // let things settle
    vTaskDelay(1000);

    while (1)
    {
        switch (FootswitchControl.state)
        {
            case FOOTSWITCH_IDLE:
            default:
            {
                // read footswitches
                if (read_footswitch_input(FOOTSWITCH_1, &value)) 
                {
                    if (value == 0)
                    {
                        ESP_LOGI(TAG, "Footswitch 1 pressed");

                        // foot switch 1 pressed
                        control_request_preset_down();

                        // wait release	
                        FootswitchControl.sample_counter = 0;
                        FootswitchControl.state = FOOTSWITCH_WAIT_RELEASE_1;
                    }
                }

                if (FootswitchControl.state == FOOTSWITCH_IDLE)
                {
                    if (read_footswitch_input(FOOTSWITCH_2, &value))
                    {
                        if (value == 0)
                        {
                            ESP_LOGI(TAG, "Footswitch 2 pressed");

                            // foot switch 2 pressed, send event
                            control_request_preset_up();

                            // wait release	
                            FootswitchControl.sample_counter = 0;
                            FootswitchControl.state = FOOTSWITCH_WAIT_RELEASE_2;
                        }
                    }
                }
            } break;

            case FOOTSWITCH_WAIT_RELEASE_1:
            {
                // read footswitch 1
                if (read_footswitch_input(FOOTSWITCH_1, &value))
                {
                    if (value != 0)
                    {
                        FootswitchControl.sample_counter++;
                        if (FootswitchControl.sample_counter == FOOTSWITCH_SAMPLE_COUNT)
                        {
                            // foot switch released
                            FootswitchControl.state = FOOTSWITCH_IDLE;		
                        }
                    }
                    else
                    {
                        // reset counter
                        FootswitchControl.sample_counter = 0;
                    }
                }
            } break;

            case FOOTSWITCH_WAIT_RELEASE_2:
            {
                // read footswitch 2
                if (read_footswitch_input(FOOTSWITCH_2, &value))
                {
                    if (value != 0)
                    {
                        FootswitchControl.sample_counter++;
                        if (FootswitchControl.sample_counter == FOOTSWITCH_SAMPLE_COUNT)
                        {
                            // foot switch released
                            FootswitchControl.state = FOOTSWITCH_IDLE;
                        }                 
                    }
                    else
                    {
                        // reset counter
                        FootswitchControl.sample_counter = 0;
                    }
                }
            } break;
        }

        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

/****************************************************************************
* NAME:        
* DESCRIPTION: 
* PARAMETERS:  
* RETURN:      
* NOTES:       
*****************************************************************************/
void footswitches_init(void)
{	
    memset((void*)&FootswitchControl, 0, sizeof(FootswitchControl));
    FootswitchControl.state = FOOTSWITCH_IDLE;

#if CONFIG_TONEX_CONTROLLER_DISPLAY_NONE
    // init GPIO
    gpio_config_t gpio_config_struct;

    gpio_config_struct.pin_bit_mask = (((uint64_t)1 << FOOTSWITCH_1) | ((uint64_t)1 << FOOTSWITCH_2));
    gpio_config_struct.mode = GPIO_MODE_INPUT;
    gpio_config_struct.pull_up_en = GPIO_PULLUP_ENABLE;
    gpio_config_struct.pull_down_en = GPIO_PULLDOWN_DISABLE;
    gpio_config_struct.intr_type = GPIO_INTR_DISABLE;
    gpio_config(&gpio_config_struct);
#endif

    // create task
    xTaskCreatePinnedToCore(footswitch_task, "FOOT", FOOTSWITCH_TASK_STACK_SIZE, NULL, FOOTSWITCH_TASK_PRIORITY, NULL, 1);
}
