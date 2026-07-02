#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>

#include "esp_system.h"
#include "esp_attr.h"
#include "esp_chip_info.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"
#include "freertos/ringbuf.h"

#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

#include "nvs_flash.h"
#include "hal/gpio_types.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"

#include "nn_model.h"
#include "emg_filter.h"

#define GESTURE_CHR_UUID                0x78, 0x56, 0x34, 0x12, 0x34, 0x12, 0x78, 0x56, \
                                        0x34, 0x12, 0x34, 0x12, 0x78, 0x56, 0x34, 0x12      // GATT characteristic UUID

#define RAWDATA_CHR_UUID                0x77, 0x56, 0x34, 0x12, 0x34, 0x12, 0x78, 0x56, \
                                        0x34, 0x12, 0x34, 0x12, 0x78, 0x56, 0x34, 0x12      // GATT characteristic UUID

#define ADC_SAMPLE_SIZE                 (3 + (CONFIG_AD1299_NUM_CH * 3))     // one sample size

#define RAW_SAMPLES_PER_PACKET          15

#define PROG_BTN_TIMEOUT                30000

typedef struct {
    uint32_t packet_counter;
    int32_t  samples[RAW_SAMPLES_PER_PACKET][CONFIG_AD1299_NUM_CH];
} ble_raw_packet_t;

typedef enum {
    BLE_MSG_RAW_DATA,
    BLE_MSG_GESTURE_VECTOR
} ble_msg_type_t;

typedef struct {
    ble_msg_type_t type;
    union {
        ble_raw_packet_t raw;
        output_vector_packet_t vector;
    } data;
} ble_tx_msg_t;

spi_device_handle_t       spi_dev;

TaskHandle_t              ble_task_handle;
TaskHandle_t              data_pump_task_handle;
TaskHandle_t              mode_sw_task_handle;

uint16_t                  gatt_gesture_handle;
uint16_t                  gatt_rawdata_handle;                

uint16_t                  current_conn_handle = BLE_HS_CONN_HANDLE_NONE;

uint8_t                   *g_spi_rx_buf = NULL;
uint8_t                   *g_spi_tx_buf = NULL;

DRAM_ATTR QueueHandle_t   emg_data_queue;
DRAM_ATTR QueueHandle_t   ble_tx_queue;

DRAM_ATTR nn_sample_t     nn_window[WINDOW_SIZE];                           // 1D-CNN input window         

static const char *TAG                          = "myocell_s3" ;

static volatile bool raw_data_mode              = false;

static const gpio_num_t     PROG_BTN_PIN        = GPIO_NUM_0 ;          // ADS<--ESP Power down pin (active low)
static const gpio_num_t     AD1299_PWDN_PIN     = GPIO_NUM_8 ;          // ADS<--ESP Power down pin (active low)
static const gpio_num_t     AD1299_RESET_PIN    = GPIO_NUM_48 ;         // ADS<--ESP Reset pin (active low)
static const gpio_num_t     AD1299_DRDY_PIN     = GPIO_NUM_14 ;         // ADS-->ESP DRDY pin (active low)
static const gpio_num_t     AD1299_START_PIN    = GPIO_NUM_21 ;         // ADS<--ESP Start data conversion pint (active high)

// SPI comands (see https://www.ti.com/lit/ds/symlink/ads1299.pdf?ts=1599826124971)
static const uint8_t        AD1299_CMD_RREG     = 0x20 ;                // Read register
static const uint8_t        AD1299_CMD_WREG     = 0x40 ;                // Write to register
static const uint8_t        AD1299_CMD_RDATAC   = 0x10 ;                // Start continouous mode
static const uint8_t        AD1299_CMD_SDATAC   = 0x11 ;                // Stop continuous mode

// Register addresses available through SPI (see https://www.ti.com/lit/ds/symlink/ads1299.pdf?ts=1599826124971)
static const uint8_t        AD1299_ADDR_ID      = 0x00 ;                // ID register
static const uint8_t        AD1299_ADDR_CONFIG1 = 0x01 ;                // CONFIG1 register
static const uint8_t        AD1299_ADDR_CONFIG2 = 0x02 ;                // CONFIG2 register
static const uint8_t        AD1299_ADDR_CONFIG3 = 0x03 ;                // CONFIG3 register
static const uint8_t        AD1299_ADDR_LEADOFF = 0x04 ;
static const uint8_t        AD1299_ADDR_CH1SET  = 0x05 ;
static const uint8_t        AD1299_ADDR_CH2SET  = 0x06 ;
static const uint8_t        AD1299_ADDR_CH3SET  = 0x07 ;
static const uint8_t        AD1299_ADDR_CH4SET  = 0x08 ;
static const uint8_t        AD1299_ADDR_BIAS_SENSP  = 0x0D ;
static const uint8_t        AD1299_ADDR_BIAS_SENSN  = 0x0E ;
// AD1299 constants (see https://www.ti.com/lit/ds/symlink/ads1299.pdf?ts=1599826124971)

void spi_data_pump_task(void* pvParameter);

// send 8bit command
void ad1299_send_cmd8(spi_device_handle_t spi, const uint8_t cmd)
{
    esp_err_t ret ;
    spi_transaction_t t ;
    memset(&t, 0, sizeof(spi_transaction_t)) ;      // Zero out the transaction
    t.flags         = SPI_TRANS_USE_TXDATA ;        // Bitwise OR of SPI_TRANS_* flags
    t.length        = 8 ;                           // Total data length, in bits
    t.user          = (void*)0 ;                    // User-defined variable. Can be used to store eg transaction ID.
    t.tx_data[0]    = cmd ;                         // Pointer to transmit buffer, or NULL for no MOSI phase
    t.rx_buffer     = NULL ;                        // Pointer to receive buffer, or NULL for no MISO phase. Written by 4 bytes-unit if DMA is used.

    int icmd = cmd ;
    ESP_LOGI( TAG, "ad1299_cmd8: send command:0x%02X", icmd ) ;

    ret = spi_device_polling_transmit( spi, &t ) ;  // send command
    if (ret==ESP_OK)
    {
        ESP_LOGI( TAG, "Sent successfuly" ) ;
    }
    ESP_ERROR_CHECK(ret) ;            //Should have had no issues.
}

// Write to ad1299 register
void ad1299_wreg(spi_device_handle_t spi, const uint8_t addr, const uint8_t value)
{
    esp_err_t ret ;
    spi_transaction_t t ;
    memset(&t, 0, sizeof(spi_transaction_t)) ;      // Zero out the transaction
    t.flags         = SPI_TRANS_USE_TXDATA ;        // Bitwise OR of SPI_TRANS_* flags
    t.length        = 8*3 ;                         // Total data length, in bits
    t.user          = (void*)0 ;                    // User-defined variable. Can be used to store eg transaction ID.
    t.tx_data[0]    = AD1299_CMD_WREG|addr ;
    t.tx_data[1]    = 0 ;                           // 1 register to read
    t.tx_data[2]    = value ;                       // value to write
    t.rx_buffer     = NULL ;                        // Pointer to receive buffer, or NULL for no MISO phase. Written by 4 bytes-unit if DMA is used.

    ESP_LOGI( TAG, "ad1299_wreg :0x%02X to REG:0x%02X", value, addr ) ;
    ret = spi_device_polling_transmit(spi, &t) ;    // Transmit!
    if (ret==ESP_OK)
    {
        ESP_LOGI( TAG, "Sent successfuly" ) ;
    }
    ESP_ERROR_CHECK(ret) ;                          // Should have had no issues.
}

uint8_t ad1299_rreg(spi_device_handle_t spi, const uint8_t addr)
{
    esp_err_t ret ;
    spi_transaction_t t ;
    memset( &t, 0, sizeof(spi_transaction_t) ) ;    // Zero out the transaction
    t.flags         = SPI_TRANS_USE_RXDATA|
                      SPI_TRANS_USE_TXDATA ;        // Bitwise OR of SPI_TRANS_* flags
    t.length        = 8*3 ;                         // Total data length, in bits
    t.rxlength      = 0 ;                           // Total data length received, should be not greater than ``length`` in full-duplex mode (0 defaults this to the value of ``length``)
    t.user          = (void*)0 ;                    // User-defined variable. Can be used to store eg transaction ID.

    t.tx_data[0]    = AD1299_CMD_RREG|addr ;
    t.tx_data[1]    = 0 ;                           // 1 register to read
    t.tx_data[2]    = 0 ;                           // NOP

    ESP_LOGI( TAG, "ad1299_rreg: read from REG:0x%02X", addr ) ;
    ret = spi_device_polling_transmit(spi, &t) ;
    if (ret==ESP_OK)
    {
        //ESP_LOGI( TAG, "Read successfuly: 0x%02X", t.rx_data[0] ) ;
        //ESP_LOGI( TAG, "Read successfuly: 0x%02X", t.rx_data[1] ) ;
        //ESP_LOGI( TAG, "Read successfuly: 0x%02X", t.rx_data[2] ) ;
        //ESP_LOGI( TAG, "Read successfuly: 0x%02X", t.rx_data[3] ) ;
    }
    ESP_ERROR_CHECK(ret) ;                          //Should have had no issues.

    return (t.rx_data[2]) ;

}

/*
    ad1299_read_data_block(spi_device_handle_t spi)
    spi     - spi handle
*/
IRAM_ATTR esp_err_t ad1299_read_data_block(spi_device_handle_t spi)
{
    spi_transaction_t t;
    memset(&t, 0, sizeof(spi_transaction_t));    // Zero out the transaction    
    t.length        = ADC_SAMPLE_SIZE * 8;
    t.rxlength      = ADC_SAMPLE_SIZE * 8;
    t.user          = (void*)0;
    
    t.tx_buffer     = g_spi_tx_buf;           
    t.rx_buffer     = g_spi_rx_buf;           

    return spi_device_polling_transmit(spi, &t);
}

// DRDY signal ISR
// DRDY becomes low when ADS1299 collect 8 samples (1 sample of 24bit for each channel )
// and ready to transfer these data to ESP32 using 1 SPI transaction with 9*24 bits length
IRAM_ATTR void drdy_gpio_isr_handler(void* arg)
{
    static BaseType_t high_task_wakeup = pdFALSE ;

    vTaskNotifyGiveFromISR( data_pump_task_handle, &high_task_wakeup ) ;

    /* If high_task_wakeup was set to true you
    should yield.  The actual macro used here is
    port specific. */
    if ( high_task_wakeup )
    {
        portYIELD_FROM_ISR( ) ;
    }

}

// PROG button toggle ISR
IRAM_ATTR void mode_sw_isr_handler(void* arg) 
{
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    
    vTaskNotifyGiveFromISR(mode_sw_task_handle, &xHigherPriorityTaskWoken);
    
    if (xHigherPriorityTaskWoken == pdTRUE) {
        portYIELD_FROM_ISR();
    }
}

static void mode_sw_handler_task(void *pvParameters) 
{
    while (1) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        static uint64_t last_isr_time = 0; 
        uint64_t current_time = esp_timer_get_time(); 
        if ((current_time - last_isr_time) > PROG_BTN_TIMEOUT) {
            raw_data_mode = !raw_data_mode;
            ESP_LOGW(TAG, "mode changed: %s", 
                     raw_data_mode ? "RawData" : "Inference");
            last_isr_time = current_time; 
        }
    }
}

static int gatt_gesture_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                                  struct ble_gatt_access_ctxt *ctxt,
                                  void *arg) 
{
    return BLE_ATT_ERR_UNLIKELY;    // notify-only
}

static int gatt_rawdata_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                                  struct ble_gatt_access_ctxt *ctxt,
                                  void *arg) 
{
    return BLE_ATT_ERR_UNLIKELY;    // notify-only
}

// GATT services and characteristics tree
const struct ble_gatt_svc_def gatt_tree[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,  
        .uuid = BLE_UUID16_DECLARE(0xFFF0),
        .characteristics = (struct ble_gatt_chr_def[]) { {
            .uuid = BLE_UUID128_DECLARE(GESTURE_CHR_UUID),
            .flags = BLE_GATT_CHR_F_NOTIFY,
            .val_handle = &gatt_gesture_handle,
            .access_cb = gatt_gesture_access_cb,
        }, 
        {
            .uuid = BLE_UUID128_DECLARE(RAWDATA_CHR_UUID),
            .flags = BLE_GATT_CHR_F_NOTIFY,
            .val_handle = &gatt_rawdata_handle,
            .access_cb = gatt_rawdata_access_cb,
        },
        {
            0,
        } }
    },
    {
        0,
    },
};

int ble_gap_event(struct ble_gap_event *event, void *arg);

void ble_app_advertise(void) 
{
    struct ble_gap_adv_params adv_params;
    struct ble_hs_adv_fields fields;
    int rc;

    memset(&fields, 0, sizeof(fields));
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    
    // Actual device name
    fields.name = (uint8_t *)"ESP32-S3-EMG"; 
    fields.name_len = strlen((char *)fields.name);
    fields.name_is_complete = 1;

    rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) return;

    // Connection settings
    memset(&adv_params, 0, sizeof(adv_params));
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND; // Allow any connection
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN; // Visible to everyone

    ble_gap_adv_start(BLE_OWN_ADDR_PUBLIC, NULL, BLE_HS_FOREVER, &adv_params, ble_gap_event, NULL);
}

int ble_gap_event(struct ble_gap_event *event, void *arg) 
{
    switch (event->type) {
        case BLE_GAP_EVENT_CONNECT:
            ESP_LOGI("BLE", "Client connected successfully!");
            current_conn_handle = event->connect.conn_handle;
            break;
        case BLE_GAP_EVENT_DISCONNECT:
            ESP_LOGI("BLE", "Client disconnected. Restart advertising!");
            current_conn_handle = BLE_HS_CONN_HANDLE_NONE;
            ble_app_advertise(); // Restart advertising on device disconnect
            break;
    }
    return 0;
}


void ble_app_on_sync(void) 
{
    // set GAP device name
    int rc = ble_svc_gap_device_name_set("ESP32-S3-EMG");
    if (rc != 0) {
        ESP_LOGE("BLE", "Error setting device name: %d", rc);
    }

    // get address type automaticaly
    uint8_t own_addr_type;
    rc = ble_hs_id_infer_auto(0, &own_addr_type);
    if (rc != 0) {
        ESP_LOGE("BLE", "Error getting address: %d", rc);
    }
    
    ble_app_advertise(); 
}

void ble_host_task(void *param) 
{
    ESP_LOGI("BLE", "BLE Host Task Started");
    
    nimble_port_run(); 
    
    nimble_port_freertos_deinit();
}

void init_ble(void) 
{
    // Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
      ESP_ERROR_CHECK(nvs_flash_erase());
      ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_ERROR_CHECK(nimble_port_init());

    ble_hs_cfg.sync_cb = ble_app_on_sync;
    
    ble_att_set_preferred_mtu(256);

    ble_svc_gap_init();
    ble_svc_gatt_init();

    int rc = ble_gatts_count_cfg(gatt_tree);
    ESP_ERROR_CHECK(rc);
    rc = ble_gatts_add_svcs(gatt_tree);
    ESP_ERROR_CHECK(rc);

    nimble_port_freertos_init(ble_host_task); // pinned to core 0
    
    ESP_LOGI("BLE", "NimBLE initialized succesfully!");
}

void ble_transmission_task(void *pvParameters) 
{
    ble_tx_msg_t msg;

    while (1) {
        if (xQueueReceive(ble_tx_queue, &msg, portMAX_DELAY) == pdTRUE) {
            
            if (current_conn_handle == BLE_HS_CONN_HANDLE_NONE) {
                continue;
            }

            struct os_mbuf *om = NULL;
            uint16_t attr_handle = 0;

            if (msg.type == BLE_MSG_RAW_DATA) {
                om = ble_hs_mbuf_from_flat(&msg.data.raw, sizeof(ble_raw_packet_t));
                attr_handle = gatt_rawdata_handle;
            } 
            else if (msg.type == BLE_MSG_GESTURE_VECTOR) {
                om = ble_hs_mbuf_from_flat(&msg.data.vector, sizeof(output_vector_packet_t));
                attr_handle = gatt_gesture_handle;
            }

            if (!om) {
                ESP_LOGE("BLE", "Failed to allocate mbuf for BLE");
                continue;
            }

            int rc = ble_gatts_notify_custom(current_conn_handle, attr_handle, om);
            if (rc != 0) {
                ESP_LOGE("BLE", "Error sending notify (type %d); rc=%d", msg.type, rc);
                continue;
            }
            
            if (msg.type == BLE_MSG_GESTURE_VECTOR)
                ESP_LOGI("BLE", "Notification sent successfully!");
        }
    }
}

void nn_inference_task(void *pvParameters) {
    memset(nn_window, 0, sizeof(nn_window));
    
    while(1) {
        ble_tx_msg_t msg;
        msg.type = BLE_MSG_GESTURE_VECTOR;
        // wait for 50 samples packet
        nn_sample_t received_data[NEW_DATA_STEP];
        if (xQueueReceive(emg_data_queue, received_data, portMAX_DELAY) == pdTRUE) {
            // left shift
            memmove(&nn_window[0], 
                    &nn_window[NEW_DATA_STEP], 
                    sizeof(nn_sample_t) * (WINDOW_SIZE - NEW_DATA_STEP));

            // copy new data at the end of array
            memcpy(&nn_window[WINDOW_SIZE - NEW_DATA_STEP], 
                    received_data, 
                    sizeof(nn_sample_t) * NEW_DATA_STEP);
            
            int64_t start_time = esp_timer_get_time();
            invoke_1d_cnn(nn_window, &msg.data.vector);
            int64_t end_time = esp_timer_get_time();
             printf("Inference Time: %lld us\n", (end_time - start_time));

            xQueueSend(ble_tx_queue, &msg, 0);
            vTaskDelay(portTICK_PERIOD_MS);
        }
    }
}

static void myocell_app_start(void)
{
    esp_err_t ret           = 0 ;

    ESP_LOGI(TAG, "Initialize GPIO lines") ;

    // Initialize GPIO pins
    gpio_reset_pin( AD1299_PWDN_PIN ) ;
    gpio_set_direction( AD1299_PWDN_PIN, GPIO_MODE_OUTPUT ) ;

    gpio_reset_pin( AD1299_RESET_PIN ) ;
    gpio_set_direction( AD1299_RESET_PIN, GPIO_MODE_OUTPUT ) ;

    gpio_reset_pin( AD1299_START_PIN ) ;
    gpio_set_direction( AD1299_START_PIN, GPIO_MODE_OUTPUT ) ;

    gpio_reset_pin( AD1299_DRDY_PIN ) ;
    gpio_set_direction( AD1299_DRDY_PIN, GPIO_MODE_INPUT ) ;
    gpio_set_intr_type( AD1299_DRDY_PIN, GPIO_INTR_NEGEDGE ) ;
    gpio_intr_enable( AD1299_DRDY_PIN ) ;

    // See 10.1.2 Setting the Device for Basic Data Capture (ADS1299 Datasheet)
    ESP_LOGI(TAG, "Set PWDN & RESET to 1") ;
    gpio_set_level(AD1299_PWDN_PIN,     1 ) ;
    gpio_set_level(AD1299_RESET_PIN,    1 ) ;
    gpio_set_level(AD1299_START_PIN,    0 ) ;

    ESP_LOGI(TAG, "Wait for 20 tclk") ;

    vTaskDelay( 100 / portTICK_PERIOD_MS ) ;

    // Reset pulse
    ESP_LOGI(TAG, "Reset ad1299") ;
    gpio_set_level(AD1299_RESET_PIN, 0 ) ;
    vTaskDelay( 100 / portTICK_PERIOD_MS ) ;
    gpio_set_level(AD1299_RESET_PIN, 1 ) ;

    vTaskDelay( 500 / portTICK_PERIOD_MS ) ;

    g_spi_rx_buf = heap_caps_malloc(ADC_SAMPLE_SIZE, MALLOC_CAP_DMA);
    g_spi_tx_buf = heap_caps_malloc(ADC_SAMPLE_SIZE, MALLOC_CAP_DMA);
    if (g_spi_rx_buf == NULL || g_spi_tx_buf == NULL)
    {
        ESP_LOGE(TAG, "error: not enough memory" );
        return;
    }

    ESP_LOGI(TAG, "Initialize SPI driver...") ;
    // SEE esp-idf/components/driver/include/driver/spi_common.h
    spi_bus_config_t buscfg = {
        .miso_io_num        = GPIO_NUM_13,
        .mosi_io_num        = GPIO_NUM_11,
        .sclk_io_num        = GPIO_NUM_12,
        .quadwp_io_num      = -1,
        .quadhd_io_num      = -1,
        .flags              = 0,                        // Abilities of bus to be checked by the driver. Or-ed value of ``SPICOMMON_BUSFLAG_*`` flags.
        .intr_flags         = ESP_INTR_FLAG_IRAM,
        .max_transfer_sz    = 0                         // maximum data size in bytes, 0 means 4094
    } ;

    // see esp-idf/components/driver/include/driver/spi_master.h
    spi_device_interface_config_t devcfg = {
        .command_bits       = 0,                        // 0-16
        .address_bits       = 0,                        // 0-64
        .dummy_bits         = 0,                        // Amount of dummy bits to insert between address and data phase
        .clock_speed_hz     = 1000000,                  // Clock speed, divisors of 80MHz, in Hz. See ``SPI_MASTER_FREQ_*``.
        .mode               = 1,                        // SPI mode 0
        .input_delay_ns     = 0,                        // The time required between SCLK and MISO
        .spics_io_num       = GPIO_NUM_10,              // CS pin
        .queue_size         = 1,                        // No queued transactions
        .cs_ena_pretrans    = 0,                        // 0 not used
        .cs_ena_posttrans   = 0,                        // 0 not used
    } ;

    //Initialize the SPI bus
    ret                     = spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO) ;
    ESP_ERROR_CHECK(ret) ;
    //Attach the LCD to the SPI bus
    ret                     = spi_bus_add_device(SPI2_HOST, &devcfg, &spi_dev ) ;
    ESP_ERROR_CHECK(ret) ;

    // Send SDATAC / Stop Read Data Continuously Mode
    ESP_LOGI(TAG, "Send SDATAC") ;

    ad1299_send_cmd8( spi_dev, AD1299_CMD_SDATAC ) ;
    vTaskDelay( 100 / portTICK_PERIOD_MS ) ;

    // RREG id
    ESP_LOGI(TAG, "Read chip Id from Reg#0:") ;
    uint8_t valueu8             = ad1299_rreg( spi_dev, AD1299_ADDR_ID ) ;

    uint8_t ad1299_rev_id       = valueu8>>5 ;
    uint8_t ad1299_check_bit    = (valueu8>>4) & 0x01 ;
    uint8_t ad1299_dev_id       = (valueu8>>2) & 0x03 ;
    uint8_t ad1299_num_ch       = (valueu8) & 0x03 ;

    if (ad1299_check_bit)
    {
        ESP_LOGI(TAG,"ads1299 found:") ;
        ESP_LOGI(TAG, "!-->ad1299_rev_id:       0x%02X",  ad1299_rev_id ) ;
        ESP_LOGI(TAG, "!-->ad1299_check_bit:    %1d (should be 1)",  ad1299_check_bit ) ;
        ESP_LOGI(TAG, "!-->ad1299_dev_id:       0x%02X",  ad1299_dev_id ) ;
        ESP_LOGI(TAG, "!-->ad1299_num_ch:       0x%02X",  ad1299_num_ch ) ;
    }
    else
    {
        ESP_LOGE(TAG, "error: ads1299 not found!" ) ;
        return ;
    }


    ESP_LOGI(TAG, "Set internal reference" ) ;
    ad1299_wreg( spi_dev, AD1299_ADDR_CONFIG3, 0xd8) ;
    vTaskDelay( 100 / portTICK_PERIOD_MS ) ;

    ad1299_wreg(spi_dev, AD1299_ADDR_BIAS_SENSP, 0x0c);
    vTaskDelay( 100 / portTICK_PERIOD_MS ) ;
    
    ad1299_wreg(spi_dev, AD1299_ADDR_BIAS_SENSN, 0x0c);
    vTaskDelay( 100 / portTICK_PERIOD_MS ) ;

    // Set device for DR=fmod/4096
    // Enable clk output
    ESP_LOGI(TAG, "Set sampling rate" ) ;
    ad1299_wreg( spi_dev, AD1299_ADDR_CONFIG1, 0x94 ) ;     // Default 0x96 (see power up sequence)
    vTaskDelay( 100 / portTICK_PERIOD_MS ) ;

    // Configure test signal parameters
    ad1299_wreg( spi_dev, AD1299_ADDR_CONFIG2, 0xb5 ) ;
    vTaskDelay( 100 / portTICK_PERIOD_MS ) ;

    // Configure test signal parameters
    ad1299_wreg( spi_dev, AD1299_ADDR_LEADOFF, 0x0e ) ;
    vTaskDelay( 100 / portTICK_PERIOD_MS ) ;

    // Set All Channels to Input Short
    for (int i=0;i<CONFIG_AD1299_NUM_CH;i++)
    {
        ad1299_wreg( spi_dev, AD1299_ADDR_CH1SET+i, 0x01 ) ;
        //vTaskDelay( 50 / portTICK_PERIOD_MS ) ;
    }
    vTaskDelay( 50 / portTICK_PERIOD_MS ) ;

    // Activate Conversion
    // After This Point DRDY Toggles at
    // fCLK / 8192
    gpio_set_level(AD1299_START_PIN, 1 ) ;
    vTaskDelay( 50 / portTICK_PERIOD_MS ) ;

    // Stop all the channels
    ad1299_send_cmd8( spi_dev, AD1299_CMD_SDATAC ) ;
    vTaskDelay( 100 / portTICK_PERIOD_MS ) ;

    // Configure channels
    ad1299_wreg( spi_dev, AD1299_ADDR_CH1SET, 0x81 ) ;      // CH1: Short circuit
    vTaskDelay( 100 / portTICK_PERIOD_MS ) ;

    ad1299_wreg( spi_dev, AD1299_ADDR_CH2SET, 0x81 ) ;      // CH2: Short circuit
    vTaskDelay( 100 / portTICK_PERIOD_MS ) ;

    ad1299_wreg( spi_dev, AD1299_ADDR_CH3SET, 0x00 ) ;      // CH3: Normal, PGA_Gain=1
    vTaskDelay( 100 / portTICK_PERIOD_MS ) ;

    ad1299_wreg( spi_dev, AD1299_ADDR_CH4SET, 0x00 ) ;      // CH4: Normal, PGA_Gain=1
    vTaskDelay( 100 / portTICK_PERIOD_MS ) ;

    ESP_LOGI(TAG, "Put device in RDATAC mode" ) ;
    ad1299_send_cmd8( spi_dev, AD1299_CMD_RDATAC ) ;
    vTaskDelay( 50 / portTICK_PERIOD_MS ) ;

    emg_data_queue = xQueueCreate(10, sizeof(nn_sample_t) * NEW_DATA_STEP);
    
    ble_tx_queue = xQueueCreate(20, sizeof(ble_tx_msg_t));

    // Start Data pump task on Core#1
    xTaskCreatePinnedToCore( &spi_data_pump_task, "spi_data_pump_task", 4096, NULL, 24, &data_pump_task_handle, 1 ) ;
    configASSERT( data_pump_task_handle ) ;

    xTaskCreatePinnedToCore(mode_sw_handler_task, "mode_sw_task", 4096, NULL, 10, &mode_sw_task_handle, 1);
    configASSERT( mode_sw_task_handle ) ;

    xTaskCreatePinnedToCore( &ble_transmission_task, "ble_transmission_task", 4096, NULL, 4, NULL, 0 ) ;

    xTaskCreatePinnedToCore( &nn_inference_task, "nn_inference_task", 8192, NULL, 5, NULL, 0);

    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << PROG_BTN_PIN),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_POSEDGE
    };
    gpio_config(&io_conf);

    // Install ISR for all GPIOs
    gpio_install_isr_service(0) ;

    // Configure ISR for DRDY signal
    gpio_isr_handler_add( AD1299_DRDY_PIN, drdy_gpio_isr_handler, NULL ) ;

    // Configure ISR for PROG button
    gpio_isr_handler_add( PROG_BTN_PIN, mode_sw_isr_handler, NULL ) ;
}

void spi_data_pump_task( void* pvParameter )
{
    int raw_sample_count = 0;
    int nn_sample_count = 0;
    
    bool last_mode = raw_data_mode;

    static float last_valid_ch[CONFIG_AD1299_NUM_CH] = {0.0f};

    static ble_tx_msg_t ble_msg;
    uint32_t ble_packet_cnt = 0;
    
    static nn_sample_t step[NEW_DATA_STEP];
    
    memset(&ble_msg, 0, sizeof(ble_tx_msg_t));
    memset(step, 0, sizeof(step));   

    while(1)
    {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        esp_err_t ret = ad1299_read_data_block(spi_dev);
        
        if (ret == ESP_OK && ((g_spi_rx_buf[0] & 0xf0) == 0xc0)) {
            for (int ch = 0; ch < CONFIG_AD1299_NUM_CH; ch++) {
                int byte_offset = 3 + (ch * 3);
                int32_t sample = (g_spi_rx_buf[byte_offset]     << 16) |
                                 (g_spi_rx_buf[byte_offset + 1] << 8)  |
                                 (g_spi_rx_buf[byte_offset + 2]);
                
                if (sample & 0x800000) {
                    sample |= 0xFF000000;
                }

                last_valid_ch[ch] = emg_filter_apply(ch, (float)sample);
            }
        }
        else {
            ESP_LOGE(TAG, "Bad SPI packet! Using last valid sample.");
        }

        if (last_mode != raw_data_mode) {
            raw_sample_count = 0;
            nn_sample_count = 0;
            last_mode = raw_data_mode;
        }

        if (raw_data_mode == true) 
        {
            for (int ch = 0; ch < CONFIG_AD1299_NUM_CH; ch++) {
                ble_msg.data.raw.samples[raw_sample_count][ch] = last_valid_ch[ch];
            }
            raw_sample_count++;

            if (raw_sample_count >= RAW_SAMPLES_PER_PACKET) {
                ble_msg.type = BLE_MSG_RAW_DATA;
                ble_msg.data.raw.packet_counter = ble_packet_cnt++;
                
                if (xQueueSend(ble_tx_queue, &ble_msg, 0) != pdTRUE) {
                    // queue full
                }
                raw_sample_count = 0;
            }
        }
        else 
        {
            for (int ch = 0; ch < CONFIG_AD1299_NUM_CH; ch++) {
                step[nn_sample_count].channels[ch] = last_valid_ch[ch];
            }
            nn_sample_count++;

            if (nn_sample_count >= NEW_DATA_STEP) {
                if (xQueueSend(emg_data_queue, step, 0) != pdTRUE) {
                    // queue full
                }
                nn_sample_count = 0; 
            }
        }
    }
}

void app_main()
{
    ESP_LOGI(TAG, "[APP] Startup..") ;
    ESP_LOGI(TAG, "[APP] Free memory: %d bytes", (int)esp_get_free_heap_size()) ;
    ESP_LOGI(TAG, "[APP] IDF version: %s", esp_get_idf_version()) ;

    esp_chip_info_t chip_info ;
    memset(&chip_info,0,sizeof(esp_chip_info_t)) ;
    esp_chip_info(&chip_info) ;
    switch(chip_info.model)
    {
        case CHIP_ESP32:
            ESP_LOGI(TAG, "[APP] Processor model: ESP32") ;
            break ;
        case CHIP_ESP32S2:
            ESP_LOGI(TAG, "[APP] Processor model: ESP32-S2") ;
            break ;
        case CHIP_ESP32S3:
            ESP_LOGI(TAG, "[APP] Processor model: ESP32-S3") ;
            break ;
        case CHIP_ESP32C3:
            ESP_LOGI(TAG, "[APP] Processor model: ESP32-C3") ;
            break ;

        default:
            ESP_LOGI(TAG, "[APP] Processor model: Unknown(%d)",(int)chip_info.model) ;
            break ;
    }
    ESP_LOGI(TAG, "[APP] Processor num cores: %d",(int)chip_info.cores) ;

    init_ble();
    init_1d_cnn();
    emg_filter_init(1000.0f, 50.0f, 10.0f, 0.99f);

    myocell_app_start() ;
}
