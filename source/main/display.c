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
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_timer.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_rgb.h"
#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "lvgl.h"
#include "demos/lv_demos.h"
#include "esp_vfs.h"
#include "esp_vfs_fat.h"
#include "esp_ota_ops.h"
#include "sys/param.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_crc.h"
#include "esp_now.h"
#include "driver/i2c.h"
#include "esp_lcd_touch_gt911.h"
#include "esp_intr_alloc.h"
#include "main.h"
#include "ui.h"
#include "display.h"
#include "CH422G.h"
#include "control.h"
#include "task_priorities.h"
#include "midi_control.h"

#if CONFIG_TONEX_CONTROLLER_DISPLAY_WAVESHARE_800_480

static const char *TAG = "app_display";

#define DISPLAY_TASK_STACK_SIZE   (6 * 1024)

// LCD panel config
#define DISPLAY_LCD_PIXEL_CLOCK_HZ     (18 * 1000 * 1000)
#define DISPLAY_LCD_BK_LIGHT_ON_LEVEL  1
#define DISPLAY_LCD_BK_LIGHT_OFF_LEVEL !DISPLAY_LCD_BK_LIGHT_ON_LEVEL
#define DISPLAY_PIN_NUM_BK_LIGHT       -1
#define DISPLAY_PIN_NUM_HSYNC          46
#define DISPLAY_PIN_NUM_VSYNC          3
#define DISPLAY_PIN_NUM_DE             5
#define DISPLAY_PIN_NUM_PCLK           7
#define DISPLAY_PIN_NUM_DATA0          14 // B3
#define DISPLAY_PIN_NUM_DATA1          38 // B4
#define DISPLAY_PIN_NUM_DATA2          18 // B5
#define DISPLAY_PIN_NUM_DATA3          17 // B6
#define DISPLAY_PIN_NUM_DATA4          10 // B7
#define DISPLAY_PIN_NUM_DATA5          39 // G2
#define DISPLAY_PIN_NUM_DATA6          0 // G3
#define DISPLAY_PIN_NUM_DATA7          45 // G4
#define DISPLAY_PIN_NUM_DATA8          48 // G5
#define DISPLAY_PIN_NUM_DATA9          47 // G6
#define DISPLAY_PIN_NUM_DATA10         21 // G7
#define DISPLAY_PIN_NUM_DATA11         1  // R3
#define DISPLAY_PIN_NUM_DATA12         2  // R4
#define DISPLAY_PIN_NUM_DATA13         42 // R5
#define DISPLAY_PIN_NUM_DATA14         41 // R6
#define DISPLAY_PIN_NUM_DATA15         40 // R7
#define DISPLAY_PIN_NUM_DISP_EN        -1

// The pixel number in horizontal and vertical
#define DISPLAY_LCD_H_RES              800
#define DISPLAY_LCD_V_RES              480

#if CONFIG_DISPLAY_DOUBLE_FB
#define DISPLAY_LCD_NUM_FB             2
#else
#define DISPLAY_LCD_NUM_FB             1
#endif // CONFIG_DISPLAY_DOUBLE_FB

#define DISPLAY_LVGL_TICK_PERIOD_MS    2
#define DISPLAY_LVGL_TASK_MAX_DELAY_MS 500
#define DISPLAY_LVGL_TASK_MIN_DELAY_MS 1

#define BUF_SIZE (1024)
#define I2C_MASTER_TIMEOUT_MS           1000

#define MAX_UI_TEXT     130

enum UIElements
{
    UI_ELEMENT_USB_STATUS,
    UI_ELEMENT_BT_STATUS,
    UI_ELEMENT_PRESET_NAME,
    UI_ELEMENT_AMP_SKIN,
    UI_ELEMENT_PRESET_DESCRIPTION,
};

enum UIAction
{
    UI_ACTION_SET_STATE,
    UI_ACTION_SET_LABEL_TEXT,
    UI_ACTION_SET_ENTRY_TEXT
};

typedef struct 
{
    uint8_t ElementID;
    uint8_t Action;
    uint32_t Value;
    char Text[MAX_UI_TEXT];
} tUIUpdate;

static SemaphoreHandle_t lvgl_mux = NULL;
static QueueHandle_t ui_update_queue;
static SemaphoreHandle_t I2CMutexHandle;

// we use two semaphores to sync the VSYNC event and the LVGL task, to avoid potential tearing effect
#if CONFIG_DISPLAY_AVOID_TEAR_EFFECT_WITH_SEM
SemaphoreHandle_t sem_vsync_end;
SemaphoreHandle_t sem_gui_ready;
#endif

/****************************************************************************
* NAME:        
* DESCRIPTION: 
* PARAMETERS:  
* RETURN:      
* NOTES:       
*****************************************************************************/
static bool display_on_vsync_event(esp_lcd_panel_handle_t panel, const esp_lcd_rgb_panel_event_data_t *event_data, void *user_data)
{
    BaseType_t high_task_awoken = pdFALSE;
#if CONFIG_DISPLAY_AVOID_TEAR_EFFECT_WITH_SEM
    if (xSemaphoreTakeFromISR(sem_gui_ready, &high_task_awoken) == pdTRUE) {
        xSemaphoreGiveFromISR(sem_vsync_end, &high_task_awoken);
    }
#endif
    return high_task_awoken == pdTRUE;
}

/****************************************************************************
* NAME:        
* DESCRIPTION: 
* PARAMETERS:  
* RETURN:      
* NOTES:       
*****************************************************************************/
static void display_lvgl_flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_map)
{
    esp_lcd_panel_handle_t panel_handle = (esp_lcd_panel_handle_t) drv->user_data;
    int offsetx1 = area->x1;
    int offsetx2 = area->x2;
    int offsety1 = area->y1;
    int offsety2 = area->y2;
#if CONFIG_DISPLAY_AVOID_TEAR_EFFECT_WITH_SEM
    xSemaphoreGive(sem_gui_ready);
    xSemaphoreTake(sem_vsync_end, portMAX_DELAY);
#endif
    // pass the draw buffer to the driver
    esp_lcd_panel_draw_bitmap(panel_handle, offsetx1, offsety1, offsetx2 + 1, offsety2 + 1, color_map);
    lv_disp_flush_ready(drv);
}

/****************************************************************************
* NAME:        
* DESCRIPTION: 
* PARAMETERS:  
* RETURN:      
* NOTES:       
*****************************************************************************/
static void display_increase_lvgl_tick(void *arg)
{
    /* Tell LVGL how many milliseconds has elapsed */
    lv_tick_inc(DISPLAY_LVGL_TICK_PERIOD_MS);
}

/****************************************************************************
* NAME:        
* DESCRIPTION: 
* PARAMETERS:  
* RETURN:      
* NOTES:       
*****************************************************************************/
bool display_lvgl_lock(int timeout_ms)
{
    // Convert timeout in milliseconds to FreeRTOS ticks
    // If `timeout_ms` is set to -1, the program will block until the condition is met
    const TickType_t timeout_ticks = (timeout_ms == -1) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    return xSemaphoreTakeRecursive(lvgl_mux, timeout_ticks) == pdTRUE;
}

/****************************************************************************
* NAME:        
* DESCRIPTION: 
* PARAMETERS:  
* RETURN:      
* NOTES:       
*****************************************************************************/
void display_lvgl_unlock(void)
{
    xSemaphoreGiveRecursive(lvgl_mux);
}

/****************************************************************************
* NAME:        
* DESCRIPTION: 
* PARAMETERS:  
* RETURN:      
* NOTES:       
*****************************************************************************/
static void display_lvgl_touch_cb(lv_indev_drv_t * drv, lv_indev_data_t * data)
{
    uint16_t touchpad_x[1] = {0};
    uint16_t touchpad_y[1] = {0};
    uint8_t touchpad_cnt = 0;
    bool touchpad_pressed = false;

    if (xSemaphoreTake(I2CMutexHandle, (TickType_t)10) == pdTRUE)
    {
        /* Read touch controller data */
        esp_lcd_touch_read_data(drv->user_data);

        /* Get coordinates */
        touchpad_pressed = esp_lcd_touch_get_coordinates(drv->user_data, touchpad_x, touchpad_y, NULL, &touchpad_cnt, 1);

        xSemaphoreGive(I2CMutexHandle);
    }
    else
    {
        ESP_LOGE(TAG, "Touch cb mutex timeout");
    }

    if (touchpad_pressed && touchpad_cnt > 0) 
    {
        data->point.x = touchpad_x[0];
        data->point.y = touchpad_y[0];
        data->state = LV_INDEV_STATE_PR;
    } 
    else 
    {
        data->state = LV_INDEV_STATE_REL;
    }
}

/****************************************************************************
* NAME:        
* DESCRIPTION: 
* PARAMETERS:  
* RETURN:      
* NOTES:       
*****************************************************************************/
void PreviousClicked(lv_event_t * e)
{
    // called from LVGL 
    ESP_LOGI(TAG, "UI Previous Clicked");      

    control_request_preset_down();      
}

/****************************************************************************
* NAME:        
* DESCRIPTION: 
* PARAMETERS:  
* RETURN:      
* NOTES:       
*****************************************************************************/
void NextClicked(lv_event_t * e)
{
    // called from LVGL 
    ESP_LOGI(TAG, "UI Next Clicked");    

    control_request_preset_up();        
}

/****************************************************************************
* NAME:        
* DESCRIPTION: 
* PARAMETERS:  
* RETURN:      
* NOTES:       
*****************************************************************************/
void AmpSkinPrevious(lv_event_t * e)
{
    control_set_skin_previous();
}

/****************************************************************************
* NAME:        
* DESCRIPTION: 
* PARAMETERS:  
* RETURN:      
* NOTES:       
*****************************************************************************/
void AmpSkinNext(lv_event_t * e)
{
    control_set_skin_next();
}

/****************************************************************************
* NAME:        
* DESCRIPTION: 
* PARAMETERS:  
* RETURN:      
* NOTES:       
*****************************************************************************/
void SaveUserDataRequest(lv_event_t * e)
{
    control_save_user_data(0);
}

/****************************************************************************
* NAME:        
* DESCRIPTION: 
* PARAMETERS:  
* RETURN:      
* NOTES:       
*****************************************************************************/
void BTBondsClearRequest(lv_event_t * e)
{
    // request to clear bluetooth bonds
    midi_delete_bluetooth_bonds();
}

/****************************************************************************
* NAME:        
* DESCRIPTION: 
* PARAMETERS:  
* RETURN:      
* NOTES:       
*****************************************************************************/
void PresetDescriptionChanged(lv_event_t * e)
{
    char* text = (char*)lv_textarea_get_text(ui_PresetDetailsTextArea);

    control_set_user_text(text);      
}

/****************************************************************************
* NAME:        
* DESCRIPTION: 
* PARAMETERS:  
* RETURN:      
* NOTES:       
*****************************************************************************/
void UI_SetUSBStatus(uint8_t state)
{
    tUIUpdate ui_update;

    // build command
    ui_update.ElementID = UI_ELEMENT_USB_STATUS;
    ui_update.Action = UI_ACTION_SET_STATE;
    ui_update.Value = state;

    // send to queue
    if (xQueueSend(ui_update_queue, (void*)&ui_update, 0) != pdPASS)
    {
        ESP_LOGE(TAG, "UI Update queue send failed!");            
    }
}

/****************************************************************************
* NAME:        
* DESCRIPTION: 
* PARAMETERS:  
* RETURN:      
* NOTES:       
*****************************************************************************/
void UI_SetBTStatus(uint8_t state)
{
    tUIUpdate ui_update;

    // build command
    ui_update.ElementID = UI_ELEMENT_BT_STATUS;
    ui_update.Action = UI_ACTION_SET_STATE;
    ui_update.Value = state;

    // send to queue
    if (xQueueSend(ui_update_queue, (void*)&ui_update, 0) != pdPASS)
    {
        ESP_LOGE(TAG, "UI Update queue send failed!");            
    }
}

/****************************************************************************
* NAME:        
* DESCRIPTION: 
* PARAMETERS:  
* RETURN:      
* NOTES:       
*****************************************************************************/
void UI_SetPresetLabel(char* text)
{
    tUIUpdate ui_update;

    // build command
    ui_update.ElementID = UI_ELEMENT_PRESET_NAME;
    ui_update.Action = UI_ACTION_SET_LABEL_TEXT;
    strncpy(ui_update.Text, text, MAX_UI_TEXT - 1);

    // send to queue
    if (xQueueSend(ui_update_queue, (void*)&ui_update, 0) != pdPASS)
    {
        ESP_LOGE(TAG, "UI Update queue send failed!");            
    }
}

/****************************************************************************
* NAME:        
* DESCRIPTION: 
* PARAMETERS:  
* RETURN:      
* NOTES:       
*****************************************************************************/
void UI_SetAmpSkin(uint16_t index)
{
    tUIUpdate ui_update;

    // build commands
    ui_update.ElementID = UI_ELEMENT_AMP_SKIN;
    ui_update.Action = UI_ACTION_SET_STATE;
    ui_update.Value = index;

    // send to queue
    if (xQueueSend(ui_update_queue, (void*)&ui_update, 0) != pdPASS)
    {
        ESP_LOGE(TAG, "UI Update queue send failed!");            
    }
}

/****************************************************************************
* NAME:        
* DESCRIPTION: 
* PARAMETERS:  
* RETURN:      
* NOTES:       
*****************************************************************************/
void UI_SetPresetDescription(char* text)
{
    tUIUpdate ui_update;

    // build command
    ui_update.ElementID = UI_ELEMENT_PRESET_DESCRIPTION;
    ui_update.Action = UI_ACTION_SET_ENTRY_TEXT;
    strncpy(ui_update.Text, text, MAX_UI_TEXT - 1);

    // send to queue
    if (xQueueSend(ui_update_queue, (void*)&ui_update, 0) != pdPASS)
    {
        ESP_LOGE(TAG, "UI Update queue send failed!");            
    }
}

/****************************************************************************
* NAME:        
* DESCRIPTION: 
* PARAMETERS:  
* RETURN:      
* NOTES:       
*****************************************************************************/
static lv_obj_t* ui_get_skin_image(uint16_t index)
{
    lv_obj_t* result;

    switch (index)
    {
#if CONFIG_TONEX_CONTROLLER_SKINS_AMP        
        // amps
        case AMP_SKIN_JCM800:
        {
            result = (lv_obj_t*)&ui_img_skin_jcm800_png;            
        } break;

        case AMP_SKIN_TWIN_REVERB:
        {
            result = (lv_obj_t*)&ui_img_skin_twinreverb_png;
        } break;

        case AMP_SKIN_2001RB:
        {
            result = (lv_obj_t*)&ui_img_skin_2001rb_png;
        } break;

        case AMP_SKIN_5150:
        {
            result = (lv_obj_t*)&ui_img_skin_5150_png;
        } break;

        case AMP_SKIN_B18N:
        {
            result = (lv_obj_t*)&ui_img_skin_b18n_png;
        } break;

        case AMP_SKIN_BLUES_DELUXE:
        {
            result = (lv_obj_t*)&ui_img_skin_bluesdeluxe_png;
        } break;

        case AMP_SKIN_DEVILLE:
        {
            result = (lv_obj_t*)&ui_img_skin_deville_png;
        } break;

        case AMP_SKIN_DUAL_RECTIFIER:
        {
            result = (lv_obj_t*)&ui_img_skin_dualrectifier_png;
        } break;

        case AMP_SKIN_GOLD_FINGER:
        {
            result = (lv_obj_t*)&ui_img_skin_goldfinger_png;
        } break;

        case AMP_SKIN_INVADER:
        {
            result = (lv_obj_t*)&ui_img_skin_invader_png;
        } break;

        case AMP_SKIN_JAZZ_CHORUS:
        {
            result = (lv_obj_t*)&ui_img_skin_jazzchorus_png;
        } break;

        case AMP_SKIN_OR_50:
        {
            result = (lv_obj_t*)&ui_img_skin_or50_png;
        } break;

        case AMP_SKIN_POWERBALL:
        {
            result = (lv_obj_t*)&ui_img_skin_powerball_png;
        } break;

        case AMP_SKIN_PRINCETON:
        {
            result = (lv_obj_t*)&ui_img_skin_princeton_png;
        } break;

        case AMP_SKIN_SVTCL:
        {
            result = (lv_obj_t*)&ui_img_skin_svtcl_png;
        } break;

        case AMP_SKIN_MAVERICK:
        {
            result = (lv_obj_t*)&ui_img_skin_maverick_png;
        } break;

        case AMP_SKIN_MK3:
        {
            result = (lv_obj_t*)&ui_img_skin_mk3_png;
        } break;

        case AMP_SKIN_SUPERBASS:
        {
            result = (lv_obj_t*)&ui_img_skin_superbass_png;
        } break;

        case AMP_SKIN_DUMBLE:
        {
            result = (lv_obj_t*)&ui_img_skin_dumble_png;
        } break;

        case AMP_SKIN_JETCITY:
        {
            result = (lv_obj_t*)&ui_img_skin_jetcity_png;
        } break;

        case AMP_SKIN_AC30:
        {
            result = (lv_obj_t*)&ui_img_skin_ac30_png;
        } break;

        case AMP_SKIN_EVH5150:
        {
            result = (lv_obj_t*)&ui_img_skin_evh5150_png;
        } break;

        case AMP_SKIN_2020:
        {
            result = (lv_obj_t*)&ui_img_skin_2020_png;
        } break;

        case AMP_SKIN_PINK_TACO:
        {
            result = (lv_obj_t*)&ui_img_skin_pinktaco_png;
        } break;

        case AMP_SKIN_SUPRO_50:
        {
            result = (lv_obj_t*)&ui_img_skin_supro50_png;
        } break;

        case AMP_SKIN_DIEZEL:
        {
            result = (lv_obj_t*)&ui_img_skin_diezel_png;
        } break;
#endif  //CONFIG_TONEX_CONTROLLER_SKINS_AMP

#if CONFIG_TONEX_CONTROLLER_SKINS_PEDAL
        // pedals
        case PEDAL_SKIN_ARION:
        {
            result = (lv_obj_t*)&ui_img_pskin_arion_png;
        } break;

        case PEDAL_SKIN_BIGMUFF:
        {
            result = (lv_obj_t*)&ui_img_pskin_bigmuff_png;
        } break;

        case PEDAL_SKIN_DARKGLASS:
        {
            result = (lv_obj_t*)&ui_img_pskin_darkglass_png;
        } break;

        case PEDAL_SKIN_DOD:
        {
            result = (lv_obj_t*)&ui_img_pskin_dod_png;
        } break;

        case PEDAL_SKIN_EHX:
        {
            result = (lv_obj_t*)&ui_img_pskin_ehx_png;
        } break;

        case PEDAL_SKIN_FENDER:
        {
            result = (lv_obj_t*)&ui_img_pskin_fender_png;
        } break;

        case PEDAL_SKIN_FULLTONE:
        {
            result = (lv_obj_t*)&ui_img_pskin_fulltone_png;
        } break;

        case PEDAL_SKIN_FZS:
        {
            result = (lv_obj_t*)&ui_img_pskin_fzs_png;
        } break;

        case PEDAL_SKIN_JHS:
        {
            result = (lv_obj_t*)&ui_img_pskin_jhs_png;
        } break;

        case PEDAL_SKIN_KLON:
        {
            result = (lv_obj_t*)&ui_img_pskin_klon_png;
        } break;

        case PEDAL_SKIN_LANDGRAF:
        {
            result = (lv_obj_t*)&ui_img_pskin_landgraf_png;
        } break;

        case PEDAL_SKIN_MXR:
        {
            result = (lv_obj_t*)&ui_img_pskin_mxr_png;
        } break;

        case PEDAL_SKIN_MXR2:
        {
            result = (lv_obj_t*)&ui_img_pskin_mxr2_png;
        } break;

        case PEDAL_SKIN_OD1:
        {
            result = (lv_obj_t*)&ui_img_pskin_od1_png;
        } break;

        case PEDAL_SKIN_PLIMSOUL:
        {
            result = (lv_obj_t*)&ui_img_pskin_plimsoul_png;
        } break;

        case PEDAL_SKIN_ROGERMAYER:
        {
            result = (lv_obj_t*)&ui_img_pskin_rogermayer_png;
        } break;

        case PEDAL_SKIN_SEYMOUR:
        {
            result = (lv_obj_t*)&ui_img_pskin_seymour_png;
        } break;

        case PEDAL_SKIN_STRYMON:
        {
            result = (lv_obj_t*)&ui_img_pskin_strymon_png;
        } break;

        case PEDAL_SKIN_TREX:
        {
            result = (lv_obj_t*)&ui_img_pskin_trex_png;
        } break;

        case PEDAL_SKIN_TUBESCREAMER:
        {
            result = (lv_obj_t*)&ui_img_pskin_tubescreamer_png;
        } break;

        case PEDAL_SKIN_WAMPLER:
        {
            result = (lv_obj_t*)&ui_img_pskin_wampler_png;
        } break;

        case PEDAL_SKIN_ZVEX:
        {
            result = (lv_obj_t*)&ui_img_pskin_zvex_png;
        } break;
#endif //CONFIG_TONEX_CONTROLLER_SKINS_PEDAL

        default:
        {
            result = (lv_obj_t*)&ui_img_skin_jcm800_png;            
        } break;
    }

    return result;
}

/****************************************************************************
* NAME:        
* DESCRIPTION: 
* PARAMETERS:  
* RETURN:      
* NOTES:       
*****************************************************************************/
static uint8_t update_ui_element(tUIUpdate* update)
{
    lv_obj_t* element_1 = NULL;

    switch (update->ElementID)
    {
        case UI_ELEMENT_USB_STATUS:
        {
            element_1 = ui_USBStatusFail;
        } break;

        case UI_ELEMENT_BT_STATUS:
        {
            element_1 = ui_BTStatusConn;
        } break;

        case UI_ELEMENT_PRESET_NAME:
        {
            element_1 = ui_PresetHeadingLabel;
        } break;

        case UI_ELEMENT_AMP_SKIN:
        {
            element_1 = ui_SkinImage;
        } break;

        case UI_ELEMENT_PRESET_DESCRIPTION:
        {
            element_1 = ui_PresetDetailsTextArea;
        } break;

        default:
        {
            ESP_LOGE(TAG, "Unknown display elment %d", update->ElementID);     
            return 0;        
        } break;
    }
    
    // check the action
    switch (update->Action)
    {
        case UI_ACTION_SET_STATE:
        {
            // check the element
            if (element_1 == ui_USBStatusFail)
            {
                if (update->Value == 0)
                {
                    // show the USB disconnected image
                    lv_obj_add_flag(ui_USBStatusOK, LV_OBJ_FLAG_HIDDEN);
                    lv_obj_clear_flag(ui_USBStatusFail, LV_OBJ_FLAG_HIDDEN);
                }
                else
                {
                    // show the USB connected image
                    lv_obj_add_flag(ui_USBStatusFail, LV_OBJ_FLAG_HIDDEN);
                    lv_obj_clear_flag(ui_USBStatusOK, LV_OBJ_FLAG_HIDDEN);
                }
            }
            else if (element_1 == ui_BTStatusConn)
            {
                if (update->Value == 0)
                {
                    // show the BT disconnected image
                    lv_obj_add_flag(ui_BTStatusConn, LV_OBJ_FLAG_HIDDEN);
                    lv_obj_clear_flag(ui_BTStatusDisconn, LV_OBJ_FLAG_HIDDEN);
                }
                else
                {
                    // show the BT connected image
                    lv_obj_add_flag(ui_BTStatusDisconn, LV_OBJ_FLAG_HIDDEN);
                    lv_obj_clear_flag(ui_BTStatusConn, LV_OBJ_FLAG_HIDDEN);
                }
            }
            else if (element_1 == ui_SkinImage)
            {
                // set skin
                lv_img_set_src(ui_SkinImage, ui_get_skin_image(update->Value));
            }
        } break;

        case UI_ACTION_SET_LABEL_TEXT:
        {
            lv_label_set_text(element_1, update->Text);
        } break;

        case UI_ACTION_SET_ENTRY_TEXT:
        {
            lv_textarea_set_text(element_1, update->Text);
        } break;

        default:
        {
            ESP_LOGE(TAG, "Unknown display action");
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
void display_task(void *arg)
{
    tUIUpdate ui_update;
    ESP_LOGI(TAG, "Display task start");

    while (1) 
    {
        // Lock the mutex due to the LVGL APIs are not thread-safe
        if (display_lvgl_lock(-1)) 
        {
            lv_task_handler();

            // check for any UI update messages
            if (xQueueReceive(ui_update_queue, (void*)&ui_update, 0) == pdPASS)
            {
                // process it
                update_ui_element(&ui_update);
            }

            // Release the mutex
            display_lvgl_unlock();
	    }

        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

/****************************************************************************
* NAME:        
* DESCRIPTION: 
* PARAMETERS:  
* RETURN:      
* NOTES:       
*****************************************************************************/
void display_init(i2c_port_t I2CNum, SemaphoreHandle_t I2CMutex)
{
    esp_err_t ret = ESP_OK;
    static lv_disp_draw_buf_t disp_buf; // contains internal graphic buffer(s) called draw buffer(s)
    static lv_disp_drv_t disp_drv;      // contains callback functions
    uint8_t touch_ok = 0;
    gpio_config_t gpio_config_struct;

    I2CMutexHandle = I2CMutex;

    // create queue for UI updates from other threads
    ui_update_queue = xQueueCreate(25, sizeof(tUIUpdate));
    if (ui_update_queue == NULL)
    {
        ESP_LOGE(TAG, "Failed to create UI update queue!");
    }

#if CONFIG_DISPLAY_AVOID_TEAR_EFFECT_WITH_SEM
    ESP_LOGI(TAG, "Create semaphores");
    sem_vsync_end = xSemaphoreCreateBinary();
    assert(sem_vsync_end);
    sem_gui_ready = xSemaphoreCreateBinary();
    assert(sem_gui_ready);
#endif

#if DISPLAY_PIN_NUM_BK_LIGHT >= 0
    ESP_LOGI(TAG, "Turn off LCD backlight");
    gpio_config_t bk_gpio_config = {
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = 1ULL << DISPLAY_PIN_NUM_BK_LIGHT
    };
    ESP_ERROR_CHECK(gpio_config(&bk_gpio_config));
#endif

    ESP_LOGI(TAG, "Install RGB LCD panel driver");
    esp_lcd_panel_handle_t panel_handle = NULL;
    esp_lcd_rgb_panel_config_t panel_config = {
        .data_width = 16, // RGB565 in parallel mode, thus 16bit in width
        .psram_trans_align = 64,
        .num_fbs = DISPLAY_LCD_NUM_FB,
#if CONFIG_DISPLAY_USE_BOUNCE_BUFFER
        .bounce_buffer_size_px = 10 * DISPLAY_LCD_H_RES,
#endif
        .clk_src = LCD_CLK_SRC_DEFAULT,
        .disp_gpio_num = DISPLAY_PIN_NUM_DISP_EN,
        .pclk_gpio_num = DISPLAY_PIN_NUM_PCLK,
        .vsync_gpio_num = DISPLAY_PIN_NUM_VSYNC,
        .hsync_gpio_num = DISPLAY_PIN_NUM_HSYNC,
        .de_gpio_num = DISPLAY_PIN_NUM_DE,
        .data_gpio_nums = {
            DISPLAY_PIN_NUM_DATA0,
            DISPLAY_PIN_NUM_DATA1,
            DISPLAY_PIN_NUM_DATA2,
            DISPLAY_PIN_NUM_DATA3,
            DISPLAY_PIN_NUM_DATA4,
            DISPLAY_PIN_NUM_DATA5,
            DISPLAY_PIN_NUM_DATA6,
            DISPLAY_PIN_NUM_DATA7,
            DISPLAY_PIN_NUM_DATA8,
            DISPLAY_PIN_NUM_DATA9,
            DISPLAY_PIN_NUM_DATA10,
            DISPLAY_PIN_NUM_DATA11,
            DISPLAY_PIN_NUM_DATA12,
            DISPLAY_PIN_NUM_DATA13,
            DISPLAY_PIN_NUM_DATA14,
            DISPLAY_PIN_NUM_DATA15,
        },
        .timings = {
            .pclk_hz = DISPLAY_LCD_PIXEL_CLOCK_HZ,
            .h_res = DISPLAY_LCD_H_RES,
            .v_res = DISPLAY_LCD_V_RES,
            // The following parameters should refer to LCD spec
            .hsync_back_porch = 8,
            .hsync_front_porch = 8,
            .hsync_pulse_width = 4,
            .vsync_back_porch = 16,
            .vsync_front_porch = 16,
            .vsync_pulse_width = 4,
            .flags.pclk_active_neg = true,
        },
        .flags.fb_in_psram = true, // allocate frame buffer in PSRAM
    };
    ESP_ERROR_CHECK(esp_lcd_new_rgb_panel(&panel_config, &panel_handle));

    ESP_LOGI(TAG, "Register event callbacks");
    esp_lcd_rgb_panel_event_callbacks_t cbs = {
        .on_vsync = display_on_vsync_event,
    };
    ESP_ERROR_CHECK(esp_lcd_rgb_panel_register_event_callbacks(panel_handle, &cbs, &disp_drv));

    ESP_LOGI(TAG, "Initialize RGB LCD panel");
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel_handle));

#if DISPLAY_PIN_NUM_BK_LIGHT >= 0
    ESP_LOGI(TAG, "Turn on LCD backlight");
    gpio_set_level(DISPLAY_PIN_NUM_BK_LIGHT, DISPLAY_LCD_BK_LIGHT_ON_LEVEL);
#endif

    esp_lcd_touch_handle_t tp = NULL;
    esp_lcd_panel_io_handle_t tp_io_handle = NULL;
    esp_lcd_panel_io_i2c_config_t tp_io_config = ESP_LCD_TOUCH_IO_I2C_GT911_CONFIG();
    
    // set Int pin to output temporarily
    gpio_config_struct.pin_bit_mask = (uint64_t)1 << TOUCH_INT;
    gpio_config_struct.mode = GPIO_MODE_OUTPUT;
    gpio_config_struct.pull_up_en = GPIO_PULLUP_DISABLE;
    gpio_config_struct.pull_down_en = GPIO_PULLDOWN_DISABLE;
    gpio_config_struct.intr_type = GPIO_INTR_DISABLE;
    gpio_config(&gpio_config_struct);

    // reset low
	CH422G_write_output(TOUCH_RESET, 0);
    esp_rom_delay_us(100 * 1000);

    // set Int to low/output
    gpio_set_level(TOUCH_INT, 0);
    esp_rom_delay_us(100 * 1000);

    // release reset
    CH422G_write_output(TOUCH_RESET, 1);
    esp_rom_delay_us(200 * 1000);

    // set interrupt to tristate
    gpio_config_struct.mode = GPIO_MODE_INPUT;
    gpio_config_struct.pull_down_en = GPIO_PULLDOWN_ENABLE;
    gpio_config(&gpio_config_struct);

    // Touch IO handle
    if (esp_lcd_new_panel_io_i2c((esp_lcd_i2c_bus_handle_t)I2CNum, &tp_io_config, &tp_io_handle) != ESP_OK)
    {
        ESP_LOGE(TAG, "Touch reset 3 failed!");
    }
    
    esp_lcd_touch_config_t tp_cfg = {
            .x_max = DISPLAY_LCD_V_RES,
            .y_max = DISPLAY_LCD_H_RES,
            .rst_gpio_num = -1,
            .int_gpio_num = -1,
            .flags = {
                .swap_xy = 0,
                .mirror_x = 0,
                .mirror_y = 0,
            },
        };

    /* Initialize touch */
    ESP_LOGI(TAG, "Initialize touch controller GT911");

    // try a few times
    for (int loop = 0; loop < 5; loop++)
    {
        ret = ESP_FAIL;

        if (xSemaphoreTake(I2CMutexHandle, (TickType_t)10000) == pdTRUE)
        {
            ret = (esp_lcd_touch_new_i2c_gt911(tp_io_handle, &tp_cfg, &tp));

            xSemaphoreGive(I2CMutexHandle);
        }
        else
        {
            ESP_LOGE(TAG, "Initialize touch loop mutex timeout");
        }
        
        if (ret == ESP_OK)
        {
            ESP_LOGI(TAG, "Touch controller init OK");
            touch_ok = 1;
            break;
        }
        else
        {
            ESP_LOGI(TAG, "Touch controller init retry %s", esp_err_to_name(ret));

            // reset I2C bus
            i2c_master_reset();
        }
           
        vTaskDelay(pdMS_TO_TICKS(25));    
    }

    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to init touch screen");
    }

    ESP_LOGI(TAG, "Initialize LVGL library");
    lv_init();
    //??? lv_fs_if_init();
    
    void *buf1 = NULL;
    void *buf2 = NULL;
#if CONFIG_DISPLAY_DOUBLE_FB
    ESP_LOGI(TAG, "Use frame buffers as LVGL draw buffers");
    ESP_ERROR_CHECK(esp_lcd_rgb_panel_get_frame_buffer(panel_handle, 2, &buf1, &buf2));
    // initialize LVGL draw buffers
    lv_disp_draw_buf_init(&disp_buf, buf1, buf2, DISPLAY_LCD_H_RES * DISPLAY_LCD_V_RES);
#else
    ESP_LOGI(TAG, "Allocate separate LVGL draw buffers from PSRAM");
    buf1 = heap_caps_malloc(DISPLAY_LCD_H_RES * 100 * sizeof(lv_color_t), MALLOC_CAP_SPIRAM);
    assert(buf1);

    // initialize LVGL draw buffers
    lv_disp_draw_buf_init(&disp_buf, buf1, buf2, DISPLAY_LCD_H_RES * 100);
#endif // CONFIG_DISPLAY_DOUBLE_FB

    ESP_LOGI(TAG, "Register display driver to LVGL");
    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res = DISPLAY_LCD_H_RES;
    disp_drv.ver_res = DISPLAY_LCD_V_RES;
    disp_drv.flush_cb = display_lvgl_flush_cb;
    disp_drv.draw_buf = &disp_buf;
    disp_drv.user_data = panel_handle;
#if CONFIG_DISPLAY_DOUBLE_FB
    disp_drv.full_refresh = true; // the full_refresh mode can maintain the synchronization between the two frame buffers
#endif
    lv_disp_t *disp = lv_disp_drv_register(&disp_drv);

    ESP_LOGI(TAG, "Install LVGL tick timer");
    
    // Tick interface for LVGL (using esp_timer to generate 2ms periodic event)
    const esp_timer_create_args_t lvgl_tick_timer_args = {
        .callback = &display_increase_lvgl_tick,
        .name = "lvgl_tick"
    };

    if (touch_ok)
    {
        static lv_indev_drv_t indev_drv;    // Input device driver (Touch)
        lv_indev_drv_init(&indev_drv);
        indev_drv.type = LV_INDEV_TYPE_POINTER;
        indev_drv.disp = disp;
        indev_drv.read_cb = display_lvgl_touch_cb;
        indev_drv.user_data = tp;

        lv_indev_drv_register(&indev_drv);
    }

    esp_timer_handle_t lvgl_tick_timer = NULL;
    ESP_ERROR_CHECK(esp_timer_create(&lvgl_tick_timer_args, &lvgl_tick_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(lvgl_tick_timer, DISPLAY_LVGL_TICK_PERIOD_MS * 1000));

    vTaskDelay(pdMS_TO_TICKS(10));
    
    lvgl_mux = xSemaphoreCreateRecursiveMutex();
    assert(lvgl_mux);

    // init GUI
    ESP_LOGI(TAG, "Init scene");
    ui_init();

    // create display task
    xTaskCreatePinnedToCore(display_task, "Dsp", DISPLAY_TASK_STACK_SIZE, NULL, DISPLAY_TASK_PRIORITY, NULL, 1);
}

#endif  //CONFIG_TONEX_CONTROLLER_DISPLAY_WAVESHARE_800_480