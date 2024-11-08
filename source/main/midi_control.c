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
#include "esp_bt.h"
#include "esp_gap_ble_api.h"
#include "esp_gattc_api.h"
#include "esp_gatt_defs.h"
#include "esp_bt_main.h"
#include "esp_gatt_common_api.h"
#include "control.h"
#include "task_priorities.h"
#include "midi_control.h"

static const char *TAG = "MidiBT";
#define GATTC_TAG        "GATTC_CLIENT"


// 7772e5db-3868-4112-a1a9-f2669d106bf3   Midi characrteristic
static uint8_t MidiCharacteristicUUIDByteReversed[] = {0xF3, 0x6B, 0x10, 0x9D, 0x66, 0xF2, 0xA9, 0xA1, 0x12, 0x41, 0x68, 0x38, 0xDB, 0xE5, 0x72, 0x77};

// 00002a19-0000-1000-8000-00805f9b34fb  battery level
//static uint8_t BatteryLevelCharacteristicUUIDByteReversed[] = {0xFB, 0x34, 0x9B, 0x5F, 0x80, 0x00, 0x00, 0x80, 0x00, 0x10, 0x00, 0x00, 0x19, 0x2A, 0x00, 0x00};

// others in the MVave Chocolate
// 03b80e5a-ede8-4b33-a751-6ce34ec4c700 - Midi service
// 00002a05-0000-1000-8000-00805f9b34fb - service changed
// 00002a4d-0000-1000-8000-00805f9b34fb - report
// 00002a33-0000-1000-8000-00805f9b34fb - boot mouse
// 0000ae42-0000-1000-8000-00805f9b34fb - unknown
// 0000ae02-0000-1000-8000-00805f9b34fb - unknown

#define PROFILE_A_APP_ID    0
#define INVALID_HANDLE      0

#define BT_SCAN_DURATION    1800    // seconds

// Declare static functions
static void esp_gap_cb(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param);
static void esp_gattc_cb(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if, esp_ble_gattc_cb_param_t *param);
static void gattc_profile_a_event_handler(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if, esp_ble_gattc_cb_param_t *param);


static esp_bt_uuid_t remote_filter_char_uuid_reuse[5]; 

static esp_bt_uuid_t notify_descr_uuid = 
{
    .len = ESP_UUID_LEN_16,
    .uuid = {.uuid16 = ESP_GATT_UUID_CHAR_CLIENT_CONFIG,},
};

static bool conn_device_a   = false;
static bool get_service_a   = false;
static bool Isconnecting    = false;
static bool stop_scan_done  = false;

static uint16_t search_start_handle = 0xFFFF;
static uint16_t search_end_handle = 0;
static esp_gattc_descr_elem_t *descr_elem_result_a  = NULL;

// M-vave Chocolate device name is 'FootCtrl'
static const char remote_device_name[20] = {"FootCtrl"};

static esp_ble_scan_params_t ble_scan_params = 
{
    .scan_type              = BLE_SCAN_TYPE_ACTIVE,
    .own_addr_type          = BLE_ADDR_TYPE_PUBLIC,
    .scan_filter_policy     = BLE_SCAN_FILTER_ALLOW_ALL,
    .scan_interval          = 0x50,
    .scan_window            = 0x30,
    .scan_duplicate         = BLE_SCAN_DUPLICATE_DISABLE
};

struct gattc_profile_inst 
{
    esp_gattc_cb_t gattc_cb;
    uint16_t gattc_if;
    uint16_t app_id;
    uint16_t conn_id;
    uint16_t service_start_handle;
    uint16_t service_end_handle;
    uint16_t char_handle;
    esp_bd_addr_t remote_bda;
};

// One gatt-based profile one app_id and one gattc_if, this array will store the gattc_if returned by ESP_GATTS_REG_EVT
static struct gattc_profile_inst gl_profile_tab = 
{
    .gattc_cb = gattc_profile_a_event_handler,
    .gattc_if = ESP_GATT_IF_NONE,       /* Not get the gatt_if, so initial is ESP_GATT_IF_NONE */
};

/****************************************************************************
* NAME:        
* DESCRIPTION: 
* PARAMETERS:  
* RETURN:      
* NOTES:       
*****************************************************************************/
static void start_scan(void)
{
    stop_scan_done = false;
    Isconnecting = false;
    esp_ble_gap_start_scanning(BT_SCAN_DURATION);
}

/****************************************************************************
* NAME:        
* DESCRIPTION: 
* PARAMETERS:  
* RETURN:      
* NOTES:       
*****************************************************************************/
static void gattc_profile_a_event_handler(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if, esp_ble_gattc_cb_param_t *param)
{
    esp_ble_gattc_cb_param_t *p_data = (esp_ble_gattc_cb_param_t *)param;

    switch (event) 
    {
        case ESP_GATTC_REG_EVT:
            ESP_LOGI(GATTC_TAG, "REG_EVT");
            esp_err_t scan_ret = esp_ble_gap_set_scan_params(&ble_scan_params);
            if (scan_ret)
            {
                ESP_LOGE(GATTC_TAG, "set scan params error, error code = %x", scan_ret);
            }
            break;
        
        /* one device connect successfully, all profiles callback function will get the ESP_GATTC_CONNECT_EVT,
        so must compare the mac address to check which device is connected, so it is a good choice to use ESP_GATTC_OPEN_EVT. */
        case ESP_GATTC_CONNECT_EVT:
            break;

        case ESP_GATTC_OPEN_EVT:
            if (p_data->open.status != ESP_GATT_OK)
            {
                //open failed, ignore the first device, connect the second device
                ESP_LOGE(GATTC_TAG, "connect device failed, status %d", p_data->open.status);
                conn_device_a = false;
                break;
            }

            memcpy(gl_profile_tab.remote_bda, p_data->open.remote_bda, 6);
            gl_profile_tab.conn_id = p_data->open.conn_id;

            ESP_LOGI(GATTC_TAG, "ESP_GATTC_OPEN_EVT conn_id %d, if %d, status %d, mtu %d", p_data->open.conn_id, gattc_if, p_data->open.status, p_data->open.mtu);
            ESP_LOGI(GATTC_TAG, "REMOTE BDA:");
            esp_log_buffer_hex(GATTC_TAG, p_data->open.remote_bda, sizeof(esp_bd_addr_t));
            
            esp_err_t mtu_ret = esp_ble_gattc_send_mtu_req(gattc_if, p_data->open.conn_id);
            if (mtu_ret)
            {
                ESP_LOGE(GATTC_TAG, "config MTU error, error code = %x", mtu_ret);
            }
            break;

        case ESP_GATTC_CFG_MTU_EVT:
            if (param->cfg_mtu.status != ESP_GATT_OK)
            {
                ESP_LOGE(GATTC_TAG,"Config mtu failed");
            }
            
            //ESP_LOGI(GATTC_TAG, "Status %d, MTU %d, conn_id %d", param->cfg_mtu.status, param->cfg_mtu.mtu, param->cfg_mtu.conn_id);
            search_start_handle = 0xFFFF;
            search_end_handle = 0;

            if (esp_ble_gattc_search_service(gattc_if, param->cfg_mtu.conn_id, NULL) != ESP_OK)
            {
                ESP_LOGE(GATTC_TAG, "Failed to start search for UUID");
            }
            else
            {
                ESP_LOGI(GATTC_TAG, "Searching for Midi Service UUID match");
            }
            break;

        case ESP_GATTC_SEARCH_RES_EVT: 
        {
            //ESP_LOGI(GATTC_TAG, "SEARCH RES: conn_id = %x is primary service %d", p_data->search_res.conn_id, p_data->search_res.is_primary);
            //ESP_LOGI(GATTC_TAG, "start handle %d end handle %d current handle value %d", p_data->search_res.start_handle, p_data->search_res.end_handle, p_data->search_res.srvc_id.inst_id);
            
            if (p_data->search_res.start_handle < search_start_handle)
            {
                search_start_handle = p_data->search_res.start_handle;
            }

            if (p_data->search_res.end_handle > search_end_handle)
            {
                search_end_handle = p_data->search_res.end_handle;
            }

            get_service_a = true;
            gl_profile_tab.service_start_handle = search_start_handle;
            gl_profile_tab.service_end_handle = search_end_handle;
            break;
        }
        
        case ESP_GATTC_SEARCH_CMPL_EVT:
            uint16_t count = 1;
            esp_gatt_status_t res;
            ESP_LOGI(GATTC_TAG, "Search complete for Services");

            if (p_data->search_cmpl.status != ESP_GATT_OK)
            {
                ESP_LOGE(GATTC_TAG, "Search service failed, error status = %x", p_data->search_cmpl.status);
                break;
            }
            
            if (get_service_a)
            {                
                // get descriptors for Midi
                esp_gattc_char_elem_t* char_elem_result = (esp_gattc_char_elem_t *)malloc(sizeof(esp_gattc_char_elem_t) * 4);
                if (!char_elem_result)
                {
                    ESP_LOGE(GATTC_TAG, "gattc no mem");
                }
                else 
                {
                    ESP_LOGI(GATTC_TAG, "Finding Characteristics");

                    // init search table
                    uint8_t filter_index = 0;

                    remote_filter_char_uuid_reuse[filter_index].len = ESP_UUID_LEN_128;
                    memcpy((void*)remote_filter_char_uuid_reuse[filter_index].uuid.uuid128, (void*)MidiCharacteristicUUIDByteReversed, ESP_UUID_LEN_128);
                    filter_index++;
                    
                    //remote_filter_char_uuid_reuse[filter_index].len = ESP_UUID_LEN_128;
                    //memcpy((void*)remote_filter_char_uuid_reuse[filter_index].uuid.uuid128, (void*)BatteryLevelCharacteristicUUIDByteReversed, ESP_UUID_LEN_128);
                    //filter_index++;

                    // loop and find all characteristics, and register notification
                    for (int loop = 0; loop < filter_index; loop++)
                    {
                        // find the characteristic
                        count = 1;
                        res = esp_ble_gattc_get_char_by_uuid(gattc_if, 
                                                        p_data->search_cmpl.conn_id, 
                                                        gl_profile_tab.service_start_handle, 
                                                        gl_profile_tab.service_end_handle, 
                                                        remote_filter_char_uuid_reuse[loop], 
                                                        char_elem_result, 
                                                        &count);

                        if (res == ESP_OK)
                        {
                            ESP_LOGI(GATTC_TAG, "Characteristic loop %d get returned %d", loop, count);

                            for (uint32_t character_loop = 0; character_loop < count; character_loop++)
                            {
                                if (char_elem_result[character_loop].properties & ESP_GATT_CHAR_PROP_BIT_NOTIFY)
                                {
                                    gl_profile_tab.char_handle = char_elem_result[character_loop].char_handle;

                                    if (esp_ble_gattc_register_for_notify(gattc_if,  gl_profile_tab.remote_bda, char_elem_result[character_loop].char_handle) != ESP_OK)
                                    {
                                        ESP_LOGE(GATTC_TAG, "esp_ble_gattc_register_for_notify failed %d %d", (int)loop, (int)char_elem_result[character_loop].char_handle);
                                    }
                                    else
                                    {
                                        ESP_LOGI(GATTC_TAG, "esp_ble_gattc_register_for_notify OK %d on handle %d", (int)loop, (int)char_elem_result[character_loop].char_handle);                                

                                        // update UI to show a BT connected
                                        control_set_bt_status(1);
                                    }
                                }  
                            }                   
                        }
                        else
                        {
                            ESP_LOGE(GATTC_TAG, "Failed to find Midi characteristic %d: %d", (int)loop, (int)res);
                        }
                    }

                    free(char_elem_result);
                }
            }
        
            break;

        case ESP_GATTC_REG_FOR_NOTIFY_EVT: 
        {
            if (p_data->reg_for_notify.status != ESP_GATT_OK)
            {
                ESP_LOGE(GATTC_TAG, "reg notify failed, error status =%x", p_data->reg_for_notify.status);
                break;
            }
        
            uint16_t count = 0;
            uint16_t notify_en = 1;

            esp_gatt_status_t ret_status = esp_ble_gattc_get_attr_count( gattc_if,
                                                                        gl_profile_tab.conn_id,
                                                                        ESP_GATT_DB_DESCRIPTOR,
                                                                        gl_profile_tab.service_start_handle,
                                                                        gl_profile_tab.service_end_handle,
                                                                        //????? to do
                                                                        gl_profile_tab.char_handle,
                                                                        &count);
            if (ret_status != ESP_GATT_OK)
            {
                ESP_LOGE(GATTC_TAG, "esp_ble_gattc_get_attr_count error");
            }
            
            if (count > 0)
            {
                descr_elem_result_a = (esp_gattc_descr_elem_t *)malloc(sizeof(esp_gattc_descr_elem_t) * count);

                if (!descr_elem_result_a)
                {
                    ESP_LOGE(GATTC_TAG, "malloc error, gattc no mem");
                }
                else
                {
                    ret_status = esp_ble_gattc_get_descr_by_char_handle(gattc_if,
                                                                        gl_profile_tab.conn_id,
                                                                        p_data->reg_for_notify.handle,
                                                                        notify_descr_uuid,
                                                                        descr_elem_result_a,
                                                                        &count);

                    if (ret_status != ESP_GATT_OK)
                    {
                        ESP_LOGE(GATTC_TAG, "esp_ble_gattc_get_descr_by_char_handle error %d", (int)ret_status);
                    }

                    if (count > 0 && descr_elem_result_a[0].uuid.len == ESP_UUID_LEN_16 && descr_elem_result_a[0].uuid.uuid.uuid16 == ESP_GATT_UUID_CHAR_CLIENT_CONFIG)
                    {
                        ret_status = esp_ble_gattc_write_char_descr(gattc_if,
                                                                    gl_profile_tab.conn_id,
                                                                    descr_elem_result_a[0].handle,
                                                                    sizeof(notify_en),
                                                                    (uint8_t*)&notify_en,
                                                                    ESP_GATT_WRITE_TYPE_RSP,
                                                                    ESP_GATT_AUTH_REQ_NONE);
                    }

                    if (ret_status != ESP_GATT_OK)
                    {
                        ESP_LOGE(GATTC_TAG, "esp_ble_gattc_write_char_descr error %d", (int)ret_status);
                    }

                    // free descr_elem_result
                    free(descr_elem_result_a);
                }
            }
            else
            {
                ESP_LOGE(GATTC_TAG, "decsr not found");
            }
            break;
        }

        case ESP_GATTC_NOTIFY_EVT:
            //ESP_LOGI(GATTC_TAG, "ESP_GATTC_NOTIFY_EVT, Receive notify value:");
            //esp_log_buffer_hex(GATTC_TAG, p_data->notify.value, p_data->notify.value_len);

            // check Midi data. Program change Should be 0x80 0x80 0xC0 XX (XX = preset index, 0-based)
            if (p_data->notify.value_len >= 4)
            {
                if ((p_data->notify.value[0] == 0x80) && (p_data->notify.value[1] == 0x80))
                {
                    if (p_data->notify.value[2] == 0xC0) 
                    {
                        // set preset
                        control_request_preset_index(p_data->notify.value[3]);
                    }
                    else if (p_data->notify.value[2] == 0xB0) 
                    {
                        // bank change
                    }
                }
            }
            break;

        case ESP_GATTC_WRITE_DESCR_EVT:
            if (p_data->write.status != ESP_GATT_OK)
            {
                ESP_LOGE(GATTC_TAG, "write descr failed, error status = %x", p_data->write.status);
                break;
            }
            ESP_LOGI(GATTC_TAG, "write descr success");
            
            uint8_t write_char_data[35];
            for (int i = 0; i < sizeof(write_char_data); ++i)
            {
                write_char_data[i] = i % 256;
            }
            esp_ble_gattc_write_char( gattc_if,
                                    gl_profile_tab.conn_id,
                                    gl_profile_tab.char_handle,
                                    sizeof(write_char_data),
                                    write_char_data,
                                    ESP_GATT_WRITE_TYPE_RSP,
                                    ESP_GATT_AUTH_REQ_NONE);
            break;

        case ESP_GATTC_WRITE_CHAR_EVT:
            if (p_data->write.status != ESP_GATT_OK)
            {
                ESP_LOGE(GATTC_TAG, "write char failed, error status = %x", p_data->write.status);
            }
            else
            {
                ESP_LOGI(GATTC_TAG, "write char success");
            }
            start_scan();
            break;

        case ESP_GATTC_SRVC_CHG_EVT: 
        {
            esp_bd_addr_t bda;
            memcpy(bda, p_data->srvc_chg.remote_bda, sizeof(esp_bd_addr_t));
            ESP_LOGI(GATTC_TAG, "ESP_GATTC_SRVC_CHG_EVT, bd_addr:%08x%04x",(bda[0] << 24) + (bda[1] << 16) + (bda[2] << 8) + bda[3], (bda[4] << 8) + bda[5]);
            break;
        }
        case ESP_GATTC_DISCONNECT_EVT:
            //Start scanning again
            start_scan();

            // update UI to show a BT disconnected
            control_set_bt_status(0);

            if (memcmp(p_data->disconnect.remote_bda, gl_profile_tab.remote_bda, 6) == 0)
            {
                ESP_LOGI(GATTC_TAG, "Device A disconnected");
                conn_device_a = false;
                get_service_a = false;
            }
            break;
            
        default:
            break;
    }
}

/****************************************************************************
* NAME:        
* DESCRIPTION: 
* PARAMETERS:  
* RETURN:      
* NOTES:       
*****************************************************************************/
static void esp_gap_cb(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param)
{
    uint8_t *adv_name = NULL;
    uint8_t adv_name_len = 0;

    switch (event) 
    {
        case ESP_GAP_BLE_UPDATE_CONN_PARAMS_EVT:
            ESP_LOGI(GATTC_TAG, "update connection params status = %d, min_int = %d, max_int = %d,conn_int = %d,latency = %d, timeout = %d",
                    param->update_conn_params.status,
                    param->update_conn_params.min_int,
                    param->update_conn_params.max_int,
                    param->update_conn_params.conn_int,
                    param->update_conn_params.latency,
                    param->update_conn_params.timeout);
            break;

        case ESP_GAP_BLE_SCAN_PARAM_SET_COMPLETE_EVT: 
        {
            //the unit of the duration is second
            esp_ble_gap_start_scanning(BT_SCAN_DURATION);
            break;
        }
        
        case ESP_GAP_BLE_SCAN_START_COMPLETE_EVT:
            //scan start complete event to indicate scan start successfully or failed
            if (param->scan_start_cmpl.status == ESP_BT_STATUS_SUCCESS) 
            {
                ESP_LOGI(GATTC_TAG, "Scan start success");
            }
            else
            {
                ESP_LOGE(GATTC_TAG, "Scan start failed");
            }
            break;

        case ESP_GAP_BLE_SCAN_RESULT_EVT: 
        {
            esp_ble_gap_cb_param_t *scan_result = (esp_ble_gap_cb_param_t *)param;
            
            switch (scan_result->scan_rst.search_evt) 
            {
            case ESP_GAP_SEARCH_INQ_RES_EVT:
                //esp_log_buffer_hex(GATTC_TAG, scan_result->scan_rst.bda, 6);
                //ESP_LOGI(GATTC_TAG, "Searched Adv Data Len %d, Scan Response Len %d", scan_result->scan_rst.adv_data_len, scan_result->scan_rst.scan_rsp_len);
                adv_name = esp_ble_resolve_adv_data(scan_result->scan_rst.ble_adv, ESP_BLE_AD_TYPE_NAME_CMPL, &adv_name_len);
                //ESP_LOGI(GATTC_TAG, "Searched Device Name Len %d", adv_name_len);
                //esp_log_buffer_char(GATTC_TAG, adv_name, adv_name_len);
                //ESP_LOGI(GATTC_TAG, "\n");
                
                if (Isconnecting)
                {
                    break;
                }
                
                if (conn_device_a && !stop_scan_done)
                {
                    stop_scan_done = true;
                    esp_ble_gap_stop_scanning();
                    ESP_LOGI(GATTC_TAG, "Device is connected, stoppiong scan");
                    break;
                }
                
                if (adv_name != NULL) 
                {
                    if (strlen(remote_device_name) == adv_name_len && strncmp((char *)adv_name, remote_device_name, adv_name_len) == 0) 
                    {
                        if (conn_device_a == false) 
                        {
                            conn_device_a = true;
                            ESP_LOGI(GATTC_TAG, "Searched device %s", remote_device_name);
                            esp_ble_gap_stop_scanning();
                            esp_ble_gattc_open(gl_profile_tab.gattc_if, scan_result->scan_rst.bda, scan_result->scan_rst.ble_addr_type, true);
                            Isconnecting = true;
                        }
                        break;
                    }
                }
                break;
            
            case ESP_GAP_SEARCH_INQ_CMPL_EVT:
                break;

            default:
                break;
            }
        break;
    }

    case ESP_GAP_BLE_SCAN_STOP_COMPLETE_EVT:
        if (param->scan_stop_cmpl.status != ESP_BT_STATUS_SUCCESS)
        {
            ESP_LOGE(GATTC_TAG, "Scan stop failed");
            break;
        }
        ESP_LOGI(GATTC_TAG, "Stop scan successfully");

        break;

    case ESP_GAP_BLE_ADV_STOP_COMPLETE_EVT:
        if (param->adv_stop_cmpl.status != ESP_BT_STATUS_SUCCESS)
        {
            ESP_LOGE(GATTC_TAG, "Adv stop failed");
            break;
        }
        ESP_LOGI(GATTC_TAG, "Stop adv successfully");
        break;

    default:
        break;
    }
}

/****************************************************************************
* NAME:        
* DESCRIPTION: 
* PARAMETERS:  
* RETURN:      
* NOTES:       
*****************************************************************************/
static void esp_gattc_cb(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if, esp_ble_gattc_cb_param_t *param)
{
    //ESP_LOGI(GATTC_TAG, "EVT %d, gattc if %d, app_id %d", event, gattc_if, param->reg.app_id);

    /* If event is register event, store the gattc_if for each profile */
    if (event == ESP_GATTC_REG_EVT) 
    {
        if (param->reg.status == ESP_GATT_OK) 
        {
            gl_profile_tab.gattc_if = gattc_if;
        } 
        else 
        {
            ESP_LOGI(GATTC_TAG, "Reg app failed, app_id %04x, status %d", param->reg.app_id, param->reg.status);
            return;
        }
    }

    /* If the gattc_if equal to profile A, call profile A cb handler,
     * so here call each profile's callback */
    do 
    {
        if (gattc_if == ESP_GATT_IF_NONE || /* ESP_GATT_IF_NONE, not specify a certain gatt_if, need to call every profile cb function */
                gattc_if == gl_profile_tab.gattc_if) 
        {
            if (gl_profile_tab.gattc_cb) 
            {
                gl_profile_tab.gattc_cb(event, gattc_if, param);
            }
        }
    } while (0);
}

/****************************************************************************
* NAME:        
* DESCRIPTION: 
* PARAMETERS:  
* RETURN:      
* NOTES:       
*****************************************************************************/
static void init_BLE(void)
{
    esp_err_t ret;

    ESP_LOGI(TAG, "Midi BLE init start");

    ESP_ERROR_CHECK(esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT));

    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    ret = esp_bt_controller_init(&bt_cfg);
    if (ret) 
    {
        ESP_LOGE(GATTC_TAG, "%s initialize controller failed: %s\n", __func__, esp_err_to_name(ret));
        return;
    }

    ret = esp_bt_controller_enable(ESP_BT_MODE_BLE);
    if (ret) 
    {
        ESP_LOGE(GATTC_TAG, "%s enable controller failed: %s\n", __func__, esp_err_to_name(ret));
        return;
    }

    ret = esp_bluedroid_init();
    if (ret) 
    {
        ESP_LOGE(GATTC_TAG, "%s init bluetooth failed: %s\n", __func__, esp_err_to_name(ret));
        return;
    }

    ret = esp_bluedroid_enable();
    if (ret) 
    {
        ESP_LOGE(GATTC_TAG, "%s enable bluetooth failed: %s\n", __func__, esp_err_to_name(ret));
        return;
    }

    // register the  callback function to the gap module
    ret = esp_ble_gap_register_callback(esp_gap_cb);
    if (ret)
    {
        ESP_LOGE(GATTC_TAG, "gap register error, error code = %x", ret);
        return;
    }

    // register the callback function to the gattc module
    ret = esp_ble_gattc_register_callback(esp_gattc_cb);
    if(ret)
    {
        ESP_LOGE(GATTC_TAG, "gattc register error, error code = %x", ret);
        return;
    }

    ret = esp_ble_gattc_app_register(PROFILE_A_APP_ID);
    if (ret)
    {
        ESP_LOGE(GATTC_TAG, "gattc app register error, error code = %x", ret);
        return;
    }

    ret = esp_ble_gatt_set_local_mtu(200);
    if (ret)
    {
        ESP_LOGE(GATTC_TAG, "set local  MTU failed, error code = %x", ret);
    }
}

/****************************************************************************
* NAME:        
* DESCRIPTION: 
* PARAMETERS:  
* RETURN:      
* NOTES:       
*****************************************************************************/
void midi_init(void)
{
    init_BLE();
}
