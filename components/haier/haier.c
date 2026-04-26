#include "haier.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include <string.h>

#ifndef CONFIG_HAIER_UART_PORT
#define CONFIG_HAIER_UART_PORT UART_NUM_1
#endif

#ifndef CONFIG_HAIER_BUF_LEN
#define CONFIG_HAIER_BUF_LEN    128
#endif

/*
I (11358) HAIER: On cmd
I (11578) HAIER: Read 37 bytes: '��"'
I (11578) HAIER: 0x3fc95560   ff ff 22 00 00 00 00 00  01 02 6d 01 00 1a 00 00  |..".......m.....|
I (11578) HAIER: 0x3fc95570   00 7f 00 00 00 00 00 01  00 03 00 00 00 01 00 00  |................|
I (11598) HAIER: 0x3fc95580   00 37 00 09 71                                    |.7..q|

I (28758) HAIER: Off cmd
I (28978) HAIER: Read 37 bytes: '��"'
I (28978) HAIER: 0x3fc95560   ff ff 22 00 00 00 00 00  01 02 6d 01 00 1a 00 00  |..".......m.....|
I (28978) HAIER: 0x3fc95570   00 7f 00 00 00 00 00 01  00 03 00 00 00 00 00 00  |................|
I (28998) HAIER: 0x3fc95580   00 37 00 09 70                                    |.7..p|
*/

/**
 * Response structure
 *
 */

#ifndef CONFIG_HAIER_UART_BUFF_SIZE
#define CONFIG_HAIER_UART_BUFF_SIZE 256
#endif

#define UART_EVENT_QUEUE_SIZE               20

#define ERR_OK                              0
#define ERR_DRV                             1
#define ERR_SYS                             2

#define HAIER_LEN_OFFSET                    2U
#define HAIER_FLAGS_OFFSET                  3U
#define HAIER_DIR1_OFFSET                   9U
#define HAIER_DIR2_OFFSET                   10U
#define HAIER_CMD_OFFSET                    11U
#define HAIER_ROOM_TEMP_OFFSET              13U
#define HAIER_MODE_OFFSET                   23U
#define HAIER_FAN_OFFSET                    25U
#define HAIER_ENABLED_OFFSET                29U
#define HAIER_SET_TEMP_OFFSET               35U
#define HAIER_SET_BYTE(buff, offset, value) (buff)[offset] = (value)
#define HAIER_GET_BYTE(buff, offset)        (buff)[offset]
#define HAIER_SET_HEADER(buff)              *((uint16_t *) &(buff)[0]) = 0xFFFF
#define HAIER_GET_HEADER(buff)              *((uint16_t *) &(buff)[0])
#define HAIER_SET_LEN(buff, len)            HAIER_SET_BYTE(buff, HAIER_LEN_OFFSET, len)
#define HAIER_GET_LEN(buff)                 HAIER_GET_BYTE(buff, HAIER_LEN_OFFSET)
#define HAIER_SET_FLAGS(buff, flags)        HAIER_SET_BYTE(buff, HAIER_FLAGS_OFFSET, flags)
#define HAIER_GET_FLAGS(buff)               HAIER_GET_BYTE(buff, HAIER_FLAGS_OFFSET)
#define HAIER_SET_DIR1(buff, dir)           HAIER_SET_BYTE(buff, HAIER_DIR1_OFFSET, dir)
#define HAIER_GET_DIR1(buff)                HAIER_GET_BYTE(buff, HAIER_DIR1_OFFSET)
#define HAIER_SET_DIR2(buff, dir)           HAIER_SET_BYTE(buff, HAIER_DIR2_OFFSET, dir)
#define HAIER_GET_DIR2(buff)                HAIER_GET_BYTE(buff, HAIER_DIR2_OFFSET)
#define HAIER_SET_CMD(buff, cmd)            HAIER_SET_BYTE(buff, HAIER_CMD_OFFSET, cmd)
#define HAIER_GET_CMD(buff)                 HAIER_GET_BYTE(buff, HAIER_CMD_OFFSET)
#define HAIER_GET_ROOM_TEMP(buff)           HAIER_GET_BYTE(buff, HAIER_ROOM_TEMP_OFFSET)
#define HAIER_GET_ENABLED(buff)             HAIER_GET_BYTE(buff, HAIER_ENABLED_OFFSET)
#define HAIER_SET_MODE(buff, mode)          HAIER_SET_BYTE(buff, HAIER_MODE_OFFSET, mode)
#define HAIER_GET_MODE(buff)                HAIER_GET_BYTE(buff, HAIER_MODE_OFFSET)
#define HAIER_SET_FAN(buff, mode)           HAIER_SET_BYTE(buff, HAIER_FAN_OFFSET, mode)
#define HAIER_GET_FAN(buff)                 HAIER_GET_BYTE(buff, HAIER_FAN_OFFSET)
#define HAIER_SET_TEMP(buff, temp)          HAIER_SET_BYTE(buff, HAIER_SET_TEMP_OFFSET, temp - 16U)
#define HAIER_GET_TEMP(buff)                HAIER_GET_BYTE(buff, HAIER_SET_TEMP_OFFSET) + 16U

typedef struct
{
    uint8_t cmd;
    uint16_t param;
} cmd_queue_t;

#if CONFIG_HAIER_EMULATE_DATA
const uint8_t test_response[] = {0xff, 0xff, 0x22, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x02, 0x6d, 0x01, 0x00,
                                 0x1a, 0x00, 0x00, 0x00, 0x7f, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x03,
                                 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x37, 0x00, 0x09, 0x71};
#endif /* CONFIG_HAIER_EMULATE_DATA */

static const char *TAG           = "HAIER";
static QueueHandle_t uart_queue  = NULL;
static QueueHandle_t cmd_queue   = NULL;
static QueueHandle_t state_queue = NULL;
static TaskHandle_t haier_task   = NULL;
static uint8_t tx_buff[CONFIG_HAIER_BUF_LEN];
static uint8_t rx_buff[CONFIG_HAIER_BUF_LEN];

static int Haier_SendReceive(uint32_t req_len);
static void Haier_Task(void *arg);

/**
 * @brief Calculate Haier protocol checksum as sum of all bytes (except start 0xffff)
 *
 * @param data  data bytes to calc
 * @param len   data size
 * @return      Checksum value
 */
uint8_t Haier_ProtocolGetChecksum(const uint8_t *data, uint16_t len)
{
    uint8_t crc = 0x0;

    while (len)
    {
        crc += *data++;
        len--;
    }

    return crc;
}

/**
 * @brief           Haier Rx data parser
 *
 * @param data      Pointer to data
 * @param len       Data length
 * @param state     Pointer to state structure to fill
 * @return uint32_t Error code
 */
uint32_t Haier_ProtocolParser(const uint8_t *data, uint16_t len, haier_state_t *state)
{
    if (len < 36 || len > CONFIG_HAIER_BUF_LEN)
    {
        return HAIER_ERR_PARSING;
    }

    if (HAIER_GET_HEADER(data) != 0xFFFF)
    {
        ESP_LOGE(TAG, "Start bytes are not 0xFFFF");
        return HAIER_ERR_PARSING;
    }

    if (HAIER_GET_LEN(data) != len - 3) // Check length without start bytes and CRC byte
    {
        ESP_LOGE(TAG, "Frame length is not correct");
        return HAIER_ERR_PARSING;
    }

    if (HAIER_GET_FLAGS(data) > 0)
    {
        ESP_LOGE(TAG, "Checksum is not supported");
        return HAIER_ERR_PARSING;
    }

    if (data[len - 1] != Haier_ProtocolGetChecksum(&data[2], len - 2 - 1)) // Minus header and checksum
    {
        ESP_LOGE(TAG, "CRC is not correct");
        return HAIER_ERR_PARSING;
    }

    if (HAIER_GET_DIR1(data) != 0x02) // TODO Check second byte
    {
        ESP_LOGE(TAG, "Not response");
        return HAIER_ERR_PARSING;
    }

    state->en        = HAIER_GET_ENABLED(data);
    state->temp_curr = HAIER_GET_ROOM_TEMP(data);
    state->mode      = HAIER_GET_MODE(data);
    state->fan_speed = HAIER_GET_FAN(data);
    state->temp_set  = HAIER_GET_TEMP(data);

    return HAIER_ERR_OK;
}

uint32_t Haier_Init(void)
{
    /* UART Configuration */
    uart_config_t uart_config = {
        .baud_rate  = 9600,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    if (uart_param_config(CONFIG_HAIER_UART_PORT, &uart_config) != ESP_OK)
    {
        ESP_LOGE(TAG, "config uart parameter failed");
        return ERR_DRV;
    }

    if (uart_set_pin(CONFIG_HAIER_UART_PORT, GPIO_NUM_4, GPIO_NUM_5, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE) != ESP_OK)
    {
        ESP_LOGE(TAG, "config uart gpio failed");
        return ERR_DRV;
    }

    if (uart_driver_install(
            CONFIG_HAIER_UART_PORT, CONFIG_HAIER_UART_BUFF_SIZE, CONFIG_HAIER_UART_BUFF_SIZE, UART_EVENT_QUEUE_SIZE,
            &uart_queue, 0
        ) != ESP_OK)
    {
        ESP_LOGE(TAG, "install uart driver failed");
        return ERR_DRV;
    }

    cmd_queue   = xQueueCreate(5, sizeof(cmd_queue_t));
    state_queue = xQueueCreate(5, sizeof(haier_state_t));

    if (cmd_queue == NULL || state_queue == NULL)
    {
        return ERR_SYS;
    }

    xTaskCreate(Haier_Task, "Haier", 1024 * 6, NULL, 5, &haier_task);

    return ERR_OK;
}

uint8_t Haier_PrepareRequest(uint8_t *data, const uint8_t *state, uint8_t cmd, uint8_t value)
{
    uint8_t len = 0;

    if (cmd < HAIER_TEMP)
    {
        len = 10; // Short command
    }
    else
    {
        len = 34;
        memcpy(data, state, len + 2);
    }

    HAIER_SET_HEADER(data);   /* Frame separator */
    HAIER_SET_LEN(data, len); /* Length without header */
    HAIER_SET_FLAGS(data, 0);

    data[4] = 0; /* Dummy bytes */
    data[5] = 0;
    data[6] = 0;
    data[7] = 0;
    data[8] = 0x01;

    HAIER_SET_DIR1(data, 0x01); /* Request */
    HAIER_SET_DIR2(data, 0x4D); /* Request */
    if (cmd < HAIER_TEMP)
    {
        HAIER_SET_CMD(data, cmd); /* Command */
    }
    else
    {
        HAIER_SET_CMD(data, 0x5f); /* ??? Command */

        switch (cmd)
        {
        case HAIER_TEMP:
            HAIER_SET_TEMP(data, value);
            break;
        case HAIER_MODE:
            HAIER_SET_MODE(data, value);
            break;
        case HAIER_FAN:
            HAIER_SET_FAN(data, value);
            break;
        default:
            break;
        }
    }

    data[len + 2] = Haier_ProtocolGetChecksum(&data[2], len);

    return len + 2 + 1; /* with header and checksum */
}

static int Haier_SendReceive(uint32_t req_len)
{
    int resp_len = 0;

    if (req_len)
    {
#if CONFIG_HAIER_EMULATE_DATA
        resp_len = sizeof(test_response);
        memcpy(rx_buff, test_response, resp_len);
#else
        uart_write_bytes(CONFIG_HAIER_UART_PORT, tx_buff, req_len); //TODO check len
        resp_len = uart_read_bytes(CONFIG_HAIER_UART_PORT, rx_buff, sizeof(rx_buff), pdMS_TO_TICKS(100));
#endif /* CONFIG_HAIER_EMULATE_DATA */
    }

    return resp_len;
}

static void Haier_Task(void *arg)
{
    (void) arg;

    cmd_queue_t cmd;
    uint32_t req_len         = 0;
    int resp_len             = 0;
    BaseType_t cmd_queue_ret = pdFALSE;
#ifdef CONFIG_HAIER_POLL_TIMER
    TickType_t last_poll_tick = xTaskGetTickCount();
#endif /* CONFIG_HAIER_POLL_TIMER */

    while (1)
    {
        req_len       = 0;

        /* wait for a new command */
        cmd_queue_ret = xQueueReceive(cmd_queue, &cmd, pdMS_TO_TICKS(10));

        if (
            (cmd_queue_ret == pdTRUE)
#ifdef CONFIG_HAIER_POLL_TIMER
            || (xTaskGetTickCount() - last_poll_tick > pdMS_TO_TICKS(CONFIG_HAIER_POLL_TIMER))
#endif /* CONFIG_HAIER_POLL_TIMER */
        )
        {
            /* Get the state first if new command received or polling timer is elapsed*/
            req_len = Haier_PrepareRequest(tx_buff, NULL, HAIER_STATE, 0);

            ESP_LOGI(TAG, "State request %" PRIu32 " bytes", req_len);
            ESP_LOG_BUFFER_HEXDUMP(TAG, tx_buff, req_len, ESP_LOG_INFO);

            resp_len       = Haier_SendReceive(req_len);
#ifdef CONFIG_HAIER_POLL_TIMER
            last_poll_tick = xTaskGetTickCount();
#endif /* CONFIG_HAIER_POLL_TIMER */

            if (resp_len > 0)
            {
                ESP_LOGI(TAG, "Read %d bytes.", resp_len);
                ESP_LOG_BUFFER_HEXDUMP(TAG, rx_buff, resp_len, ESP_LOG_INFO);
            }

            /* If the command is present (except status request) */
            if ((cmd_queue_ret == pdTRUE) && (cmd.cmd > HAIER_STATE))
            {
                req_len = Haier_PrepareRequest(tx_buff, rx_buff, cmd.cmd, cmd.param);

                ESP_LOGI(TAG, "Command request %" PRIu32 " bytes", req_len);
                ESP_LOG_BUFFER_HEXDUMP(TAG, tx_buff, req_len, ESP_LOG_INFO);

                resp_len = Haier_SendReceive(req_len);

                if (resp_len > 0)
                {
                    ESP_LOGI(TAG, "Read %d bytes.", resp_len);
                    ESP_LOG_BUFFER_HEXDUMP(TAG, rx_buff, resp_len, ESP_LOG_INFO);
                }
            }

            haier_state_t state;
            if (Haier_ProtocolParser(rx_buff, resp_len, &state) == HAIER_ERR_OK)
            {
                xQueueSend(state_queue, &state, pdMS_TO_TICKS(10));
            }
        }
    }
}

uint32_t Haier_Cmd(uint8_t cmd, uint8_t param)
{
    cmd_queue_t queue_val = {.cmd = cmd, .param = param};

    if (xQueueSend(cmd_queue, &queue_val, pdMS_TO_TICKS(10)) == pdTRUE)
    {
        return ERR_OK;
    }

    return ERR_SYS;
}

uint32_t Haier_GetState(haier_state_t *state, TickType_t timeout)
{
    if (state_queue != NULL && xQueueReceive(state_queue, state, timeout) == pdTRUE)
    {
        return ERR_OK;
    }

    return ERR_SYS;
}
