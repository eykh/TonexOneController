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
    EVENT_PRESET_DOWN,
    EVENT_PRESET_UP,
    EVENT_PRESET_INDEX,
    EVENT_SET_PRESET_DETAILS,
    EVENT_SET_USB_STATUS,
    EVENT_SET_BT_STATUS,
    EVENT_SET_AMP_SKIN,
    EVENT_SAVE_USER_DATA,
    EVENT_SET_USER_TEXT,
    EVENT_SET_CONFIG_BT_MODE,
    EVENT_SET_CONFIG_MV_CHOC_ENABLE,
    EVENT_SET_CONFIG_XV_MD1_ENABLE,
    EVENT_SET_CONFIG_MIDI_ENABLE,
    EVENT_SET_CONFIG_MIDI_CHANNEL,
    EVENT_SET_CONFIG_TOGGLE_BYPASS
};

typedef struct
{
    uint8_t Event;
    char Text[MAX_TEXT_LENGTH];
    uint32_t Value;
} tControlMessage;

typedef struct __attribute__ ((packed)) 
{
    uint16_t SkinIndex;
    char PresetDescription[MAX_TEXT_LENGTH];
} tUserData;

typedef struct __attribute__ ((packed)) 
{
    tUserData UserData[MAX_PRESETS_DEFAULT];

    uint8_t BTMode;

    // bt client flags
    uint16_t BTClientMvaveChocolateEnable: 1;
    uint16_t BTClientXviveMD1Enable: 1;
    uint16_t BTClientSpares: 14;

    // serial Midi flags
    uint8_t MidiSerialEnable: 1;
    uint8_t MidiSpares: 7;

    uint8_t MidiChannel;

    // general flags
    uint16_t GeneralDoublePressToggleBypass: 1;
    uint16_t GeneralSpare: 15;

    uint8_t Spare;
} tConfigData;

typedef struct 
{
    uint32_t PresetIndex;                        // 0-based index
    char PresetName[MAX_TEXT_LENGTH];
    uint32_t USBStatus;
    uint32_t BTStatus;
    tConfigData ConfigData;
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
        case EVENT_PRESET_DOWN:
        {
            if (ControlData.USBStatus != 0)
            {
                // send message to USB
                usb_previous_preset();
            }
        } break;

        case EVENT_PRESET_UP:
        {
            if (ControlData.USBStatus != 0)
            {
                // send message to USB
                usb_next_preset();
            }
        } break;

        case EVENT_PRESET_INDEX:
        {
            if (ControlData.USBStatus != 0)
            {
                // send message to USB
                usb_set_preset(message->Value);
            }
        } break;

        case EVENT_SET_PRESET_DETAILS:
        {
            ControlData.PresetIndex = message->Value;

            memcpy((void*)ControlData.PresetName, (void*)message->Text, MAX_TEXT_LENGTH);
            ControlData.PresetName[MAX_TEXT_LENGTH - 1] = 0;

#if !CONFIG_TONEX_CONTROLLER_DISPLAY_NONE
            // update UI
            UI_SetPresetLabel(ControlData.PresetName);
            UI_SetAmpSkin(ControlData.ConfigData.UserData[ControlData.PresetIndex].SkinIndex);
            UI_SetPresetDescription(ControlData.ConfigData.UserData[ControlData.PresetIndex].PresetDescription);
#endif //CONFIG_TONEX_CONTROLLER_DISPLAY_NONE            
        } break;

        case EVENT_SET_USB_STATUS:
        {
            ControlData.USBStatus = message->Value;

#if !CONFIG_TONEX_CONTROLLER_DISPLAY_NONE
            // update UI
            UI_SetUSBStatus(ControlData.USBStatus);
#endif //CONFIG_TONEX_CONTROLLER_DISPLAY_NONE            
        } break;

        case EVENT_SET_BT_STATUS:
        {
            ControlData.BTStatus = message->Value;

#if !CONFIG_TONEX_CONTROLLER_DISPLAY_NONE
            // update UI
            UI_SetBTStatus(ControlData.BTStatus);
#endif //CONFIG_TONEX_CONTROLLER_DISPLAY_NONE                        
        } break;

        case EVENT_SET_AMP_SKIN:
        {
            ControlData.ConfigData.UserData[ControlData.PresetIndex].SkinIndex = message->Value;

#if !CONFIG_TONEX_CONTROLLER_DISPLAY_NONE
            // update UI
            UI_SetAmpSkin(ControlData.ConfigData.UserData[ControlData.PresetIndex].SkinIndex);
#endif //CONFIG_TONEX_CONTROLLER_DISPLAY_NONE                                    
        } break;

        case EVENT_SAVE_USER_DATA:
        {
            // save it
            SaveUserData();

            if (message->Value != 0)
            {
                ESP_LOGI(TAG, "Config save rebooting");
                vTaskDelay(10);
                esp_restart();
            }
        } break;

        case EVENT_SET_USER_TEXT:
        {
            memcpy((void*)ControlData.ConfigData.UserData[ControlData.PresetIndex].PresetDescription, (void*)message->Text, MAX_TEXT_LENGTH);
            ControlData.ConfigData.UserData[ControlData.PresetIndex].PresetDescription[MAX_TEXT_LENGTH - 1] = 0;
        } break;

        case EVENT_SET_CONFIG_BT_MODE:
        {
            ESP_LOGI(TAG, "Config set BT mode %d", (int)message->Value);
            ControlData.ConfigData.BTMode = (uint8_t)message->Value;
        } break;

        case EVENT_SET_CONFIG_MV_CHOC_ENABLE:
        {
            ESP_LOGI(TAG, "Config set MV Choc enable %d", (int)message->Value);
            ControlData.ConfigData.BTClientMvaveChocolateEnable = (uint8_t)message->Value;
        } break;

        case EVENT_SET_CONFIG_XV_MD1_ENABLE:
        {
            ESP_LOGI(TAG, "Config set XV MD1 enable %d", (int)message->Value);
            ControlData.ConfigData.BTClientXviveMD1Enable = (uint8_t)message->Value;
        } break;

        case EVENT_SET_CONFIG_MIDI_ENABLE:
        {
            ESP_LOGI(TAG, "Config set Midi enable %d", (int)message->Value);
            ControlData.ConfigData.MidiSerialEnable = (uint8_t)message->Value;
        } break;

        case EVENT_SET_CONFIG_MIDI_CHANNEL:
        {
            ESP_LOGI(TAG, "Config set Midi channel %d", (int)message->Value);
            ControlData.ConfigData.MidiChannel = (uint8_t)message->Value;
        } break;

        case EVENT_SET_CONFIG_TOGGLE_BYPASS:
        {
            ESP_LOGI(TAG, "Config set Toggle Bypass %d", (int)message->Value);
            ControlData.ConfigData.GeneralDoublePressToggleBypass = (uint8_t)message->Value;
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
void control_sync_preset_details(uint16_t index, char* name)
{
    tControlMessage message;

    ESP_LOGI(TAG, "control_sync_preset_details");            

    message.Event = EVENT_SET_PRESET_DETAILS;
    message.Value = index;
    sprintf(message.Text, "%d: ", (int)index + 1);
    strncat(message.Text, name, MAX_TEXT_LENGTH - 1);

    // send to queue
    if (xQueueSend(control_input_queue, (void*)&message, 0) != pdPASS)
    {
        ESP_LOGE(TAG, "control_sync_preset_details queue send failed!");            
    }
}

/****************************************************************************
* NAME:        
* DESCRIPTION: 
* PARAMETERS:  
* RETURN:      
* NOTES:       
*****************************************************************************/
void control_set_user_text(char* text)
{
    tControlMessage message;

    ESP_LOGI(TAG, "control_set_user_text");            

    message.Event = EVENT_SET_USER_TEXT;
    strncat(message.Text, text, MAX_TEXT_LENGTH - 1);

    // send to queue
    if (xQueueSend(control_input_queue, (void*)&message, 0) != pdPASS)
    {
        ESP_LOGE(TAG, "control_set_user_text queue send failed!");            
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
void control_save_user_data(uint8_t reboot)
{
    tControlMessage message;

    ESP_LOGI(TAG, "control_save_user_data");

    message.Event = EVENT_SAVE_USER_DATA;
    message.Value = reboot;

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
void control_set_config_btmode(uint32_t status)
{
    tControlMessage message;

    ESP_LOGI(TAG, "control_set_config_btmode");

    message.Event = EVENT_SET_CONFIG_BT_MODE;
    message.Value = status;

    // send to queue
    if (xQueueSend(control_input_queue, (void*)&message, 0) != pdPASS)
    {
        ESP_LOGE(TAG, "control_set_config_btmode queue send failed!");            
    }
}

/****************************************************************************
* NAME:        
* DESCRIPTION: 
* PARAMETERS:  
* RETURN:      
* NOTES:       
*****************************************************************************/
void control_set_config_mv_choc_enable(uint32_t status)
{
    tControlMessage message;

    ESP_LOGI(TAG, "control_set_config_mv_choc_enable");

    message.Event = EVENT_SET_CONFIG_MV_CHOC_ENABLE;
    message.Value = status;

    // send to queue
    if (xQueueSend(control_input_queue, (void*)&message, 0) != pdPASS)
    {
        ESP_LOGE(TAG, "control_set_config_mv_choc_enable queue send failed!");            
    }
}

/****************************************************************************
* NAME:        
* DESCRIPTION: 
* PARAMETERS:  
* RETURN:      
* NOTES:       
*****************************************************************************/
void control_set_config_xv_md1_enable(uint32_t status)
{
    tControlMessage message;

    ESP_LOGI(TAG, "control_set_config_xv_md1_enable");

    message.Event = EVENT_SET_CONFIG_XV_MD1_ENABLE;
    message.Value = status;

    // send to queue
    if (xQueueSend(control_input_queue, (void*)&message, 0) != pdPASS)
    {
        ESP_LOGE(TAG, "control_set_config_xv_md1_enable queue send failed!");            
    }
}
 
/****************************************************************************
* NAME:        
* DESCRIPTION: 
* PARAMETERS:  
* RETURN:      
* NOTES:       
*****************************************************************************/
void control_set_config_serial_midi_enable(uint32_t status)
{
    tControlMessage message;

    ESP_LOGI(TAG, "control_set_config_serial_midi_enable");

    message.Event = EVENT_SET_CONFIG_MIDI_ENABLE;
    message.Value = status;

    // send to queue
    if (xQueueSend(control_input_queue, (void*)&message, 0) != pdPASS)
    {
        ESP_LOGE(TAG, "control_set_config_serial_midi_enable queue send failed!");            
    }
}    
    
/****************************************************************************
* NAME:        
* DESCRIPTION: 
* PARAMETERS:  
* RETURN:      
* NOTES:       
*****************************************************************************/
void control_set_config_serial_midi_channel(uint32_t status)
{
    tControlMessage message;

    ESP_LOGI(TAG, "control_set_config_serial_midi_channel");

    message.Event = EVENT_SET_CONFIG_MIDI_CHANNEL;
    message.Value = status;

    // send to queue
    if (xQueueSend(control_input_queue, (void*)&message, 0) != pdPASS)
    {
        ESP_LOGE(TAG, "control_set_config_serial_midi_channel queue send failed!");            
    }
}        

/****************************************************************************
* NAME:        
* DESCRIPTION: 
* PARAMETERS:  
* RETURN:      
* NOTES:       
*****************************************************************************/
void control_set_config_toggle_bypass(uint32_t status)
{
    tControlMessage message;

    ESP_LOGI(TAG, "control_set_config_toggle_bypass");

    message.Event = EVENT_SET_CONFIG_TOGGLE_BYPASS;
    message.Value = status;

    // send to queue
    if (xQueueSend(control_input_queue, (void*)&message, 0) != pdPASS)
    {
        ESP_LOGE(TAG, "control_set_config_toggle_bypass queue send failed!");            
    }
}           

/****************************************************************************
* NAME:        
* DESCRIPTION: 
* PARAMETERS:  
* RETURN:      
* NOTES:       
*****************************************************************************/
void control_set_skin_next(void)
{
    if (ControlData.ConfigData.UserData[ControlData.PresetIndex].SkinIndex < (SKIN_MAX - 1))
    {
        ControlData.ConfigData.UserData[ControlData.PresetIndex].SkinIndex++;
        control_set_amp_skin_index(ControlData.ConfigData.UserData[ControlData.PresetIndex].SkinIndex);
    }
}

/****************************************************************************
* NAME:        
* DESCRIPTION: 
* PARAMETERS:  
* RETURN:      
* NOTES:       
*****************************************************************************/
void control_set_skin_previous(void)
{
    if (ControlData.ConfigData.UserData[ControlData.PresetIndex].SkinIndex > 0)
    {
        ControlData.ConfigData.UserData[ControlData.PresetIndex].SkinIndex--;
    
        control_set_amp_skin_index(ControlData.ConfigData.UserData[ControlData.PresetIndex].SkinIndex);
    }
}

/****************************************************************************
* NAME:        
* DESCRIPTION: 
* PARAMETERS:  
* RETURN:      none
* NOTES:       none
****************************************************************************/
uint8_t control_get_config_bt_mode(void)
{
    return ControlData.ConfigData.BTMode;
}

/****************************************************************************
* NAME:        
* DESCRIPTION: 
* PARAMETERS:  
* RETURN:      none
* NOTES:       none
****************************************************************************/
uint8_t control_get_config_bt_mvave_choc_enable(void)
{
    return ControlData.ConfigData.BTClientMvaveChocolateEnable;
}

/****************************************************************************
* NAME:        
* DESCRIPTION: 
* PARAMETERS:  
* RETURN:      none
* NOTES:       none
****************************************************************************/
uint8_t control_get_config_bt_xvive_md1_enable(void)
{
    return ControlData.ConfigData.BTClientXviveMD1Enable;
}

/****************************************************************************
* NAME:        
* DESCRIPTION: 
* PARAMETERS:  
* RETURN:      none
* NOTES:       none
****************************************************************************/
uint8_t control_get_config_double_toggle(void)
{
    return ControlData.ConfigData.GeneralDoublePressToggleBypass;
}

/****************************************************************************
* NAME:        
* DESCRIPTION: 
* PARAMETERS:  
* RETURN:      none
* NOTES:       none
****************************************************************************/
uint8_t control_get_config_midi_serial_enable(void)
{
    return ControlData.ConfigData.MidiSerialEnable;
}

/****************************************************************************
* NAME:        
* DESCRIPTION: 
* PARAMETERS:  
* RETURN:      none
* NOTES:       none
****************************************************************************/
uint8_t control_get_config_midi_channel(void)
{
    return ControlData.ConfigData.MidiChannel;
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
        size_t required_size = sizeof(ControlData.ConfigData);
        err = nvs_set_blob(my_handle, NVS_USERDATA_NAME, (void*)&ControlData.ConfigData, required_size);

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
    uint8_t save_needed = 0;

    ESP_LOGI(TAG, "Load User Data");

    // open storage
    err = nvs_open("storage", NVS_READWRITE, &my_handle);

    if (err == ESP_OK) 
    {
        // read data
        size_t required_size = sizeof(ControlData.ConfigData);
        err = nvs_get_blob(my_handle, NVS_USERDATA_NAME, (void*)&ControlData.ConfigData, &required_size);

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

    // check values
    if (ControlData.ConfigData.BTMode > BT_MODE_PERIPHERAL)
    {
        ESP_LOGW(TAG, "Config BTMode invalid");
        ControlData.ConfigData.BTMode = BT_MODE_CENTRAL;
        save_needed = 1;
    }

    if (ControlData.ConfigData.MidiChannel > 16)
    {
        ESP_LOGW(TAG, "Config MidiChannel invalid");
        ControlData.ConfigData.MidiChannel = 1;
        save_needed = 1;
    }

    if (save_needed)
    {
        SaveUserData();
    }

    ESP_LOGI(TAG, "Config BT Mode: %d", (int)ControlData.ConfigData.BTMode);
    ESP_LOGI(TAG, "Config BT Mvave Choc: %d", (int)ControlData.ConfigData.BTClientMvaveChocolateEnable);
    ESP_LOGI(TAG, "Config BT Xvive MD1: %d", (int)ControlData.ConfigData.BTClientMvaveChocolateEnable);
    ESP_LOGI(TAG, "Config Midi enable: %d", (int)ControlData.ConfigData.MidiSerialEnable);
    ESP_LOGI(TAG, "Config Midi channel: %d", (int)ControlData.ConfigData.MidiChannel);
    ESP_LOGI(TAG, "Config Toggle bypass: %d", (int)ControlData.ConfigData.GeneralDoublePressToggleBypass);

    // status    
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
        if (xQueueReceive(control_input_queue, (void*)&message, pdMS_TO_TICKS(20)) == pdPASS)
        {
            // process it
            process_control_command(&message);
        }

        // don't hog the CPU
        vTaskDelay(pdMS_TO_TICKS(1));
	}
}

/****************************************************************************
* NAME:        
* DESCRIPTION: 
* PARAMETERS:  
* RETURN:      
* NOTES:       
*****************************************************************************/
void control_load_config(void)
{
    esp_err_t ret;

    memset((void*)&ControlData, 0, sizeof(ControlData));

    // this will become init from Flash mem
    for (uint32_t loop = 0; loop < MAX_PRESETS_DEFAULT; loop++)
    {
        sprintf(ControlData.ConfigData.UserData[loop].PresetDescription, "Description");
    }

    // default config, will be overwritten or used as default
    ControlData.ConfigData.BTMode = BT_MODE_CENTRAL;
    ControlData.ConfigData.BTClientMvaveChocolateEnable = 1;
    ControlData.ConfigData.BTClientXviveMD1Enable = 1;
    ControlData.ConfigData.GeneralDoublePressToggleBypass = 0;
    ControlData.ConfigData.MidiSerialEnable = 1;
    ControlData.ConfigData.MidiChannel = 1;

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
    // create queue for commands from other threads
    control_input_queue = xQueueCreate(10, sizeof(tControlMessage));
    if (control_input_queue == NULL)
    {
        ESP_LOGE(TAG, "Failed to create control input queue!");
    }

    xTaskCreatePinnedToCore(control_task, "CTRL", CTRL_TASK_STACK_SIZE, NULL, CTRL_TASK_PRIORITY, NULL, 1);
}
