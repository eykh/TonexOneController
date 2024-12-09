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


#ifndef _MAIN_H
#define _MAIN_H

#ifdef __cplusplus
extern "C" {
#endif

#define APP_VERSION		"1.0.4.1"

// IO defines
#if CONFIG_TONEX_CONTROLLER_DISPLAY_WAVESHARE_800_480
    // IO expander
    #define FOOTSWITCH_1		IO_EXPANDER_PIN_NUM_0
    #define TOUCH_RESET 		IO_EXPANDER_PIN_NUM_1
    #define LCD_BACKLIGHT		IO_EXPANDER_PIN_NUM_2
    #define LCD_RESET    		IO_EXPANDER_PIN_NUM_3
    #define SD_CS       		IO_EXPANDER_PIN_NUM_4
    #define FOOTSWITCH_2		IO_EXPANDER_PIN_NUM_5

    // Micro pins
    #define TOUCH_INT                   GPIO_NUM_4    // touch panel interrupt
#else
    // direct IO pins
    #define FOOTSWITCH_1		GPIO_NUM_4
    #define FOOTSWITCH_2		GPIO_NUM_6

#endif

esp_err_t i2c_master_reset(void);

#ifdef __cplusplus
} /*extern "C"*/
#endif

#endif