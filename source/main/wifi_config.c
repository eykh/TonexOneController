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
#include "esp_mac.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "lwip/err.h"
#include "lwip/sys.h"
#include "mdns.h"
#include <esp_http_server.h>
#include "control.h"
#include "wifi_config.h"
#include "task_priorities.h"

#define WIFI_CONFIG_TASK_STACK_SIZE   (3 * 1024)

#define ESP_WIFI_SSID      "TonexConfig"
#define ESP_WIFI_PASS      "12345678"
#define ESP_WIFI_CHANNEL   7
#define MAX_STA_CONN       2

static const char *TAG = "wifi_config";
static uint8_t client_connected = 0;
static httpd_handle_t http_server = NULL;
static httpd_config_t http_config = HTTPD_DEFAULT_CONFIG();

static esp_err_t index_get_handler(httpd_req_t *req);
static esp_err_t update_post_handler(httpd_req_t* req);
static esp_err_t get_handler(httpd_req_t *req);

static const httpd_uri_t index_get = 
{
	.uri	  = "/",
	.method   = HTTP_GET,
	.handler  = index_get_handler,
	.user_ctx = NULL
};

static const httpd_uri_t update_post = 
{
	.uri	  = "/config",
	.method   = HTTP_POST,
	.handler  = update_post_handler,
	.user_ctx = NULL
};

// web page for config
extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[] asm("_binary_index_html_end");

static esp_err_t stop_webserver(void);

/****************************************************************************
* NAME:        
* DESCRIPTION: 
* PARAMETERS:  
* RETURN:      none
* NOTES:       none
****************************************************************************/
static esp_err_t get_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "get_handler\n");

    // Send a simple response
    const char resp[] = "URI GET Response";

    httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

/****************************************************************************
* NAME:        
* DESCRIPTION: 
* PARAMETERS:  
* RETURN:      none
* NOTES:       none
****************************************************************************/
static esp_err_t index_get_handler(httpd_req_t *req)
{
	httpd_resp_send(req, (const char*)index_html_start, index_html_end - index_html_start);
	return ESP_OK;
}

/****************************************************************************
* NAME:        
* DESCRIPTION: 
* PARAMETERS:  
* RETURN:      none
* NOTES:       none
****************************************************************************/
static char from_hex(char ch) 
{
    return isdigit(ch) ? ch - '0' : tolower(ch) - 'a' + 10;
}

/****************************************************************************
* NAME:        
* DESCRIPTION: 
* PARAMETERS:  
* RETURN:      none
* NOTES:       none
****************************************************************************/
static char* url_decode(char *str) 
{
    char *pstr = str, *buf = malloc(strlen(str) + 1), *pbuf = buf;
  
    while (*pstr) 
    {
        if (*pstr == '%') 
        {
            if (pstr[1] && pstr[2]) 
            {
                *pbuf++ = from_hex(pstr[1]) << 4 | from_hex(pstr[2]);
                pstr += 2;
            }
        } 
        else if (*pstr == '+') 
        { 
            *pbuf++ = ' ';
        } 
        else 
        {
            *pbuf++ = *pstr;
        }
    
        pstr++;
    }
  
    *pbuf = '\0';
    
    return buf;
}

/****************************************************************************
* NAME:        
* DESCRIPTION: 
* PARAMETERS:  
* RETURN:      none
* NOTES:       none
****************************************************************************/
uint8_t get_submitted_value(char* dest, char* ptr)
{
    char tmp_str[128];
    char* tmp_ptr = tmp_str;
    char* decoded;
    
    // copy until end of value
    while ((*ptr != ' ') && (*ptr != 0) && (*ptr != '&'))
    {
        *tmp_ptr = *ptr;
        tmp_ptr++;
        ptr++;
    } 
    
    // ensure null terminated
    *tmp_ptr = 0;
    
    // decode the URL
    decoded = url_decode(tmp_str);
    
    strcpy(dest, decoded);
    free(decoded);
      
    return 1;
}

/****************************************************************************
* NAME:        
* DESCRIPTION: 
* PARAMETERS:  
* RETURN:      none
* NOTES:       none
****************************************************************************/
static esp_err_t update_post_handler(httpd_req_t* req)
{
    char value[20] = {0};
    char* ptr;
    uint8_t temp_val;

    //ESP_LOGI(TAG, "root_post_handler req->uri=[%s]", req->uri);
	//ESP_LOGI(TAG, "root_post_handler content length %d", req->content_len);
	char*  buf = malloc(req->content_len + 1);

	size_t off = 0;
	while (off < req->content_len) 
    {
		// Read data received in the request
		int ret = httpd_req_recv(req, buf + off, req->content_len - off);
		if (ret <= 0) 
        {
			if (ret == HTTPD_SOCK_ERR_TIMEOUT) 
            {
				httpd_resp_send_408(req);
			}
			free (buf);
            ESP_LOGE(TAG, "update_post_handler failed timeout");
			return ESP_FAIL;
		}
		off += ret;
		//ESP_LOGI(TAG, "root_post_handler recv length %d", ret);
	}
	buf[off] = '\0';
	ESP_LOGI(TAG, "root_post_handler buf=[%s]", buf);

    // check for POST elements                  
    // look for bt mode
    ptr = strstr(buf, "btmode=");    
    if (ptr != NULL)
    {
        // skip up to =
        ptr += strlen("btmode=");
        get_submitted_value(value, ptr);
 
        uint8_t index = BT_MODE_CENTRAL;
        if (strcmp(value, "disabled") == 0)
        {
            index = BT_MODE_DISABLED;
        }
        else if (strcmp(value, "client") == 0)
        {
            index = BT_MODE_CENTRAL;
        }
        else if (strcmp(value, "server") == 0)
        {
            index = BT_MODE_PERIPHERAL;
        }
        control_set_config_btmode(index);
    }

    // look for midienabled
    ptr = strstr(buf, "midienabled=");    
    temp_val = 0;
    if (ptr != NULL)
    {
        // skip up to =
        ptr += strlen("midienabled=");
        get_submitted_value(value, ptr);

        if (strcmp(value, "on") == 0)
        {
            temp_val = 1;
        }
    }
    control_set_config_serial_midi_enable(temp_val);

    // look for midichannel
    ptr = strstr(buf, "midichannel=");    
    if (ptr != NULL)
    {
        // skip up to =
        ptr += strlen("midichannel=");
        get_submitted_value(value, ptr);

        control_set_config_serial_midi_channel(atoi(value));        
    }
    
    // look for togglebypass
    ptr = strstr(buf, "togglebypass=");   
    temp_val = 0;
    if (ptr != NULL)
    {
        // skip up to =
        ptr += strlen("togglebypass=");
        get_submitted_value(value, ptr);

        if (strcmp(value, "on") == 0)
        {
            temp_val = 1;
        }
    }
    control_set_config_toggle_bypass(temp_val);

    // look for mvavechoc
    ptr = strstr(buf, "mvavechoc=");    
    temp_val = 0;
    if (ptr != NULL)
    {
        // skip up to =
        ptr += strlen("mvavechoc=");
        get_submitted_value(value, ptr);

        if (strcmp(value, "on") == 0)
        {
            temp_val = 1;
        }
    }   
    control_set_config_mv_choc_enable(temp_val);

    // look for xvivemd1
    ptr = strstr(buf, "xvivemd1=");    
    temp_val = 0;
    if (ptr != NULL)
    {
        // skip up to =
        ptr += strlen("xvivemd1=");
        get_submitted_value(value, ptr);

        if (strcmp(value, "on") == 0)
        {
            temp_val = 1;
        }
    }
    control_set_config_xv_md1_enable(temp_val);
    free(buf);

    // Send a simple response
    const char resp[] = "<span style=\"font-size: 7vw;\">Config save complete. Rebooting...</span>\n";
    httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);

    vTaskDelay(pdMS_TO_TICKS(250));
    stop_webserver();
    vTaskDelay(pdMS_TO_TICKS(250));

    // save it and reboot after
    control_save_user_data(1);

	return ESP_OK;
}

/****************************************************************************
* NAME:        
* DESCRIPTION: 
* PARAMETERS:  
* RETURN:      none
* NOTES:       none
****************************************************************************/
static esp_err_t stop_webserver(void)
{
    // Stop the httpd server
    if (http_server != NULL)
    {
        httpd_stop(http_server);
    }

    return ESP_OK;
}

/****************************************************************************
* NAME:        
* DESCRIPTION: 
* PARAMETERS:  
* RETURN:      none
* NOTES:       none
****************************************************************************/
static esp_err_t http_server_init(void)
{
    ESP_LOGI(TAG, "Http server init");
    
    // set config
    http_config.task_priority      = tskIDLE_PRIORITY + 1;
    http_config.core_id            = 0;
    http_config.stack_size         = (3 * 1024);  
    http_config.server_port        = 80;
    http_config.ctrl_port          = 32768;
    http_config.max_open_sockets   = 2;
    http_config.max_uri_handlers   = 2;
    http_config.max_resp_headers   = 3;
    http_config.backlog_conn       = 1;
    http_config.keep_alive_enable  = true;
    http_config.keep_alive_idle    = 10;     // seconds
    http_config.keep_alive_interval = 10;    // seconds
    http_config.keep_alive_count = 5;
    http_config.open_fn = NULL;
    http_config.close_fn = NULL;
    http_config.uri_match_fn = NULL;
    http_config.lru_purge_enable = true;

	if (httpd_start(&http_server, &http_config) == ESP_OK) 
    {
        if (http_server != NULL)
        {
            ESP_LOGI(TAG, "Http register uri 1");
    	    httpd_register_uri_handler(http_server, &index_get);

            ESP_LOGI(TAG, "Http register uri 2");
		    httpd_register_uri_handler(http_server, &update_post);
        }
	}

    if (http_server == NULL)
    {
        ESP_LOGE(TAG, "Http server init failed!");
        return ESP_FAIL;
    }
    else
    {
        ESP_LOGI(TAG, "Http server init OK");
        return ESP_OK;
    }
}

/****************************************************************************
* NAME:        
* DESCRIPTION: 
* PARAMETERS:  
* RETURN:      
* NOTES:       
*****************************************************************************/
static void wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data)
{
    if (event_id == WIFI_EVENT_AP_STACONNECTED) 
    {
        wifi_event_ap_staconnected_t* event = (wifi_event_ap_staconnected_t*) event_data;
        ESP_LOGI(TAG, "station "MACSTR" join, AID=%d", MAC2STR(event->mac), event->aid);
        client_connected = 1;
    } 
    else if (event_id == WIFI_EVENT_AP_STADISCONNECTED) 
    {
        wifi_event_ap_stadisconnected_t* event = (wifi_event_ap_stadisconnected_t*) event_data;
        ESP_LOGI(TAG, "station "MACSTR" leave, AID=%d", MAC2STR(event->mac), event->aid);
        client_connected = 0;
    }
}

/****************************************************************************
* NAME:        
* DESCRIPTION: 
* PARAMETERS:  
* RETURN:      
* NOTES:       
*****************************************************************************/
static void wifi_init_softap(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        NULL));

    wifi_config_t wifi_config = {
        .ap = {
            .ssid = ESP_WIFI_SSID,
            .ssid_len = strlen(ESP_WIFI_SSID),
            .channel = ESP_WIFI_CHANNEL,
            .password = ESP_WIFI_PASS,
            .max_connection = MAX_STA_CONN,
            .authmode = WIFI_AUTH_WPA_WPA2_PSK,
            .pmf_cfg = {
                    .required = false,
            },
        },
    };
    
    if (strlen(ESP_WIFI_PASS) == 0) 
    {
        wifi_config.ap.authmode = WIFI_AUTH_OPEN;
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "wifi_init_softap finished. SSID:%s password:%s channel:%d", ESP_WIFI_SSID, ESP_WIFI_PASS, ESP_WIFI_CHANNEL);
}

/****************************************************************************
* NAME:        
* DESCRIPTION: 
* PARAMETERS:  
* RETURN:      
* NOTES:       
*****************************************************************************/
void start_mdns_service()
{
    //initialize mDNS service
    esp_err_t err = mdns_init();
    if (err) 
    {
        ESP_LOGE(TAG, "MDNS Init failed: %d\n", err);
        return;
    }

    mdns_hostname_set("tonex");
    mdns_instance_name_set("tonex");
    mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
}

/****************************************************************************
* NAME:        
* DESCRIPTION: 
* PARAMETERS:  
* RETURN:      
* NOTES:       
*****************************************************************************/
static void wifi_config_task(void *arg)
{
    ESP_LOGI(TAG, "Wifi config task start");

    // let everything settle and init
    vTaskDelay(pdMS_TO_TICKS(1000));

    ESP_LOGI(TAG, "Wifi config starting");

    // start up WiFi access point
    wifi_init_softap();
    start_mdns_service();

    // start web server
    http_server_init();

    // allow WiFi to run for 60 seconds
    vTaskDelay(pdMS_TO_TICKS(60000));

    // if no clients, kill Wifi    
    if (client_connected == 0)
    {
        // kill
        ESP_LOGI(TAG, "Wifi config stopping");
        stop_webserver();
        vTaskDelay(pdMS_TO_TICKS(5000));
        esp_wifi_stop();
        ESP_LOGI(TAG, "Wifi config stopped");
    }

    // do nothing
    while (1) 
    {
		vTaskDelay(pdMS_TO_TICKS(10000));
	}
}

/****************************************************************************
* NAME:        
* DESCRIPTION: 
* PARAMETERS:  
* RETURN:      
* NOTES:       
*****************************************************************************/
void wifi_config_init(void)
{
    xTaskCreatePinnedToCore(wifi_config_task, "WIFI", WIFI_CONFIG_TASK_STACK_SIZE, NULL, WIFI_TASK_PRIORITY, NULL, 0);
}
