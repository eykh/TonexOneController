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


#include <inttypes.h>
#include <string.h>
#include <stdlib.h>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_check.h"
#include "esp_log.h"
#include "usb/usb_host.h"
#include "driver/i2c.h"
#include "nvs_flash.h"
#include "esp_vfs.h"
#include "esp_vfs_fat.h"
#include "esp_ota_ops.h"
#include "sys/param.h"
#include "control.h"
#include "footswitches.h"
#include "display.h"
#include "usb_comms.h"
#include "task_priorities.h"

#define CTRL_TASK_STACK_SIZE   (3 * 1024)
#define NVS_USERDATA_NAME       "userdata"        

#define MAX_TEXT_LENGTH                         128
#define MAX_PRESETS_DEFAULT                     20

enum CommandEvents
{
    EVENT_SET_MAX_PRESETS,
    EVENT_PRESET_DOWN,
    EVENT_PRESET_UP,
    EVENT_PRESET_INDEX,
    EVENT_SET_PRESET_NAME,
    EVENT_SET_PRESET_DETAILS,
    EVENT_SET_USB_STATUS,
    EVENT_SET_BT_STATUS,
    EVENT_SET_AMP_SKIN,
    EVENT_SAVE_USER_DATA
};

typedef struct
{
    uint8_t Event;
    char Text[MAX_TEXT_LENGTH];
    uint32_t Value;
} tControlMessage;

typedef struct
{
    uint16_t AmpSkinIndex;
    char PresetDescription[MAX_TEXT_LENGTH];
} tUserData;

typedef struct 
{
    uint32_t PresetIndex;                        // 0-based index
    char PresetName[MAX_TEXT_LENGTH];
    uint32_t MaxPresets;
    uint32_t USBStatus;
    uint32_t BTStatus;
    tUserData UserData[MAX_PRESETS_DEFAULT];
} tControlData;

static const char *TAG = "app_control";
static QueueHandle_t control_input_queue;
static tControlData ControlData;

static uint8_t SaveUserData(void);
static uint8_t LoadUserData(void);

/****************************************************************************
* NAME:        
* DESCRIPTION: 
* PARAMETERS:  
* RETURN:      
* NOTES:       
*****************************************************************************/
static uint8_t process_control_command(tControlMessage* message)
{
    ESP_LOGI(TAG, "Control command %d", message->Event);

    // check what we got
    switch (message->Event)
    {
        case EVENT_SET_MAX_PRESETS:
        {
            ControlData.MaxPresets = message->Value;
        } break;

        case EVENT_PRESET_DOWN:
        {
            if (ControlData.PresetIndex > 0)
            {
                ControlData.PresetIndex--;

                if (ControlData.USBStatus != 0)
                {
                    // send message to USB
                    usb_set_preset(ControlData.PresetIndex);

                    // change skin and user text
                    UI_SetAmpSkin(ControlData.UserData[ControlData.PresetIndex].AmpSkinIndex);
                    UI_SetPresetDescription(ControlData.UserData[ControlData.PresetIndex].PresetDescription);
                }
            }
        } break;

        case EVENT_PRESET_UP:
        {
            if (ControlData.PresetIndex < ControlData.MaxPresets)
            {
                ControlData.PresetIndex++;

                if (ControlData.USBStatus != 0)
                {
                    // send message to USB
                    usb_set_preset(ControlData.PresetIndex);

                    // change skin and user text
                    UI_SetAmpSkin(ControlData.UserData[ControlData.PresetIndex].AmpSkinIndex);
                    UI_SetPresetDescription(ControlData.UserData[ControlData.PresetIndex].PresetDescription);
                }
            }
        } break;

        case EVENT_PRESET_INDEX:
        {
            if (message->Value < ControlData.MaxPresets)
            {
                ControlData.PresetIndex = message->Value;

                if (ControlData.USBStatus != 0)
                {
                    // send message to USB
                    usb_set_preset(ControlData.PresetIndex);

                    // change skin and user text
                    UI_SetAmpSkin(ControlData.UserData[ControlData.PresetIndex].AmpSkinIndex);
                    UI_SetPresetDescription(ControlData.UserData[ControlData.PresetIndex].PresetDescription);
                }
            }
        } break;

        case EVENT_SET_PRESET_NAME:
        {
            memcpy((void*)ControlData.PresetName, (void*)message->Text, MAX_TEXT_LENGTH);
            ControlData.PresetName[MAX_TEXT_LENGTH - 1] = 0;

            // update UI
            UI_SetPresetLabel(ControlData.PresetName);
        } break;

        case EVENT_SET_PRESET_DETAILS:
        {
            memcpy((void*)ControlData.UserData[ControlData.PresetIndex].PresetDescription, (void*)message->Text, MAX_TEXT_LENGTH);
            ControlData.UserData[ControlData.PresetIndex].PresetDescription[MAX_TEXT_LENGTH - 1] = 0;
        } break;

        case EVENT_SET_USB_STATUS:
        {
            ControlData.USBStatus = message->Value;

            // update UI
            UI_SetUSBStatus(ControlData.USBStatus);

            if (ControlData.USBStatus != 0)
            {
                // device connected, sync to current preset
                usb_set_preset(ControlData.PresetIndex);
            }
        } break;

        case EVENT_SET_BT_STATUS:
         {
            ControlData.BTStatus = message->Value;

            // update UI
            UI_SetBTStatus(ControlData.BTStatus);
        } break;

        case EVENT_SET_AMP_SKIN:
        {
            ControlData.UserData[ControlData.PresetIndex].AmpSkinIndex = message->Value;

            // update UI
            UI_SetAmpSkin(ControlData.UserData[ControlData.PresetIndex].AmpSkinIndex);
        } break;

        case EVENT_SAVE_USER_DATA:
        {
            // save it
            SaveUserData();
        } break;
    }

    return 1;
}

/****************************************************************************
* NAME:        
* DESCRIPTION: 
* PARAMETERS:  
* RETURN:      
* NOTES:       
*****************************************************************************/
void control_request_preset_down(void)
{
    tControlMessage message;

    ESP_LOGI(TAG, "control_request_preset_down");            

    message.Event = EVENT_PRESET_DOWN;

    // send to queue
    if (xQueueSend(control_input_queue, (void*)&message, 0) != pdPASS)
    {
        ESP_LOGE(TAG, "control_request_preset_down queue send failed!");            
    }
}

/****************************************************************************
* NAME:        
* DESCRIPTION: 
* PARAMETERS:  
* RETURN:      
* NOTES:       
*****************************************************************************/
void control_request_preset_up(void)
{
    tControlMessage message;

    ESP_LOGI(TAG, "control_request_preset_up");

    message.Event = EVENT_PRESET_UP;

    // send to queue
    if (xQueueSend(control_input_queue, (void*)&message, 0) != pdPASS)
    {
        ESP_LOGE(TAG, "control_request_preset_up queue send failed!");            
    }
}

/****************************************************************************
* NAME:        
* DESCRIPTION: 
* PARAMETERS:  
* RETURN:      
* NOTES:       
*****************************************************************************/
void control_request_preset_index(uint8_t index)
{
    tControlMessage message;

    ESP_LOGI(TAG, "control_request_preset_index %d", index);

    message.Event = EVENT_PRESET_INDEX;
    message.Value = index;

    // send to queue
    if (xQueueSend(control_input_queue, (void*)&message, 0) != pdPASS)
    {
        ESP_LOGE(TAG, "control_request_preset_index queue send failed!");            
    }
}

/****************************************************************************
* NAME:        
* DESCRIPTION: 
* PARAMETERS:  
* RETURN:      
* NOTES:       
*****************************************************************************/
void control_set_preset_name(char* name)
{
    tControlMessage message;

    ESP_LOGI(TAG, "control_set_preset_name");            

    message.Event = EVENT_SET_PRESET_NAME;
    sprintf(message.Text, "%d: ", (int)ControlData.PresetIndex + 1);
    strncat(message.Text, name, MAX_TEXT_LENGTH - 1);

    // send to queue
    if (xQueueSend(control_input_queue, (void*)&message, 0) != pdPASS)
    {
        ESP_LOGE(TAG, "control_set_preset_name queue send failed!");            
    }
}

/****************************************************************************
* NAME:        
* DESCRIPTION: 
* PARAMETERS:  
* RETURN:      
* NOTES:       
*****************************************************************************/
void control_set_preset_details(char* name)
{
    tControlMessage message;

    ESP_LOGI(TAG, "control_set_preset_details");            

    message.Event = EVENT_SET_PRESET_DETAILS;
    strncat(message.Text, name, MAX_TEXT_LENGTH - 1);

    // send to queue
    if (xQueueSend(control_input_queue, (void*)&message, 0) != pdPASS)
    {
        ESP_LOGE(TAG, "control_set_preset_name queue send failed!");            
    }
}

/****************************************************************************
* NAME:        
* DESCRIPTION: 
* PARAMETERS:  
* RETURN:      
* NOTES:       
*****************************************************************************/
void control_set_max_presets(uint32_t max)
{
    tControlMessage message;

    ESP_LOGI(TAG, "control_set_max_presets");

    message.Event = EVENT_SET_MAX_PRESETS;
    message.Value = max;

    // send to queue
    if (xQueueSend(control_input_queue, (void*)&message, 0) != pdPASS)
    {
        ESP_LOGE(TAG, "control_set_max_presets queue send failed!");            
    }
}

/****************************************************************************
* NAME:        
* DESCRIPTION: 
* PARAMETERS:  
* RETURN:      
* NOTES:       
*****************************************************************************/
void control_set_usb_status(uint32_t status)
{
    tControlMessage message;

    ESP_LOGI(TAG, "control_set_usb_status");

    message.Event = EVENT_SET_USB_STATUS;
    message.Value = status;

    // send to queue
    if (xQueueSend(control_input_queue, (void*)&message, 0) != pdPASS)
    {
        ESP_LOGE(TAG, "control_set_usb_status queue send failed!");            
    }
}

/****************************************************************************
* NAME:        
* DESCRIPTION: 
* PARAMETERS:  
* RETURN:      
* NOTES:       
*****************************************************************************/
void control_set_bt_status(uint32_t status)
{
    tControlMessage message;

    ESP_LOGI(TAG, "control_set_bt_status");

    message.Event = EVENT_SET_BT_STATUS;
    message.Value = status;

    // send to queue
    if (xQueueSend(control_input_queue, (void*)&message, 0) != pdPASS)
    {
        ESP_LOGE(TAG, "control_set_usb_status queue send failed!");            
    }
}

/****************************************************************************
* NAME:        
* DESCRIPTION: 
* PARAMETERS:  
* RETURN:      
* NOTES:       
*****************************************************************************/
void control_save_user_data(void)
{
    tControlMessage message;

    ESP_LOGI(TAG, "control_save_user_data");

    message.Event = EVENT_SAVE_USER_DATA;

    // send to queue
    if (xQueueSend(control_input_queue, (void*)&message, 0) != pdPASS)
    {
        ESP_LOGE(TAG, "control_save_user_data queue send failed!");            
    }
}

/****************************************************************************
* NAME:        
* DESCRIPTION: 
* PARAMETERS:  
* RETURN:      
* NOTES:       
*****************************************************************************/
void control_set_amp_skin_index(uint32_t status)
{
    tControlMessage message;

    ESP_LOGI(TAG, "control_set_amp_skin_index");

    message.Event = EVENT_SET_AMP_SKIN;
    message.Value = status;

    // send to queue
    if (xQueueSend(control_input_queue, (void*)&message, 0) != pdPASS)
    {
        ESP_LOGE(TAG, "control_set_amp_skin_index queue send failed!");            
    }
}

/****************************************************************************
* NAME:        
* DESCRIPTION: 
* PARAMETERS:  
* RETURN:      
* NOTES:       
*****************************************************************************/
void control_set_amp_skin_next(void)
{
    if (ControlData.UserData[ControlData.PresetIndex].AmpSkinIndex < (AMP_SKIN_MAX - 1))
    {
        ControlData.UserData[ControlData.PresetIndex].AmpSkinIndex++;
        control_set_amp_skin_index(ControlData.UserData[ControlData.PresetIndex].AmpSkinIndex);
    }
}

/****************************************************************************
* NAME:        
* DESCRIPTION: 
* PARAMETERS:  
* RETURN:      
* NOTES:       
*****************************************************************************/
void control_set_amp_skin_previous(void)
{
    if (ControlData.UserData[ControlData.PresetIndex].AmpSkinIndex > 0)
    {
        ControlData.UserData[ControlData.PresetIndex].AmpSkinIndex--;
    
        control_set_amp_skin_index(ControlData.UserData[ControlData.PresetIndex].AmpSkinIndex);
    }
}

/****************************************************************************
* NAME:        
* DESCRIPTION: 
* PARAMETERS:  
* RETURN:      none
* NOTES:       none
****************************************************************************/
static uint8_t SaveUserData(void)
{
    esp_err_t err;
    nvs_handle_t my_handle;
    uint8_t result = 0;

    ESP_LOGI(TAG, "Writing User Data");

    // open storage
    err = nvs_open("storage", NVS_READWRITE, &my_handle);

    if (err == ESP_OK) 
    {
        // write value
        size_t required_size = sizeof(ControlData.UserData);
        err = nvs_set_blob(my_handle, NVS_USERDATA_NAME, (void*)&ControlData.UserData, required_size);

        switch (err) 
        {
            case ESP_OK:
            {
                result = 1;

                ESP_LOGI(TAG, "Wrote User Data OK");
            } break;
            
            default:
            {
                ESP_LOGE(TAG, "Error (%s) writing User Data\n", esp_err_to_name(err));
            } break;
        }

        // commit value
        err = nvs_commit(my_handle);

        // close
        nvs_close(my_handle);
    }
    else
    {
        ESP_LOGE(TAG, "Write User Data failed to open");
    }

    return result;
}

/****************************************************************************
* NAME:        
* DESCRIPTION: 
* PARAMETERS:  
* RETURN:      none
* NOTES:       none
****************************************************************************/
static uint8_t LoadUserData(void)
{
    esp_err_t err;
    nvs_handle_t my_handle;
    uint8_t result = 0;

    ESP_LOGI(TAG, "Load User Data");

    // open storage
    err = nvs_open("storage", NVS_READWRITE, &my_handle);

    if (err == ESP_OK) 
    {
        // read data
        size_t required_size = sizeof(ControlData.UserData);
        err = nvs_get_blob(my_handle, NVS_USERDATA_NAME, (void*)&ControlData.UserData, &required_size);

         switch (err) 
         {
            case ESP_OK:
            {
                // close
                nvs_close(my_handle);

                ESP_LOGI(TAG, "Load User Data OK");

                result = 1;
            } break;
            
            case ESP_ERR_NVS_NOT_FOUND:
            default:
            {
                ESP_LOGE(TAG, "Error (%s) reading User Data \n", esp_err_to_name(err));

                // close
                nvs_close(my_handle);

                // write default values
                SaveUserData();
            } break;
        }
    }
    else
    {
        ESP_LOGE(TAG, "Read User Data failed to open");
    }

    // failed    
    return result;
}

/****************************************************************************
* NAME:        
* DESCRIPTION: 
* PARAMETERS:  
* RETURN:      
* NOTES:       
*****************************************************************************/
void control_task(void *arg)
{
    tControlMessage message;

    ESP_LOGI(TAG, "Control task start");

    while (1) 
    {
        // check for any input messages
        if (xQueueReceive(control_input_queue, (void*)&message, 0) == pdPASS)
        {
            // process it
            process_control_command(&message);
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
void control_init(void)
{
    esp_err_t ret;

    memset((void*)&ControlData, 0, sizeof(ControlData));
    ControlData.MaxPresets = MAX_PRESETS_DEFAULT;

    // this will become init from Flash mem
    for (uint32_t loop = 0; loop < MAX_PRESETS_DEFAULT; loop++)
    {
        sprintf(ControlData.UserData[loop].PresetDescription, "Description");
    }

    // create queue for commands from other threads
    control_input_queue = xQueueCreate(10, sizeof(tControlMessage));
    if (control_input_queue == NULL)
    {
        ESP_LOGE(TAG, "Failed to create control input queue!");
    }

    // Initialize NVS
    ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) 
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to init NVS");
    }

    // load the non-volatile user data
    LoadUserData();

    xTaskCreatePinnedToCore(control_task, "CTRL", CTRL_TASK_STACK_SIZE, NULL, CTRL_TASK_PRIORITY, NULL, 1);
}