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
#include <sys/unistd.h>
#include <sys/stat.h>
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
#include "esp_intr_alloc.h"
#include "usb/usb_host.h"
#include "esp_private/periph_ctrl.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"

#include "main.h"
#include "task_priorities.h"
#include "usb_comms.h"
#include "display.h"
#include "footswitches.h"
#include "control.h"
#include "midi_control.h"
#include "CH422G.h"
#include "midi_serial.h"
#include "wifi_config.h"

#define I2C_MASTER_SCL_IO               9       /*!< GPIO number used for I2C master clock */
#define I2C_MASTER_SDA_IO               8       /*!< GPIO number used for I2C master data  */
#define I2C_MASTER_NUM                  0       /*!< I2C master i2c port number, the number of i2c peripheral interfaces available will depend on the chip */
#define I2C_MASTER_FREQ_HZ              400000                     /*!< I2C master clock frequency */
#define I2C_MASTER_TX_BUF_DISABLE       0                          /*!< I2C master doesn't need buffer */
#define I2C_MASTER_RX_BUF_DISABLE       0                          /*!< I2C master doesn't need buffer */

#define I2C_CLR_BUS_SCL_NUM            (9)
#define I2C_CLR_BUS_HALF_PERIOD_US     (5)

#define MOUNT_POINT "/sdcard"

// Pin assignments for SD Card
#define PIN_NUM_MISO        13
#define PIN_NUM_MOSI        11
#define PIN_NUM_CLK         12
#define PIN_NUM_CS          -1


static const char *TAG = "app_main";


#if CONFIG_TONEX_CONTROLLER_DISPLAY_WAVESHARE_800_480

SemaphoreHandle_t I2CMutex;
static esp_err_t i2c_master_init(void);

/****************************************************************************
* NAME:        
* DESCRIPTION: 
* PARAMETERS:  
* RETURN:      
* NOTES:       
*****************************************************************************/
esp_err_t i2c_master_reset(void)
{
    int sda_io = I2C_MASTER_SDA_IO;
    int scl_io = I2C_MASTER_SCL_IO;
    const int scl_half_period = I2C_CLR_BUS_HALF_PERIOD_US; // use standard 100kHz data rate
    int i = 0;

    ESP_LOGI(TAG, "I2C bus reset");

    // nuke it
    i2c_reset_tx_fifo(I2C_MASTER_NUM);
    i2c_reset_rx_fifo(I2C_MASTER_NUM);
    periph_module_disable(PERIPH_I2C0_MODULE);
    periph_module_enable(PERIPH_I2C0_MODULE);
    i2c_driver_delete(I2C_MASTER_NUM);

    // manually clock the bus if SDA is stuck
    gpio_set_direction(scl_io, GPIO_MODE_OUTPUT_OD);
    gpio_set_direction(sda_io, GPIO_MODE_INPUT_OUTPUT_OD);

    // If a SLAVE device was in a read operation when the bus was interrupted, the SLAVE device is controlling SDA.
    // The only bit during the 9 clock cycles of a READ byte the MASTER(ESP32) is guaranteed control over is during the ACK bit
    // period. If the slave is sending a stream of ZERO bytes, it will only release SDA during the ACK bit period.
    // So, this reset code needs to synchronize the bit stream with, Either, the ACK bit, Or a 1 bit to correctly generate
    // a STOP condition.
    gpio_set_level(scl_io, 0);
    gpio_set_level(sda_io, 1);
    esp_rom_delay_us(scl_half_period);

    if (!gpio_get_level(sda_io))
    {
        ESP_LOGI(TAG, "I2C bus clearing stuck SDA");
    }

    while (!gpio_get_level(sda_io) && (i++ < I2C_CLR_BUS_SCL_NUM)) 
    {
        gpio_set_level(scl_io, 1);
        esp_rom_delay_us(scl_half_period);
        gpio_set_level(scl_io, 0);
        esp_rom_delay_us(scl_half_period);
    }
    
    gpio_set_level(sda_io, 0); // setup for STOP
    gpio_set_level(scl_io, 1);
    esp_rom_delay_us(scl_half_period);
    gpio_set_level(sda_io, 1); // STOP, SDA low -> high while SCL is HIGH

    // init again
    return i2c_master_init();
}

/****************************************************************************
* NAME:        
* DESCRIPTION: 
* PARAMETERS:  
* RETURN:      
* NOTES:       
*****************************************************************************/
static esp_err_t i2c_master_init(void)
{
    esp_err_t res;

    i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = I2C_MASTER_SDA_IO,
        .scl_io_num = I2C_MASTER_SCL_IO,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = I2C_MASTER_FREQ_HZ,
    };

    i2c_param_config(I2C_MASTER_NUM, &conf);

    res = i2c_driver_install(I2C_MASTER_NUM, conf.mode, I2C_MASTER_RX_BUF_DISABLE, I2C_MASTER_TX_BUF_DISABLE, 0);
    return res;
}

/****************************************************************************
* NAME:        
* DESCRIPTION: 
* PARAMETERS:  
* RETURN:      
* NOTES:       
*****************************************************************************/
static void InitIOExpander(i2c_port_t I2CNum, SemaphoreHandle_t I2CMutex)
{
    // init IO expander
    if (CH422G_init(I2CNum, I2CMutex) == ESP_OK)
    {
        // set IO expander to output mode. Can't do mixed pins
        // For inputs, we will temporarily flip the mode
        CH422G_set_io_mode(1);

        ESP_LOGI(TAG, "IO Expander init OK");
    }
    else
    {
        ESP_LOGE(TAG, "Failed to init IO expander!");
    }
}
#endif  //CONFIG_TONEX_CONTROLLER_DISPLAY_WAVESHARE_800_480

/****************************************************************************
* NAME:        
* DESCRIPTION: 
* PARAMETERS:  
* RETURN:      
* NOTES:       
*****************************************************************************/
#if 0
static void InitSDCard(void)
{
    esp_err_t ret;

    // Set CS pin low
    CH422G_write_direction(SD_CS, IO_EXPANDER_OUTPUT);
    CH422G_write_output(SD_CS, 0);

    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 5,
        .allocation_unit_size = 16 * 1024
    };

    sdmmc_card_t *card;
    const char mount_point[] = MOUNT_POINT;
    ESP_LOGI(TAG, "Initializing SD card");
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();

    spi_bus_config_t bus_cfg = {
        .mosi_io_num = PIN_NUM_MOSI,
        .miso_io_num = PIN_NUM_MISO,
        .sclk_io_num = PIN_NUM_CLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4000,
    };
    
    ret = spi_bus_initialize(host.slot, &bus_cfg, SDSPI_DEFAULT_DMA);
    if (ret != ESP_OK) 
    {
        printf("Failed to initialize SPI bus.\r\n");
        return;
    }
    
    // This initializes the slot without card detect (CD) and write protect (WP) signals.
    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.gpio_cs = PIN_NUM_CS;
    slot_config.host_id = host.slot;

    ret = esp_vfs_fat_sdspi_mount(mount_point, &host, &slot_config, &mount_config, &card);

    if (ret != ESP_OK) 
    {      
        ESP_LOGI(TAG, "Failed to init SD card %s", esp_err_to_name(ret));
        return;
    }
    
    ESP_LOGI(TAG, "SD Card mounted");

    // Card has been initialized, print its properties
    sdmmc_card_print_info(stdout, card);

    // copy amp skin images from SD card to PSRAM. This is done mainly because the Waveshare
    // boards use an I2C IO expander to drive the SD card chip select, which is not suppported
    // by the ESP drivers. Would have to have some way of making LGVL call a custom function
    // to clear/set CS via I2C before/after all transactions, which doesn't seem to be supported.
    //
    // By copying fromm SD to PSAM, we free up program memory (compared to internal storage) and
    // also allow the user to add/changes skins without needing to compile. Plus PSRAM is faster
    // to access than SD at run time

    ESP_LOGI(TAG, "PSRAM free space before skin load %d", (int)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    struct dirent* dp;
    struct stat st;
    DIR* dir = opendir(MOUNT_POINT);

    if (dir == NULL) 
    {
        ESP_LOGE(TAG, "Can't Open Dir.");
    }
    else
    {
        while ((dp = readdir(dir)) != NULL) 
        {
            ESP_LOGI(TAG, "%s", dp->d_name);

            // check if file is a PNG image
            if (strstr(strupr(dp->d_name), ".PNG") != NULL)
            {
                // get status of file            
                sprintf(full_path, "%s/%s", MOUNT_POINT, dp->d_name);

                if (stat(full_path, &st) == 0) 
                {
                    ESP_LOGI(TAG, "Found SD card png file %s. Size: %d", full_path, (int)st.st_size);

                    if (st.st_size > 0)
                    {
                        // allocate buffer in PSRAM to hold it
                        void* img_ptr = heap_caps_malloc(st.st_size, MALLOC_CAP_SPIRAM);
                        if (img_ptr == NULL)
                        {
                            ESP_LOGE(TAG, "Unable to malloc space for amp skin %s", full_path);
                            break;
                        }
                        else
                        {
                            // add to struct for future usage    
                            //dp->d_name, img_ptr
                        }
                    }
                }
                else
                {
                    ESP_LOGE(TAG, "stat not zero %s", full_path);
                }
            }
        }

        closedir (dir);
    }

    // deselect CS
    CH422G_write_output(SD_CS, 1);

    ESP_LOGI(TAG, "PSRAM free space after skin load %d", (int)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    // unmount partition
    esp_vfs_fat_sdcard_unmount(mount_point, card);
    spi_bus_free(host.slot);
}
#endif

/****************************************************************************
* NAME:        
* DESCRIPTION: 
* PARAMETERS:  
* RETURN:      
* NOTES:       
*****************************************************************************/
void app_main(void)
{
    ESP_LOGI(TAG, "ToneX One Controller App start");

    // load the config first
    control_load_config();

#if CONFIG_TONEX_CONTROLLER_DISPLAY_WAVESHARE_800_480
    // create mutex for shared I2C bus
    I2CMutex = xSemaphoreCreateMutex();
    if (I2CMutex == NULL)
    {
        ESP_LOGE(TAG, "I2C Mutex create failed!");
    }

    // init I2C master
    ESP_ERROR_CHECK(i2c_master_init());
    ESP_LOGI(TAG, "I2C initialized successfully");

    // init IO expander
    ESP_LOGI(TAG, "Init IO Expander");
    InitIOExpander(I2C_MASTER_NUM, I2CMutex);
    
    // Init SD card
    // Note here: this was intended to be used to load Amp skin images
    // from SD card, but 2 MB PSRAM not enough to load to ram, and
    // SD card using IO expander for chip select makes direct LVGL load
    // from SD really tricky.
    //InitSDCard();
#else    
    ESP_LOGI(TAG, "Display disabled");
#endif

    // init control task
    ESP_LOGI(TAG, "Init Control");
    control_init();

#if CONFIG_TONEX_CONTROLLER_DISPLAY_WAVESHARE_800_480
    // init GUI
    ESP_LOGI(TAG, "Init display");
    display_init(I2C_MASTER_NUM, I2CMutex);
#endif

    // init Footswitches
    ESP_LOGI(TAG, "Init footswitches");
    footswitches_init();

    if (control_get_config_bt_mode() != BT_MODE_DISABLED)
    {
        // init Midi Bluetooth
        ESP_LOGI(TAG, "Init MIDI BT");
        midi_init();
    }
    else
    {
        ESP_LOGI(TAG, "MIDI BT disabled");
    }

    if (control_get_config_midi_serial_enable())
    {
        // init Midi serial
        ESP_LOGI(TAG, "Init MIDI Serial");
        midi_serial_init();
    }
    else
    {    
        ESP_LOGI(TAG, "Serial MIDI disabled");
    }

    // init USB
    ESP_LOGI(TAG, "Init USB");
    init_usb_comms();

    // init WiFi config
    wifi_config_init();
}
