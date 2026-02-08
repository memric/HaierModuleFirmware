/*
 * telegram_bot.c
 *
 *  Created on: 26.08.2022
 *      Author: chudnikov
 */

#include "telegram_bot.h"
#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "esp_tls.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifndef USE_TLS_BUNDLE
#define USE_TLS_BUNDLE 0
#endif

#define WEB_SERVER       "api.telegram.org"
#define WEB_URL          "https://api.telegram.org/bot"
#if CONFIG_TG_EN
#define BOT_TOKEN        CONFIG_TG_BOT_TOKEN
#else
#define BOT_TOKEN       "TEST_TOKEN"
#endif /* CONFIG_TG_EN */

#define UPDATE_TIMEOUT   5

#define HTTP_METHOD_GET  "GET"
#define HTTP_METHOD_POST "POST"

#define ON_BTN_STR       "ON"
#define OFF_BTN_STR      "OFF"
#define STATE_BTN_STR    "STATE"

#ifndef CONFIG_TG_CHAT_ID
#define CONFIG_TG_CHAT_ID 1
#endif

#define STR_APPEND(msg...) resp_len += snprintf(&resp_msg[resp_len], sizeof(resp_msg) - resp_len, msg)

static const char *TAG           = "TELEGRAM";
static TaskHandle_t telebot_task = NULL;

#if !USE_TLS_BUNDLE
extern const uint8_t telegram_cert_pem_start[] asm("_binary_telegram_cert_pem_start");
extern const uint8_t telegram_cert_pem_end[] asm("_binary_telegram_cert_pem_end");
#endif
char url[256];
char buf[512];
char resp[512]           = {0};

static cJSON *mainMarkup = NULL;

static int32_t TeleBot_Http_Request(const char *http_mthd, const char *t_mthd, char *req, uint32_t req_len, char *resp,
                                    uint32_t resp_len);
static void TeleBot_Task(void *arg);
int32_t TeleBot_SendMessage(uint32_t chat_id, const char *msg, cJSON *markup);
void TeleBot_MessageCallback(uint32_t chat_id, const char *msg);

/**
 * @brief	Bot initialization
 *
 */
void TeleBot_Init(void)
{
    xTaskCreate(TeleBot_Task, "Telegram Bot", 1024 * 6, NULL, 3, &telebot_task);
}

/**
 * @brief 	Connects to Telegram bot server, sends http request and
 * 			gets response
 *
 * @param http_mthd	HTTP method GET or POST
 * @param t_mthd	Telegram method
 * @param req		Request string (JSON)
 * @param req_len	Request length
 * @param resp		Pointer to buffer for response from server
 * @param resp_len	Response lengthp
 * @return
 */
static int32_t TeleBot_Http_Request(const char *http_mthd, const char *t_mthd, char *req, uint32_t req_len, char *resp,
                                    uint32_t resp_len)
{
    int32_t ret, len;
    int32_t http_req_len = 0;
    int32_t ret_len      = 0;

#if USE_TLS_BUNDLE
    ESP_LOGI(TAG, "https_request using crt bundle");
    esp_tls_cfg_t cfg = {
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
#else
    ESP_LOGI(TAG, "https_request using cacert_buf");
    esp_tls_cfg_t cfg = {
        .cacert_buf   = (const unsigned char *) telegram_cert_pem_start,
        .cacert_bytes = telegram_cert_pem_end - telegram_cert_pem_start,
    };
#endif

    /*Compose url*/
    len = snprintf(url, sizeof(url), "" WEB_URL "%s/%s", BOT_TOKEN, t_mthd);

    if (len < 0)
        return -1;

    ESP_LOGD(TAG, "HTTPS url: %s", url);

    /*Set connection*/
    esp_tls_t *tls = esp_tls_init();
    if (!tls)
    {
        ESP_LOGE(TAG, "Failed to allocate esp_tls handle!");
        return -1;
    }

    if (esp_tls_conn_http_new_sync(url, &cfg, tls) == 1)
    {
        ESP_LOGI(TAG, "Connection established...");

        /*Compose request header*/
        len = snprintf(buf, sizeof(buf),
                       "%s /bot%s/%s HTTP/1.1\r\n"
                       "Host: " WEB_SERVER "\r\n"
                       /*"User-Agent: esp-idf/1.0 esp32\r\n"*/
                       "Connection: close\r\n",
                       http_mthd, BOT_TOKEN, t_mthd);

        if (len > 0)
        {
            http_req_len += len;

            if ((req != NULL) && (req_len < (sizeof(buf) - http_req_len)))
            {
                /*Append request string*/
                len = snprintf(&buf[http_req_len], sizeof(buf) - len,
                               "Content-Type: application/json\r\n"
                               "Content-Length: %" PRIu32 "\r\n\r\n",
                               req_len);

                if (len > 0)
                {
                    http_req_len += len;

                    len = snprintf(&buf[http_req_len], sizeof(buf) - http_req_len, "%s", req);

                    if (len > 0)
                    {
                        http_req_len += len;
                    }
                    else
                    {
                        http_req_len = 0;
                    }
                }
                else
                {
                    http_req_len = 0;
                }
            }
            else
            {
                /*Append \r\n for GET request*/
                len = snprintf(&buf[http_req_len], sizeof(buf) - http_req_len, "\r\n");

                if (len > 0)
                {
                    http_req_len += len;
                }
                else
                {
                    http_req_len = 0;
                }
            }

            ESP_LOGI(TAG, "HTTP Request: %s", buf);
        }

        if (http_req_len == 0)
        {
            ESP_LOGE(TAG, "Request composing error");
        }
        else
        {
            size_t written_bytes = 0;

            /*write request*/
            do
            {
                ret = esp_tls_conn_write(tls, buf + written_bytes, http_req_len - written_bytes);

                if (ret >= 0)
                {
                    ESP_LOGI(TAG, "%" PRIi32 " bytes written", ret);
                    written_bytes += ret;
                }
                else if (ret != ESP_TLS_ERR_SSL_WANT_READ && ret != ESP_TLS_ERR_SSL_WANT_WRITE)
                {
                    ESP_LOGE(TAG, "esp_tls_conn_write returned: [0x%02" PRIx32 "](%s)", ret, esp_err_to_name(ret));
                    break;
                }
            } while (written_bytes < http_req_len);

            /*Read response*/
            if (written_bytes == http_req_len)
            {
                ESP_LOGI(TAG, "Reading HTTP response...");

                do
                {
                    len = sizeof(buf);
                    bzero(buf, sizeof(buf));
                    ret = esp_tls_conn_read(tls, (char *) buf, len);

                    if (ret == ESP_TLS_ERR_SSL_WANT_WRITE || ret == ESP_TLS_ERR_SSL_WANT_READ)
                    {
                        continue;
                    }

                    if (ret < 0)
                    {
                        ESP_LOGE(TAG, "esp_tls_conn_read returned [-0x%02" PRIx32 "](%s)", -ret, esp_err_to_name(ret));
                        break;
                    }

                    if (ret == 0)
                    {
                        ESP_LOGI(TAG, "connection closed");
                        break;
                    }

                    len = ret;
                    ESP_LOGD(TAG, "%" PRIi32 " bytes read", len);
                    /* Print response directly to stdout as it is read */
                    for (int i = 0; i < len; i++)
                    {
                        putchar(buf[i]);
                    }
                    putchar('\n'); // JSON output doesn't have a newline at end

                    /*JSON beginning searching*/
                    char *pch = strstr(buf, "\r\n{");
                    if (pch != NULL)
                    {
                        ret_len = 0;
                        len -= (pch - buf);
                    }
                    else
                    {
                        pch = buf;
                    }
                    /*Copy response*/
                    while ((len > 0) && (ret_len < resp_len))
                    {
                        *resp++ = *pch++;
                        ret_len++;
                        len--;
                    }

                } while (ret != 0);
            }
        }
    }
    else
    {
        ESP_LOGE(TAG, "Connection failed...");
    }

    /*Delete connection*/
    esp_tls_conn_destroy(tls);

    return ret_len;
}

/**
 * getUpdates method
 */
static int32_t TeleBot_GetUpdates(int32_t *id)
{
    int32_t retval = -1;

    /*Construct JSON method object*/
    cJSON *message = cJSON_CreateObject();

    if (message != NULL)
    {
        cJSON *msg_timeout = cJSON_CreateNumber(UPDATE_TIMEOUT);
        cJSON *msg_offset  = cJSON_CreateNumber(*id);

        if (msg_timeout != NULL && msg_offset != NULL)
        {
            cJSON_AddItemToObject(message, "timeout", msg_timeout);
            cJSON_AddItemToObject(message, "offset", msg_offset);

            char *req = cJSON_PrintUnformatted(message);

            retval    = TeleBot_Http_Request("POST", "getUpdates", req, strlen(req), resp, sizeof(resp));

            if (retval > 0)
            {
                ESP_LOGD(TAG, "Resp: %s", resp);

                cJSON *json = cJSON_ParseWithLength(resp, sizeof(resp));

                if (json != NULL)
                {
                    cJSON *result = cJSON_GetObjectItemCaseSensitive(json, "result");
                    cJSON *res_item;
                    cJSON_ArrayForEach(res_item, result)
                    {
                        /*Get update id*/
                        cJSON *upd_id = cJSON_GetObjectItemCaseSensitive(res_item, "update_id");
                        if (upd_id != NULL && upd_id->valueint >= *id)
                        {
                            /*Recalculate offset*/
                            *id = upd_id->valueint + 1;
                        }

                        cJSON *message = cJSON_GetObjectItemCaseSensitive(res_item, "message");
                        cJSON *text    = cJSON_GetObjectItemCaseSensitive(message, "text");

                        if (text != NULL)
                        {
                            cJSON *chat    = cJSON_GetObjectItemCaseSensitive(message, "chat");
                            cJSON *chat_id = cJSON_GetObjectItemCaseSensitive(chat, "id");

                            TeleBot_MessageCallback((uint32_t) chat_id->valueint, text->valuestring);
                        }
                    }

                    cJSON_Delete(json);
                }
            }

            free(req);
        }

        cJSON_Delete(message);
    }

    return retval;
}

/**
 * Sends message
 */
int32_t TeleBot_SendMessage(uint32_t chat_id, const char *msg, cJSON *markup)
{
    int32_t retval = -1;

    /*Construct JSON method object*/
    cJSON *message = cJSON_CreateObjectReference(NULL);

    if (message != NULL)
    {
        cJSON *msg_chat_id = cJSON_CreateNumber(chat_id);
        cJSON *msg_text    = cJSON_CreateString(msg);

        if (msg_chat_id != NULL && msg_text != NULL)
        {
            cJSON_AddItemToObject(message, "chat_id", msg_chat_id);
            cJSON_AddItemToObject(message, "text", msg_text);

            if (markup != NULL)
            {
                cJSON_AddItemToObject(message, "reply_markup", markup);
            }

            char *req = cJSON_PrintUnformatted(message);

            ESP_LOGD(TAG, "sendMessage: %s", req);

            retval = TeleBot_Http_Request("POST", "sendMessage", req, strlen(req), resp, sizeof(resp));

            free(req);
        }

        cJSON_Delete(message);
    }

    return retval;
}

/**
 * Main task
 */
static void TeleBot_Task(void *arg)
{
    (void) arg;
    int32_t id = -1;
    int32_t resp_len;
    haier_state_t haier_state;
    char resp_msg[128];

    /*Create markup*/
    mainMarkup = cJSON_CreateObject();

    if (mainMarkup != NULL)
    {
        cJSON *btn1 = cJSON_CreateString(ON_BTN_STR);
        cJSON *btn2 = cJSON_CreateString(OFF_BTN_STR);
        cJSON *btn3 = cJSON_CreateString(STATE_BTN_STR);

        cJSON *btns = cJSON_CreateArray();
        cJSON *row1 = cJSON_CreateArray();
        cJSON *row2 = cJSON_CreateArray();

        if (btn2 != NULL && btn1 != NULL && btn3 != NULL && btns != NULL && row1 != NULL && row2 != NULL)
        {
            cJSON_AddItemToArray(row1, btn1);
            cJSON_AddItemToArray(row1, btn2);
            cJSON_AddItemToArray(row2, btn3);
            cJSON_AddItemToArray(btns, row1);
            cJSON_AddItemToArray(btns, row2);

            cJSON_AddItemToObject(mainMarkup, "keyboard", btns);
        }

        cJSON_AddBoolToObject(mainMarkup, "resize_keyboard", 1);
    }

    TeleBot_SendMessage(CONFIG_TG_CHAT_ID, "Bot started", mainMarkup);

    while (1)
    {
        /* Get updates */
        resp_len = TeleBot_GetUpdates(&id);

        if (Haier_GetState(&haier_state, 10 / portTICK_PERIOD_MS) == 0 && resp_len > 0 && id != -1)
        {
            resp_len = 0;

            STR_APPEND("🔌: ");
            if (haier_state.en)
            {
                STR_APPEND("ON");
            }
            else
            {
                STR_APPEND("OFF");
            }
            STR_APPEND("\n🌡️: %i°C; Set: %i°C", haier_state.temp_curr, haier_state.temp_set);
            STR_APPEND("\n⚙️: ");
            switch (haier_state.mode)
            {
            case 0:
                STR_APPEND("Smart");
                break;
            case 1:
                STR_APPEND("Cool");
                break;
            case 2:
                STR_APPEND("Heat");
                break;
            case 3:
                STR_APPEND("Vent");
                break;
            case 4:
                STR_APPEND("Dry");
                break;
            default:
                STR_APPEND("Unknown: %i", haier_state.mode);
            }
            STR_APPEND("\n💨: ");
            switch (haier_state.fan_speed)
            {
            case 0:
                STR_APPEND("Max");
                break;
            case 1:
                STR_APPEND("Mid");
                break;
            case 2:
                STR_APPEND("Min");
                break;
            case 3:
                STR_APPEND("Auto");
                break;
            default:
                STR_APPEND("Unknown: %i", haier_state.fan_speed);
            }

            if (resp_len > 0)
            {
                resp_msg[resp_len] = 0;
                TeleBot_SendMessage(CONFIG_TG_CHAT_ID, resp_msg, mainMarkup);
            }
        }

        vTaskDelay(1000 / portTICK_PERIOD_MS);
    }
}

/**
 * Message callback
 */
void TeleBot_MessageCallback(uint32_t chat_id, const char *msg)
{
    if (chat_id != CONFIG_TG_CHAT_ID)
        return;

    ESP_LOGI(TAG, "Message from %" PRIu32 ": %s", chat_id, msg);

    if (memcmp(msg, ON_BTN_STR, strlen(ON_BTN_STR)) == 0)
    {
        Haier_Cmd(HAIER_ON);
        TeleBot_SendMessage(chat_id, "ON command received", mainMarkup);
    }
    else if (memcmp(msg, OFF_BTN_STR, strlen(OFF_BTN_STR)) == 0)
    {
        Haier_Cmd(HAIER_OFF);
        TeleBot_SendMessage(chat_id, "OFF command received", mainMarkup);
    }
    else if (memcmp(msg, STATE_BTN_STR, strlen(STATE_BTN_STR)) == 0)
    {
        Haier_Cmd(HAIER_STATE);
        TeleBot_SendMessage(chat_id, "State command received", mainMarkup);
    }
    else
    {
        TeleBot_SendMessage(chat_id, "Unresolved command: ", mainMarkup);
        TeleBot_SendMessage(chat_id, msg, mainMarkup);
    }
}
