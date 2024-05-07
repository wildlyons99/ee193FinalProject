#include "freertos/FreeRTOS.h"
#include "freertos/task.h" // Used for timer delay
#include "esp_netif.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"


#include "mqtt_client.h"
#include "minimal_wifi.h"
#include "esp_mac.h"

#include "esp_adc/adc_oneshot.h"


#include "esp_sleep.h"
#include "esp_log.h"

// nvs flash support 
#include "nvs_flash.h"
#include "nvs.h"


#define ADC_CHANNEL ADC_CHANNEL_0


#define WIFI_SSID      "Tufts_Wireless"
#define WIFI_PASS      ""

#define BROKER_URI "mqtt://en1-pi.eecs.tufts.edu"


void app_main() {
    // I belive below is outdated
    // 8 = SCLK 
    // 18 = SIO
    // 19 = CS
/* ------------------------------------------
MQTT
------------------------------------------ */
    uint8_t* espmac = 0;
    esp_base_mac_addr_get(espmac);
    // Enable Flash (aka non-volatile storage, NVS)
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
      ESP_ERROR_CHECK(nvs_flash_erase());
      ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Normally we'd need to initialize the event loop and the network stack,
    // but both of these will be done when we connect to WiFi.
    printf("Connecting to WiFi...");
    wifi_connect(WIFI_SSID, WIFI_PASS);

    // Initialize the MQTT client
    // Read the documentation for more information on what you can configure:
    // https://docs.espressif.com/projects/esp-idf/en/latest/esp32c3/api-reference/protocols/mqtt.html
    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = BROKER_URI,
    };
    esp_mqtt_client_handle_t client = esp_mqtt_client_init(&mqtt_cfg);
    esp_mqtt_client_start(client);


/* ------------------------------------------
TMP126 CONFIG!
------------------------------------------ */

    spi_device_handle_t spi;
    spi_bus_config_t buscfg = {
        .miso_io_num = -1,
        .mosi_io_num = 1,
        .sclk_io_num = 2,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        //.max_transfer_sz = PARALLEL_LINES * 320 * 2 + 8
    };

    spi_device_interface_config_t devcfg = {

        .flags = SPI_DEVICE_HALFDUPLEX | SPI_DEVICE_3WIRE,
        .command_bits = 16,
        .clock_speed_hz = 1 * 1000 * 1000,
        .mode = 3,          //SPI mode 0
        .spics_io_num = 8,
        .queue_size = 1,
        //.flags = SPI_DEVICE_HALFDUPLEX | SPI_DEVICE_POSITIVE_CS,
        /* .pre_cb = cs_high,
        .post_cb = cs_low, */
        //.input_delay_ns = EEPROM_INPUT_DELAY_NS, 
    };

    ret = spi_bus_initialize(SPI2_HOST, &buscfg, 0);
    ESP_ERROR_CHECK(ret);
    //Attach the LCD to the SPI bus
    ret = spi_bus_add_device(SPI2_HOST, &devcfg, &spi);
    ESP_ERROR_CHECK(ret);

    u_int8_t rec[4] = { 0 };
    spi_transaction_t t = {
        .rxlength = 16,
        //.flags = SPI_TRANS_USE_RXDATA,     
        //.tx_buffer = ,
        //.cmd = 0b0000000100001100,
        .cmd = 0b0000000100000000,
        .rx_buffer = rec,

    };
    ret = spi_device_polling_transmit(spi, &t);
    ESP_ERROR_CHECK(ret);



/* ------------------------------------------
RUN!!!
------------------------------------------ */
    while(1){

        ret = spi_device_polling_transmit(spi, &t);
        ESP_ERROR_CHECK(ret);
        u_int16_t raw_val = (((u_int16_t) rec[0]) << 8) + rec[1];
        printf("TEMP Result: %d\n", raw_val);

        int16_t shift = raw_val >> 2;
        double result = shift * 0.03125;
        printf("TEMP Result: %f\n", result);
        char c_result[100];

        sprintf(c_result, "%f", result);

        // path format: teamX/nodeY/tempupdate
        // data will be a string with three comma-separated fields: timestamp,temperature,battery
        // Subtopic: teamJ
        // Node 
        // TODO: measure batteyr voltage... 
        esp_mqtt_client_publish(client, "teamJ/node0/tempupdate", c_result, 6, 1, 1);


        // configure ito deep sleep mode for an hour
        // esp_sleep_enable_timer_wakeup(3600e6);

        // do for one minute for testing
        esp_sleep_enable_timer_wakeup(60e6); 

        printf("Entering deep sleep mode\n");
        esp_deep_sleep_start();

        // vTaskDelay(1000);
        //vTaskDelay(1000 / portTICK_PERIOD_MS);
    }

}


#define NUM_TEMPS 24

// Array to store temperature data
double temperatures[NUM_TEMPS];

// for if we want to do flash memory...
void write_temp_to_flash(double temp) {
    // Open NVS
    printf("\n");
    printf("Opening Non-Volatile Storage (NVS) handle... ");
    nvs_handle_t my_handle;
    esp_err_t err = nvs_open("storage", NVS_READWRITE, &my_handle);
    if (err != ESP_OK) {
        printf("Error (%s) opening NVS handle!\n", esp_err_to_name(err));
    } else {
        printf("Done\n");

        // Read current index from NVS
        int index = 0;
        err = nvs_get_i32(my_handle, "index", &index);
        if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
            printf("Error reading index from NVS: %s\n", esp_err_to_name(err));
            nvs_close(my_handle);
            return;
        }

        // Fill array with temperature data until array is full
        while (index < NUM_TEMPS) {
            // read index value
            err = nvs_get_i32(my_handle, "index", &index);
            if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
                printf("Error reading index from NVS: %s\n", esp_err_to_name(err));
                nvs_close(my_handle);
                return;
            }

            temperatures[index] = temp; // Add temperature data to array
            printf("Added temperature %.2f to index %d\n", temp, index);
            index++; // Move to next index

            // Write updated index to NVS
            err = nvs_set_i32(my_handle, "index", index);
            if (err != ESP_OK) {
                printf("Error writing index to NVS: %s\n", esp_err_to_name(err));
                nvs_close(my_handle);
                return;
            }

            // Commit changes to NVS
            err = nvs_commit(my_handle);
            if (err != ESP_OK) {
                printf("Error committing changes to NVS: %s\n", esp_err_to_name(err));
                nvs_close(my_handle);
                return;
            }

            // Write temperature data to NVS
            printf("Writing temperature data to NVS... \n");
            char temp_name[20]; // Generate temperature name dynamically
            snprintf(temp_name, sizeof(temp_name), "temp_%d", index);
            err = nvs_set_blob(my_handle, temp_name, &temperatures[index], sizeof(float));
            if (err != ESP_OK) {
                printf("Error writing temperature %d: %s\n", index, esp_err_to_name(err));
            }

            // do for one minute for testing
            esp_sleep_enable_timer_wakeup(60e6); 

            esp_deep_sleep_start();
        }
        

        // Commit changes
        printf("Committing updates in NVS ... ");
        err = nvs_commit(my_handle);
        printf((err != ESP_OK) ? "Failed!\n" : "Done\n");

        // Close NVS
        nvs_close(my_handle);
    }
}




