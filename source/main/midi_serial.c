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
#include "driver/uart.h"
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
#include "midi_serial.h"
#include "control.h"
#include "task_priorities.h"

#define MIDI_SERIAL_TASK_STACK_SIZE             (3 * 1024)
#define MIDI_SERIAL_BUFFER_SIZE                 128

#if CONFIG_TONEX_CONTROLLER_DISPLAY_WAVESHARE_800_480
    #define UART_PORT_NUM                           UART_NUM_1
    // to do here: finalise this
    //#define UART_RX_PIN                             GPIO_NUM_43

    // Waveshare 7" using ADC port - development use
    #define UART_RX_PIN                           GPIO_NUM_6 
#else
    #define UART_PORT_NUM                           UART_NUM_1
    #define UART_RX_PIN                             GPIO_NUM_5
#endif

static const char *TAG = "app_midi_serial";

// Note: based on https://github.com/vit3k/tonex_controller/blob/main/main/midi.cpp

static uint8_t midi_serial_buffer[MIDI_SERIAL_BUFFER_SIZE];
static uint8_t midi_serial_channel = 0;

/****************************************************************************
* NAME:        
* DESCRIPTION: 
* PARAMETERS:  
* RETURN:      
* NOTES:       
*****************************************************************************/
static void midi_serial_task(void *arg)
{
    int rx_length;

    ESP_LOGI(TAG, "Midi Serial task start");

    // init UART
    uart_config_t uart_config = {
        .baud_rate = 31250,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    
    int intr_alloc_flags = 0;
    ESP_ERROR_CHECK(uart_driver_install(UART_PORT_NUM, MIDI_SERIAL_BUFFER_SIZE * 2, 0, 0, NULL, intr_alloc_flags));
    ESP_ERROR_CHECK(uart_param_config(UART_PORT_NUM, &uart_config));
    ESP_ERROR_CHECK(uart_set_pin(UART_PORT_NUM, UART_PIN_NO_CHANGE, UART_RX_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));

    while (1) 
    {
        // try to read data from UART
        rx_length = uart_read_bytes(UART_PORT_NUM, midi_serial_buffer, (MIDI_SERIAL_BUFFER_SIZE - 1), pdMS_TO_TICKS(20));
        
        if (rx_length != 0)
        {
            // ESP_LOG_BUFFER_HEXDUMP(TAG, data, rx_length, ESP_LOG_INFO);
            ESP_LOGI(TAG, "Midi Serial Got %d bytes", rx_length);

            for (size_t i = 0; i < rx_length; i++)
            {
                // Skip real-time messages (status bytes 0xF8 to 0xFF)
                if (midi_serial_buffer[i] >= 0xF8)
                {
                    continue;
                }

                // Check if this byte is a status byte for Program Change
                if ((midi_serial_buffer[i] & 0xF0) == 0xC0)
                {
                    // Program Change status byte found
                    uint8_t channel = midi_serial_buffer[i] & 0x0F;

                    // Ensure there's a data byte following the status byte
                    if ((i + 1) < MIDI_SERIAL_BUFFER_SIZE)
                    {
                        uint8_t programNumber = midi_serial_buffer[i + 1];

                        if (channel == midi_serial_channel)
                        {
                            ESP_LOGI(TAG, "Change to preset %d", programNumber);

                            // change to this preset
                            control_request_preset_index(programNumber);
                        }
                        
                        // Skip the data byte
                        i++;
                    }
                    else
                    {
                        ESP_LOGW(TAG, "Warning: Incomplete Program Change message at end of buffer");
                        break;
                    }
                }
                else if (midi_serial_buffer[i] & 0x80)
                {
                    // This is a status byte for a different type of message
                    // Skip this message by finding the next status byte or end of buffer
                    while ((++i < MIDI_SERIAL_BUFFER_SIZE) && !(midi_serial_buffer[i] & 0x80))
                    {
                    }
                    i--; // Decrement i because the for loop will increment it again
                }
                
                // If it's not a status byte, it's a data byte of a message we're not interested in
                // The loop will automatically move to the next byte
            }

            // don't hog the CPU
            vTaskDelay(pdMS_TO_TICKS(2));
        }
    }
}

/****************************************************************************
* NAME:        
* DESCRIPTION: 
* PARAMETERS:  
* RETURN:      
* NOTES:       
*****************************************************************************/
void midi_serial_init(void)
{	
    memset((void*)midi_serial_buffer, 0, sizeof(midi_serial_buffer));

    // get the channel to use
    midi_serial_channel = control_get_config_midi_channel();

    // adjust to zero based indexing
    if (midi_serial_channel > 0)
    {
        midi_serial_channel--;
    }

    xTaskCreatePinnedToCore(midi_serial_task, "MIDIS", MIDI_SERIAL_TASK_STACK_SIZE, NULL, MIDI_SERIAL_TASK_PRIORITY, NULL, 1);
}
