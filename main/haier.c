#include "haier.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#ifndef CONFIG_HAIER_UART_PORT
#define CONFIG_HAIER_UART_PORT UART_NUM_1
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

#ifndef CONFIG_HAIER_UART_BUFF_SIZE
#define CONFIG_HAIER_UART_BUFF_SIZE 256
#endif

#define UART_EVENT_QUEUE_SIZE 20

#define ERR_OK                0
#define ERR_DRV               1
#define ERR_SYS               2

#pragma pack(push, 1)
typedef struct
{
    uint16_t start;   /*!< Frame start 0xFFFF */
    uint8_t len;      /*!< Frame len */
    uint8_t flags;    /*!< Frame flags. Indicates CRC presence */
    uint8_t dummy[5]; /*!< Dummy bytes */
    uint8_t type;     /*!< Frame type. 1 - request, 2 response */
} haier_hdr_t;
#pragma pack(pop)

typedef struct
{
    uint8_t cmd;
    uint16_t param;
} cmd_queue_t;

static const char *TAG           = "HAIER";
static QueueHandle_t uart_queue  = NULL;
static QueueHandle_t cmd_queue   = NULL;
static QueueHandle_t state_queue = NULL;
static TaskHandle_t haier_task   = NULL;
static uint8_t tx_buff[128]      = {0xFF, 0xFF, 0};
static uint8_t rx_buff[128];

static void Haier_Task(void *arg);

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

    if (uart_driver_install(CONFIG_HAIER_UART_PORT, CONFIG_HAIER_UART_BUFF_SIZE, CONFIG_HAIER_UART_BUFF_SIZE,
                            UART_EVENT_QUEUE_SIZE, &uart_queue, 0) != ESP_OK)
    {
        ESP_LOGE(TAG, "install uart driver failed");
        return ERR_DRV;
    }

    cmd_queue   = xQueueCreate(5, sizeof(cmd_queue_t));
    state_queue = xQueueCreate(5, sizeof(state_queue_t));

    if (cmd_queue == NULL || state_queue == NULL)
    {
        return ERR_SYS;
    }

    xTaskCreate(Haier_Task, "Haier", 1024 * 6, NULL, 3, &haier_task);

    return ERR_OK;
}

static void Haier_Task(void *arg)
{
    (void) arg;

    cmd_queue_t cmd;
    uint32_t len = 0;

    while (1)
    {
        if (xQueueReceive(cmd_queue, &cmd, 10 / portTICK_PERIOD_MS) == pdTRUE)
        {
            len = 0;

            switch (cmd.cmd)
            {
            case HAIER_ON:
                ESP_LOGI(TAG, "On cmd");
                tx_buff[2]  = 0x0A; /* Length */
                tx_buff[8]  = 0x01;
                tx_buff[9]  = 0x01; /* Request */
                tx_buff[10] = 0x4D; /* Request */
                tx_buff[11] = 0x02; /* ON */
                tx_buff[12] = 0x5B; /* CRC */
                len         = 13;
                break;
            case HAIER_OFF:
                ESP_LOGI(TAG, "Off cmd");
                tx_buff[2]  = 0x0A; /* Length */
                tx_buff[8]  = 0x01;
                tx_buff[9]  = 0x01; /* Request */
                tx_buff[10] = 0x4D; /* Request */
                tx_buff[11] = 0x03; /* OFF */
                tx_buff[12] = 0x5C; /* CRC */
                len         = 13;
                break;
            case HAIER_STATE:
                ESP_LOGI(TAG, "State cmd");
                tx_buff[2]  = 0x0A; /* Length */
                tx_buff[8]  = 0x01;
                tx_buff[9]  = 0x01; /* Request */
                tx_buff[10] = 0x4D; /* Request */
                tx_buff[11] = 0x01; /* State */
                tx_buff[12] = 0x5A; /* CRC */
                len         = 13;
                break;
            default:
                break;
            }

            if (len)
            {
                uart_write_bytes(CONFIG_HAIER_UART_PORT, tx_buff, len);
            }
        }

        const int rxBytes = uart_read_bytes(CONFIG_HAIER_UART_PORT, rx_buff, sizeof(rx_buff), 100 / portTICK_PERIOD_MS);
        if (rxBytes > 0)
        {
            rx_buff[rxBytes] = 0;
            ESP_LOGI(TAG, "Read %d bytes: '%s'", rxBytes, rx_buff);
            ESP_LOG_BUFFER_HEXDUMP(TAG, rx_buff, rxBytes, ESP_LOG_INFO);

            if (rx_buff[0] == 0xFF && rx_buff[2] == 0x22)
            {
                state_queue_t state = {.en        = rx_buff[29],
                                       .temp_curr = rx_buff[13],
                                       .mode      = rx_buff[23],
                                       .fan_speed = rx_buff[25],
                                       .temp_set  = 16 + rx_buff[35]};

                xQueueSend(state_queue, &state, 10 / portTICK_PERIOD_MS);
            }
        }
    }
}

uint32_t Haier_Cmd(uint8_t c)
{
    cmd_queue_t cmd = {
        .cmd = c,
    };

    if (xQueueSend(cmd_queue, &cmd, 10 / portTICK_PERIOD_MS) == pdTRUE)
    {
        return ERR_OK;
    }

    return ERR_SYS;
}

uint32_t Haier_GetState(state_queue_t *state, TickType_t timeout)
{
    if (xQueueReceive(state_queue, state, timeout) == pdTRUE)
    {
        return ERR_OK;
    }

    return ERR_SYS;
}
