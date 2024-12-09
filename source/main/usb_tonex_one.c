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

//Tonex One device
//-----------------------------
//idVendor = 0x1963
//idProduct = 0x00D1

//Index  LANGID  String
//0x00   0x0000  0x0409 
//0x01   0x0409  "IK Multimedia"
//0x02   0x0409  "ToneX One"
//0x04   0x0409  "ToneX One Record"
//0x05   0x0409  "ToneX One Playback"
//0x06   0x0409  "ToneX Control VCOM"
//0x09   0x0409  "ToneX One USB Input"
//0x0A   0x0409  "ToneX One USB Output"
//0x0B   0x0409  "ToneX One Internal Clock"
//0x0C   0x0409  "ToneX One In 1"
//0x0D   0x0409  "ToneX One In 2"
//0x10   0x0409  "ToneX One Out 1"
//0x11   0x0409  "ToneX One Out 2"
//0x12   0x0409  "xxxxxxxxxxxxxxxxxxxx"     // Serial Number

//Composite Device.
//- 5 interfaces total
//- Communications Device Class "Tonex Control VCOM"
//- Audio Device Class "Audio Protocol IP version 2.00"

//----------------------------------------------------
//Endpoint 7 is the Control endpoint, using CDC protocol.

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/ringbuf.h"
#include "esp_log.h"
#include "usb/usb_host.h"
#include "usb_comms.h"
#include "usb/cdc_acm_host.h"
#include "driver/i2c.h"
#include "usb_comms.h"
#include "usb_tonex_one.h"
#include "control.h"

static const char *TAG = "app_TonexOne";

#define MAX_PRESETS         20
#define MAX_TX_SIZE         64

// Response from Tonex One to a preset change is about 1202, 1352, 1361 bytes with details of the preset. 
// preset name is proceeded by this byte sequence:
static const uint8_t ToneOnePresetByteMarker[] = {0xB9, 0x04, 0xB9, 0x02, 0xBC, 0x21};

// lengths of preset name and drive character
#define TONEX_ONE_RESP_OFFSET_PRESET_NAME_LEN       32

// Tonex One can send quite large data quickly, so make a generous receive buffer
#define RX_TEMP_BUFFER_SIZE                         3072

#define TONEX_ONE_CDC_INTERFACE_INDEX               0
#define MAX_RAW_DATA                                3072

// credit to https://github.com/vit3k/tonex_controller for some of the below details and implementation
enum CommsState
{
    COMMS_STATE_IDLE,
    COMMS_STATE_HELLO,
    COMMS_STATE_READY,
    COMMS_STATE_GET_STATE
};

typedef enum Status 
{
    STATUS_OK,
    STATUS_INVALID_FRAME,
    STATUS_INVALID_ESCAPE_SEQUENCE,
    STATUS_CRC_MISMATCH
} Status;

typedef enum Type 
{
    TYPE_UNKNOWN,
    TYPE_STATE_UPDATE,
    TYPE_HELLO
} Type;

typedef enum Slot
{
    A = 0,
    B = 1,
    C = 2
} Slot;

typedef struct __attribute__ ((packed)) 
{
    Type type;
    uint16_t size;
    uint16_t unknown;
} tHeader;

typedef struct __attribute__ ((packed)) 
{
    uint8_t RawData[MAX_RAW_DATA];
    uint16_t Length;
} tPedalData;

typedef struct __attribute__ ((packed)) 
{
    tHeader Header;
    uint8_t SlotAPreset;
    uint8_t SlotBPreset;
    uint8_t SlotCPreset;
    Slot CurrentSlot;
    tPedalData PedalData;
} tTonexMessage;

typedef struct __attribute__ ((packed)) 
{
    tTonexMessage Message;
    uint8_t TonexState;
} tTonexData;

typedef struct __attribute__ ((packed)) 
{
    uint8_t Data[MAX_RAW_DATA];
    uint16_t Length;
} tCDCRxQueueEntry;

/*
** Static vars
*/
static cdc_acm_dev_hdl_t cdc_dev;
static tTonexData TonexData;
static char preset_name[TONEX_ONE_RESP_OFFSET_PRESET_NAME_LEN + 1];
static uint8_t TxBuffer[MAX_RAW_DATA];
static uint8_t FramedBuffer[MAX_RAW_DATA];
static QueueHandle_t input_queue;
static QueueHandle_t data_rx_queue;
static tCDCRxQueueEntry rx_entry;
static tCDCRxQueueEntry rx_entry_int;
static uint8_t boot_init_needed = 0;

/*
** Static function prototypes
*/
static uint16_t addFraming(uint8_t* input, uint16_t inlength, uint8_t* output);
static Status removeFraming(uint8_t* input, uint16_t inlength, uint8_t* output, uint16_t* outlength);
static esp_err_t usb_tonex_one_transmit(uint8_t* tx_data, uint16_t tx_len);
static Status usb_tonex_one_parse(uint8_t* message, uint16_t inlength);
static esp_err_t usb_tonex_one_set_active_slot(Slot newSlot);
static esp_err_t usb_tonex_one_set_preset_in_slot(uint16_t preset, Slot newSlot, uint8_t selectSlot);
static uint16_t usb_tonex_one_get_current_active_preset(void);

/****************************************************************************
* NAME:        
* DESCRIPTION: 
* PARAMETERS:  
* RETURN:      
* NOTES:       
*****************************************************************************/
static uint16_t calculateCRC(uint8_t* data, uint16_t length) 
{
    uint16_t crc = 0xFFFF;

    for (uint16_t loop = 0; loop < length; loop++) 
    {
        crc ^= data[loop];

        for (uint8_t i = 0; i < 8; ++i) 
        {
            if (crc & 1) 
            {
                crc = (crc >> 1) ^ 0x8408;  // 0x8408 is the reversed polynomial x^16 + x^12 + x^5 + 1
            } 
            else 
            {
                crc = crc >> 1;
            }
        }
    }
    
    return ~crc;
}

/****************************************************************************
* NAME:        
* DESCRIPTION: 
* PARAMETERS:  
* RETURN:      
* NOTES:       
*****************************************************************************/
static uint16_t addByteWithStuffing(uint8_t* output, uint8_t byte) 
{
    uint16_t length = 0;

    if (byte == 0x7E || byte == 0x7D) 
    {
        output[length] = 0x7D;
        length++;
        output[length] = byte ^ 0x20;
        length++;
    }
    else 
    {
        output[length] = byte;
        length++;
    }

    return length;
}

/****************************************************************************
* NAME:        
* DESCRIPTION: 
* PARAMETERS:  
* RETURN:      
* NOTES:       
*****************************************************************************/
static uint16_t addFraming(uint8_t* input, uint16_t inlength, uint8_t* output)
{
    uint16_t outlength = 0;

    // Start flag
    output[outlength] = 0x7E;
    outlength++;

    // add input bytes
    for (uint16_t byte = 0; byte < inlength; byte++) 
    {
        outlength += addByteWithStuffing(&output[outlength], input[byte]);
    }

    // add CRC
    uint16_t crc = calculateCRC(input, inlength);
    outlength += addByteWithStuffing(&output[outlength], crc & 0xFF);
    outlength += addByteWithStuffing(&output[outlength], (crc >> 8) & 0xFF);

    // End flag
    output[outlength] = 0x7E;
    outlength++;

    return outlength;
}

/****************************************************************************
* NAME:        
* DESCRIPTION: 
* PARAMETERS:  
* RETURN:      
* NOTES:       
*****************************************************************************/
static Status removeFraming(uint8_t* input, uint16_t inlength, uint8_t* output, uint16_t* outlength)
{
    *outlength = 0;
    uint8_t* output_ptr = output;

    if ((inlength < 4) || (input[0] != 0x7E) || (input[inlength - 1] != 0x7E))
    {
        ESP_LOGE(TAG, "Invalid Frame (1)");
        return STATUS_INVALID_FRAME;
    }

    for (uint16_t i = 1; i < inlength - 1; ++i) 
    {
        if (input[i] == 0x7D) 
        {
            if ((i + 1) >= (inlength - 1))
            {
                ESP_LOGE(TAG, "Invalid Escape sequence");
                return STATUS_INVALID_ESCAPE_SEQUENCE;
            }

            *output_ptr = input[i + 1] ^ 0x20;
            output_ptr++;
            (*outlength)++;
            ++i;
        } 
        else if (input[i] == 0x7E) 
        {
            break;
        } 
        else 
        {
            *output_ptr = input[i];
            output_ptr++;
            (*outlength)++;
        }
    }

    if (*outlength < 2) 
    {
        ESP_LOGE(TAG, "Invalid Frame (2)");
        return STATUS_INVALID_FRAME;
    }

    //ESP_LOGI(TAG, "In:");
    //ESP_LOG_BUFFER_HEXDUMP(TAG, input, inlength, ESP_LOG_INFO);
    //ESP_LOGI(TAG, "Out:");
    //ESP_LOG_BUFFER_HEXDUMP(TAG, output, *outlength, ESP_LOG_INFO);

    uint16_t received_crc = (output[(*outlength) - 1] << 8) | output[(*outlength) - 2];
    (*outlength) -= 2;

    uint16_t calculated_crc = calculateCRC(output, *outlength);

    if (received_crc != calculated_crc) 
    {
        ESP_LOGE(TAG, "Crc mismatch: %X, %X", (int)received_crc, (int)calculated_crc);
        return STATUS_CRC_MISMATCH;
    }

    return STATUS_OK;
}

/****************************************************************************
* NAME:        
* DESCRIPTION: 
* PARAMETERS:  
* RETURN:      
* NOTES:       
*****************************************************************************/
static esp_err_t usb_tonex_one_hello(void)
{
    uint16_t outlength;
    
    ESP_LOGI(TAG, "Sending Hello");

    // build message
    uint8_t request[] = {0xb9, 0x03, 0x00, 0x82, 0x04, 0x00, 0x80, 0x0b, 0x01, 0xb9, 0x02, 0x02, 0x0b};

    // add framing
    outlength = addFraming(request, sizeof(request), FramedBuffer);

    // send it
    return usb_tonex_one_transmit(FramedBuffer, outlength);
}

/****************************************************************************
* NAME:        
* DESCRIPTION: 
* PARAMETERS:  
* RETURN:      
* NOTES:       
*****************************************************************************/
static esp_err_t usb_tonex_one_request_state(void)
{
    uint16_t outlength;

    // build message
    uint8_t request[] = {0xb9, 0x03, 0x00, 0x82, 0x06, 0x00, 0x80, 0x0b, 0x03, 0xb9, 0x02, 0x81, 0x06, 0x03, 0x0b};

    // add framing
    outlength = addFraming(request, sizeof(request), FramedBuffer);

    // send it
    return usb_tonex_one_transmit(FramedBuffer, outlength);
}

/****************************************************************************
* NAME:        
* DESCRIPTION: 
* PARAMETERS:  
* RETURN:      
* NOTES:       
*****************************************************************************/
static esp_err_t __attribute__((unused)) usb_tonex_one_set_active_slot(Slot newSlot)
{
    uint16_t framed_length;

    ESP_LOGI(TAG, "Setting slot %d", (int)newSlot);

    // Build message, length to 0 for now                    len LSB  len MSB
    uint8_t message[] = {0xb9, 0x03, 0x81, 0x06, 0x03, 0x82, 0,       0,       0x80, 0x0b, 0x03};
    
    // set length 
    message[6] = TonexData.Message.PedalData.Length & 0xFF;
    message[7] = (TonexData.Message.PedalData.Length >> 8) & 0xFF;

    // save the slot
    TonexData.Message.CurrentSlot = newSlot;

    // firmware v1.1.4: offset needed is 12
    // firmware v1.2.6: offset needed is 18
    //todo could do version check and support multiple versions
    uint8_t offset_from_end = 18;

    // modify the buffer with the new slot
    TonexData.Message.PedalData.RawData[TonexData.Message.PedalData.Length - offset_from_end + 7] = (uint8_t)newSlot;

    // build total message
    memcpy((void*)TxBuffer, (void*)message, sizeof(message));
    memcpy((void*)&TxBuffer[sizeof(message)], (void*)TonexData.Message.PedalData.RawData, TonexData.Message.PedalData.Length);

    // add framing
    framed_length = addFraming(TxBuffer, sizeof(message) + TonexData.Message.PedalData.Length, FramedBuffer);

    // send it
    return usb_tonex_one_transmit(FramedBuffer, framed_length);    
}

/****************************************************************************
* NAME:        
* DESCRIPTION: 
* PARAMETERS:  
* RETURN:      
* NOTES:       
*****************************************************************************/
static esp_err_t usb_tonex_one_set_preset_in_slot(uint16_t preset, Slot newSlot, uint8_t selectSlot)
{
    uint16_t framed_length;
    
    // firmware v1.1.4: offset needed is 12
    // firmware v1.2.6: offset needed is 18
    //todo could do version check and support multiple versions
    uint8_t offset_from_end = 18;

    ESP_LOGI(TAG, "Setting preset %d in slot %d", (int)preset, (int)newSlot);

    // Build message, length to 0 for now                    len LSB  len MSB
    uint8_t message[] = {0xb9, 0x03, 0x81, 0x06, 0x03, 0x82, 0,       0,       0x80, 0x0b, 0x03};
    
    // set length 
    message[6] = TonexData.Message.PedalData.Length & 0xFF;
    message[7] = (TonexData.Message.PedalData.Length >> 8) & 0xFF;

    // force pedal to Stomp mode. 0 here = A/B mode, 1 = stomp mode
    TonexData.Message.PedalData.RawData[14] = 1;
    
    // check if setting same preset twice will set bypass
    if (control_get_config_double_toggle())
    {
        if (selectSlot && (TonexData.Message.CurrentSlot == newSlot) && (preset == usb_tonex_one_get_current_active_preset()))
        {
            // are we in bypass mode?
            if (TonexData.Message.PedalData.RawData[TonexData.Message.PedalData.Length - offset_from_end + 6] == 1)
            {
                ESP_LOGI(TAG, "Disabling bypass mode");

                // disable bypass mode
                TonexData.Message.PedalData.RawData[TonexData.Message.PedalData.Length - offset_from_end + 6] = 0;
            }
            else
            {
                ESP_LOGI(TAG, "Enabling bypass mode");

                // enable bypass mode
                TonexData.Message.PedalData.RawData[TonexData.Message.PedalData.Length - offset_from_end + 6] = 1;
            }
        }
        else
        {
            // new preset, disable bypass mode to be sure
            TonexData.Message.PedalData.RawData[TonexData.Message.PedalData.Length - offset_from_end + 6] = 0;
        }
    }

    TonexData.Message.CurrentSlot = newSlot;

  
    // set the preset index into the slot position
    switch (newSlot)
    {
        case A:
        {
            TonexData.Message.PedalData.RawData[TonexData.Message.PedalData.Length - offset_from_end] = preset;
        } break;

        case B:
        {
            TonexData.Message.PedalData.RawData[TonexData.Message.PedalData.Length - offset_from_end + 2] = preset;
        } break;

        case C:
        {
            TonexData.Message.PedalData.RawData[TonexData.Message.PedalData.Length - offset_from_end + 4] = preset;
        } break;
    }

    if (selectSlot)
    {
        // modify the buffer with the new slot
        TonexData.Message.PedalData.RawData[TonexData.Message.PedalData.Length - offset_from_end + 7] = (uint8_t)newSlot;
    }

    //ESP_LOGI(TAG, "State Data after changes");
    //ESP_LOG_BUFFER_HEXDUMP(TAG, TonexData.Message.PedalData.RawData, TonexData.Message.PedalData.Length, ESP_LOG_INFO);

    // build total message
    memcpy((void*)TxBuffer, (void*)message, sizeof(message));
    memcpy((void*)&TxBuffer[sizeof(message)], (void*)TonexData.Message.PedalData.RawData, TonexData.Message.PedalData.Length);

    // do framing
    framed_length = addFraming(TxBuffer, sizeof(message) + TonexData.Message.PedalData.Length, FramedBuffer);

    // send it
    return usb_tonex_one_transmit(FramedBuffer, framed_length);
}

/****************************************************************************
* NAME:        
* DESCRIPTION: 
* PARAMETERS:  
* RETURN:      
* NOTES:       
*****************************************************************************/
static bool usb_tonex_one_handle_rx(const uint8_t* data, size_t data_len, void* arg)
{
    ESP_LOGI(TAG, "CDC Data received %d", (int)data_len);
    //ESP_LOG_BUFFER_HEXDUMP(TAG, data, data_len, ESP_LOG_INFO);

    rx_entry_int.Length = data_len;
    memcpy((void*)rx_entry_int.Data, (void*)data, data_len);

    // add to queue
    if (xQueueSendFromISR(data_rx_queue, (void*)&rx_entry_int, NULL) != pdTRUE) 
    {
        ESP_LOGE(TAG, "Failed to send message to rx queue!");
        return false;
    }

    return true;
}

/****************************************************************************
* NAME:        
* DESCRIPTION: 
* PARAMETERS:  
* RETURN:      
* NOTES:       
*****************************************************************************/
static esp_err_t usb_tonex_one_transmit(uint8_t* tx_data, uint16_t tx_len)
{
    esp_err_t ret = ESP_FAIL;

    ret = cdc_acm_host_data_tx_blocking(cdc_dev, tx_data, tx_len, 500);
        
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "cdc_acm_host_data_tx_blocking() failed: %s", esp_err_to_name(ret));   
    }

    return ret;
}

/****************************************************************************
* NAME:        
* DESCRIPTION: 
* PARAMETERS:  
* RETURN:      
* NOTES:       
*****************************************************************************/
uint16_t usb_tonex_one_parse_value(uint8_t* message, uint8_t* index)
{
    uint16_t value = 0;
    
    if (message[*index] == 0x81 || message[*index] == 0x82)
    {
        value = (message[(*index) + 2] << 8) | message[(*index) + 1];
        (*index) += 3;
    }
    else if (message[*index] == 0x80)
    {
        value = message[(*index) + 1];
        (*index) += 2;
    }
    else
    {
        value = message[*index];
        (*index)++;
    }
    
    return value;
}

/****************************************************************************
* NAME:        
* DESCRIPTION: 
* PARAMETERS:  
* RETURN:      
* NOTES:       
*****************************************************************************/
static Status usb_tonex_one_parse_state(uint8_t* unframed, uint16_t length, uint16_t index)
{
    TonexData.Message.Header.type = TYPE_STATE_UPDATE;

    TonexData.Message.PedalData.Length = length - index;
    memcpy((void*)TonexData.Message.PedalData.RawData, (void*)&unframed[index], TonexData.Message.PedalData.Length);

    // firmware v1.1.4: offset needed is 12
    // firmware v1.2.6: offset needed is 18
    //todo could do version check and support multiple versions
    uint8_t offset_from_end = 18;

    index += (TonexData.Message.PedalData.Length - offset_from_end);
    TonexData.Message.SlotAPreset = unframed[index];
    index += 2;
    TonexData.Message.SlotBPreset = unframed[index];
    index += 2;
    TonexData.Message.SlotCPreset = unframed[index];
    index += 3;
    TonexData.Message.CurrentSlot = unframed[index];

    ESP_LOGI(TAG, "Slot A: %d. Slot B:%d. Slot C:%d. Current slot: %d", (int)TonexData.Message.SlotAPreset, (int)TonexData.Message.SlotBPreset, (int)TonexData.Message.SlotCPreset, (int)TonexData.Message.CurrentSlot);

    //ESP_LOGI(TAG, "State Data Rx: %d %d", (int)length, (int)index);
    //ESP_LOG_BUFFER_HEXDUMP(TAG, TonexData.Message.PedalData.RawData, TonexData.Message.PedalData.Length, ESP_LOG_INFO);

    return STATUS_OK;
}

/****************************************************************************
* NAME:        
* DESCRIPTION: 
* PARAMETERS:  
* RETURN:      
* NOTES:       
*****************************************************************************/
static uint16_t usb_tonex_one_get_current_active_preset(void)
{
    uint16_t result = 0;

    switch (TonexData.Message.CurrentSlot)
    {
        case A:
        {        
            result = TonexData.Message.SlotAPreset;
        } break;
    
        case B:
        {
            result = TonexData.Message.SlotBPreset;
        } break;
    
        case C:
        default:
        {
            result = TonexData.Message.SlotCPreset;
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
static Status usb_tonex_one_parse(uint8_t* message, uint16_t inlength)
{
    uint16_t out_len = 0;

    Status status = removeFraming(message, inlength, FramedBuffer, &out_len);

    if (status != STATUS_OK)
    {
        ESP_LOGE(TAG, "Remove framing failed");
        return STATUS_INVALID_FRAME;
    }
    
    if (out_len < 5)
    {
        ESP_LOGE(TAG, "Message too short");
        return STATUS_INVALID_FRAME;
    }
    
    if ((FramedBuffer[0] != 0xB9) || (FramedBuffer[1] != 0x03))
    {
        ESP_LOGE(TAG, "Invalid header");
        return STATUS_INVALID_FRAME;
    }
    
    tHeader header;
    uint8_t index = 2;
    uint16_t type = usb_tonex_one_parse_value(FramedBuffer, &index);

    switch (type)
    {
        case 0x0306:
        {
            header.type = TYPE_STATE_UPDATE;
        } break;

        case 0x02:
        {
            header.type = TYPE_HELLO;
        } break;

        default:
        {
            ESP_LOGI(TAG, "Unknown type %d", (int)type);            
            header.type = TYPE_UNKNOWN;
        } break;
    };
    
    header.size = usb_tonex_one_parse_value(FramedBuffer, &index);
    header.unknown = usb_tonex_one_parse_value(FramedBuffer, &index);

    //ESP_LOGI(TAG, "Structure ID: %d", header.type);
    //ESP_LOGI(TAG, "Size: %d", header.size);

    if ((out_len - index) != header.size)
    {
        ESP_LOGE(TAG, "Invalid message size");
        return STATUS_INVALID_FRAME;
    }

    // check message type
    switch (header.type)
    {
        case TYPE_HELLO:
        {
            ESP_LOGI(TAG, "Hello response");
            memcpy((void*)&TonexData.Message.Header,  (void*)&header, sizeof(header));        
            return STATUS_OK;
        }

        case TYPE_STATE_UPDATE:
        {
            return usb_tonex_one_parse_state(FramedBuffer, out_len, index);
        }
        
        default:
        {
            ESP_LOGI(TAG, "Unknown structure. Skipping.");            
            memcpy((void*)&TonexData.Message.Header, (void*)&header, sizeof(header));
            return STATUS_OK;
        }
    };
}

/****************************************************************************
* NAME:        
* DESCRIPTION: 
* PARAMETERS:  
* RETURN:      
* NOTES:       
*****************************************************************************/
void usb_tonex_one_handle(class_driver_t* driver_obj)
{    
    void* temp_ptr;  
    tUSBMessage message;

    // check state
    switch (TonexData.TonexState)
    {
        case COMMS_STATE_IDLE:
        default:
        {
            // do the hello 
            if (usb_tonex_one_hello() == ESP_OK)
            {
                TonexData.TonexState = COMMS_STATE_HELLO;
            }
            else
            {
                ESP_LOGI(TAG, "Send Hello failed");
            }
        } break;

        case COMMS_STATE_HELLO:
        {
            // waiting for response to arrive
        } break;

        case COMMS_STATE_READY:
        {
            // check for any input messages
            if (xQueueReceive(input_queue, (void*)&message, 0) == pdPASS)
            {
                ESP_LOGI(TAG, "Got Input message: %d", message.Command);

                // process it
                switch (message.Command)
                {
                    case USB_COMMAND_SET_PRESET:
                    {
                        if (message.Payload < MAX_PRESETS)
                        {
                            // always using Stomp mode C for preset setting
                            if (usb_tonex_one_set_preset_in_slot(message.Payload, C, 1) != ESP_OK)
                            {
                                // failed return to queue?
                            }
                        }
                    } break;   

                    case USB_COMMAND_NEXT_PRESET:
                    {
                        if (TonexData.Message.SlotCPreset < (MAX_PRESETS - 1))
                        {
                            // always using Stomp mode C for preset setting
                            if (usb_tonex_one_set_preset_in_slot(TonexData.Message.SlotCPreset + 1, C, 1) != ESP_OK)
                            {
                                // failed return to queue?
                            }
                        }
                    } break;

                    case USB_COMMAND_PREVIOUS_PRESET:
                    {
                        if (TonexData.Message.SlotCPreset > 0)
                        {
                            // always using Stomp mode C for preset setting
                            if (usb_tonex_one_set_preset_in_slot(TonexData.Message.SlotCPreset - 1, C, 1) != ESP_OK)
                            {
                                // failed return to queue?
                            }
                        }
                    } break;
                }
            }
        } break;

        case COMMS_STATE_GET_STATE:
        {
            // waiting for state data
        } break;
    }

    // check if we have received anything (via RX interrupt)
    if (xQueueReceive(data_rx_queue, (void*)&rx_entry, 5) == pdPASS)
    {
        ESP_LOGI(TAG, "Got data via CDC");

        // check if we got a complete message
        if ((rx_entry.Length >= 2) && (rx_entry.Data[0] == 0x7E) && (rx_entry.Data[rx_entry.Length - 1] == 0x7E))
        {
            Status status = usb_tonex_one_parse(rx_entry.Data, rx_entry.Length);

            if (status != STATUS_OK)
            {
                ESP_LOGE(TAG, "Error parsing message: %d", (int)status);
            }
            else
            {
                // check what we got
                switch (TonexData.Message.Header.type)
                {
                    case TYPE_STATE_UPDATE:
                    {
                        uint16_t current_preset = usb_tonex_one_get_current_active_preset();

                        ESP_LOGI(TAG, "Received State Update. Current slot: %d. Preset: %d", (int)TonexData.Message.CurrentSlot, (int)current_preset);

                        // check for ToneOnePresetByteMarker[] to get preset name
                        temp_ptr = memmem((void*)rx_entry.Data, rx_entry.Length, (void*)ToneOnePresetByteMarker, sizeof(ToneOnePresetByteMarker));
                        if (temp_ptr != NULL)
                        {
                            ESP_LOGI(TAG, "Got preset name");

                            // grab name
                            memcpy((void*)preset_name, (void*)(temp_ptr + sizeof(ToneOnePresetByteMarker)), TONEX_ONE_RESP_OFFSET_PRESET_NAME_LEN);                
                        }

                        // make sure we are showing the correct preset as active                
                        control_sync_preset_details(current_preset, preset_name);

                        TonexData.TonexState = COMMS_STATE_READY;   

                        // note here: after boot, the state doesn't contain the preset name
                        // work around here is to request a change of preset A, but not to the currently active sloy.
                        // this results in pedal sending the full status details including the preset name
                        if (boot_init_needed)
                        {
                            uint8_t temp_preset = TonexData.Message.SlotAPreset;

                            if (temp_preset < (MAX_PRESETS - 1))
                            {
                                temp_preset++;
                            }
                            else
                            {
                                temp_preset--;
                            }
                            
                            usb_tonex_one_set_preset_in_slot(temp_preset, A, 0);

                            boot_init_needed = 0;
                        }
                    } break;

                    case TYPE_HELLO:
                    {
                        ESP_LOGI(TAG, "Received Hello");

                        // get current state
                        usb_tonex_one_request_state();
                        TonexData.TonexState = COMMS_STATE_GET_STATE;

                        // flag that we need to do the boot init procedure
                        boot_init_needed = 1;
                    } break;

                    default:
                    {
                        ESP_LOGI(TAG, "Message unknown %d", (int)TonexData.Message.Header.type);
                    } break;
                }
            }
        }
        else
        {
            ESP_LOGW(TAG, "Missing start or end bytes. Len:%d Start:%X End:%X", (int)rx_entry.Length, (int)rx_entry.Data[0], (int)rx_entry.Data[rx_entry.Length - 1]);
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
void usb_tonex_one_init(class_driver_t* driver_obj, QueueHandle_t comms_queue)
{
    // save the queue handle
    input_queue = comms_queue;

    memset((void*)&TonexData, 0, sizeof(TonexData));
    TonexData.TonexState = COMMS_STATE_IDLE;

    // create queue for data receive
    data_rx_queue = xQueueCreate(2, sizeof(tCDCRxQueueEntry));
    if (data_rx_queue == NULL)
    {
        ESP_LOGE(TAG, "Failed to create data_rx_queue queue!");
    }

    // code from ESP support forums, work around start. Refer to https://www.esp32.com/viewtopic.php?t=30601
    // Relates to this:
    // 
    // Endpoint Descriptor:
    // ------------------------------
    // 0x07	bLength
    // 0x05	bDescriptorType
    // 0x87	bEndpointAddress  (IN endpoint 7)
    // 0x02	bmAttributes      (Transfer: Bulk / Synch: None / Usage: Data)
    // 0x0040	wMaxPacketSize    (64 bytes)
    // 0x00	bInterval         
    // *** ERROR: Invalid wMaxPacketSize. Must be 512 bytes in high speed mode.

    //Endpoint Descriptor:
    //------------------------------
    // 0x07	bLength
    // 0x05	bDescriptorType
    // 0x07	bEndpointAddress  (OUT endpoint 7)
    // 0x02	bmAttributes      (Transfer: Bulk / Synch: None / Usage: Data)
    // 0x0200	wMaxPacketSize    (512 bytes)   <= invalid for full speed mode we are using here
    // 0x00	bInterval         
    const usb_config_desc_t* config_desc;
    ESP_ERROR_CHECK(usb_host_get_active_config_descriptor(driver_obj->dev_hdl, &config_desc));

    // fix wMaxPacketSize
    int off = 0;
    uint16_t wTotalLength = config_desc->wTotalLength;
    const usb_standard_desc_t *next_desc = (const usb_standard_desc_t *)config_desc;
    if (next_desc)
    {
        do
        {
            if (next_desc->bDescriptorType == USB_B_DESCRIPTOR_TYPE_ENDPOINT)
            {
                usb_ep_desc_t *mod_desc = (usb_ep_desc_t *)next_desc;
                if (mod_desc->wMaxPacketSize > 64)
                {
                    ESP_LOGW(TAG, "EP 0x%02X with wrong wMaxPacketSize %d - fixed to 64", mod_desc->bEndpointAddress, mod_desc->wMaxPacketSize);
                    mod_desc->wMaxPacketSize = 64;
                }
            }

            next_desc = usb_parse_next_descriptor(next_desc, wTotalLength, &off);
        } while (next_desc != NULL);
    }
    // code from forums, work around end

    // install CDC host driver
    ESP_ERROR_CHECK(cdc_acm_host_install(NULL));

    ESP_LOGI(TAG, "Opening CDC ACM device 0x%04X:0x%04X", IK_MULTIMEDIA_USB_VENDOR, TONEX_ONE_PRODUCT_ID);

    // set the config
    const cdc_acm_host_device_config_t dev_config = {
        .connection_timeout_ms = 1000,
        .out_buffer_size = 1024,
        .in_buffer_size = RX_TEMP_BUFFER_SIZE,
        .user_arg = NULL,
        .event_cb = NULL,
        .data_cb = usb_tonex_one_handle_rx
    };
    
    // open it
    ESP_ERROR_CHECK(cdc_acm_host_open(IK_MULTIMEDIA_USB_VENDOR, TONEX_ONE_PRODUCT_ID, TONEX_ONE_CDC_INTERFACE_INDEX, &dev_config, &cdc_dev));
    assert(cdc_dev);
    
    //cdc_acm_host_desc_print(cdc_dev);
    vTaskDelay(100);

    ESP_LOGI(TAG, "Setting up line coding");

    cdc_acm_line_coding_t line_coding;
    ESP_ERROR_CHECK(cdc_acm_host_line_coding_get(cdc_dev, &line_coding));
    ESP_LOGI(TAG, "Line Get: Rate: %d, Stop bits: %d, Parity: %d, Databits: %d", (int)line_coding.dwDTERate, (int)line_coding.bCharFormat, (int)line_coding.bParityType, (int)line_coding.bDataBits);

    // set line coding
    ESP_LOGI(TAG, "Set line coding");
    cdc_acm_line_coding_t new_line_coding = {
        .dwDTERate = 115200,
        .bCharFormat = 0,
        .bParityType = 0,
        .bDataBits = 8
    };

    if (cdc_acm_host_line_coding_set(cdc_dev, &new_line_coding) != ESP_OK)
    {
        ESP_LOGE(TAG, "Set line coding failed");
    }

    // disable flow control
    ESP_LOGI(TAG, "Set line state");
    if (cdc_acm_host_set_control_line_state(cdc_dev, true, true) != ESP_OK)
    {
        ESP_LOGE(TAG, "Set line state failed");
    }

    // let things finish init and settle
    vTaskDelay(250);

    // update UI
    control_set_usb_status(1);
}

/****************************************************************************
* NAME:        
* DESCRIPTION: 
* PARAMETERS:  
* RETURN:      
* NOTES:       
*****************************************************************************/
void usb_tonex_one_deinit(void)
{
    //to do here: need to clean up properly if pedal disconnected
    //cdc_acm_host_close();
}
