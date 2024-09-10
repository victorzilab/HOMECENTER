#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "esp_log.h"
#include "mqtt_client.h"
#include "driver/gpio.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "protocol_examples_common.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "bmx280.h"

// Define your GPIO pin
#define GPIO_OUTPUT_PIN_SEL  ((1ULL<<14))  

// MQTT Topics
#define TOPIC_QOS0 "qos0"
#define TOPIC_QOS1 "qos1"
#define TOPIC_GPIO_STATE "GPIO/gpio_state"
#define SENSOR_PRESSURE "sensor/pressure"
#define SENSOR_HUMIDITY "sensor/humidity"
#define SENSOR_TEMPERATURE "sensor/temperature"

static const char *TAG = "mqttws_example";

static esp_mqtt_client_handle_t mqtt_client;
static int gpio_state = 0; // Track the state of GPIO2

// Function declarations
static void subscribe_to_topics(esp_mqtt_client_handle_t client);
static void publish_gpio_state(esp_mqtt_client_handle_t client);
static void gpio_state_publisher_task(void *pvParameters);
static void getSensorData();
static void publish_sensor_readings(float temp, float pres, float hum); // Function to publish sensor data

// Function to handle MQTT events
static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    ESP_LOGD(TAG, "Event dispatched from event loop base=%s, event_id=%" PRIi32, base, event_id);
    esp_mqtt_event_handle_t event = event_data;
    esp_mqtt_client_handle_t client = event->client;

    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "MQTT_EVENT_CONNECTED");
        subscribe_to_topics(client);
        break;
    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "MQTT_EVENT_DISCONNECTED");
        break;
    case MQTT_EVENT_SUBSCRIBED:
        ESP_LOGI(TAG, "MQTT_EVENT_SUBSCRIBED, msg_id=%d", event->msg_id);
        publish_gpio_state(client); // Publish GPIO state upon successful subscription
        break;
    case MQTT_EVENT_UNSUBSCRIBED:
        ESP_LOGI(TAG, "MQTT_EVENT_UNSUBSCRIBED, msg_id=%d", event->msg_id);
        break;
    case MQTT_EVENT_PUBLISHED:
        ESP_LOGI(TAG, "MQTT_EVENT_PUBLISHED, msg_id=%d", event->msg_id);
        break;
    case MQTT_EVENT_DATA:
        ESP_LOGI(TAG, "MQTT_EVENT_DATA");
        printf("TOPIC=%.*s\r\n", event->topic_len, event->topic);
        printf("DATA=%.*s\r\n", event->data_len, event->data);

        if (strncmp(event->topic, "qos1", event->topic_len) == 0) {
            // Parse the message data
            int new_state = atoi(event->data);
            ESP_LOGI(TAG, "Received new state: %d", new_state);

            // Set GPIO state according to the message
            if (new_state == 0 || new_state == 1) {
                gpio_state = new_state;
                gpio_set_level(14, gpio_state);
                ESP_LOGI(TAG, "Set GPIO2 to %d", gpio_state);
                publish_gpio_state(client); // Publish GPIO state
            } else {
                ESP_LOGW(TAG, "Received invalid state: %.*s", event->data_len, event->data);
            }
        }
        break;
    case MQTT_EVENT_ERROR:
        ESP_LOGI(TAG, "MQTT_EVENT_ERROR");
        // Handle errors as needed
        break;
    default:
        ESP_LOGI(TAG, "Other event id:%d", event->event_id);
        break;
    }
}

// Function to subscribe to MQTT topics
static void subscribe_to_topics(esp_mqtt_client_handle_t client)
{
    int msg_id;
    msg_id = esp_mqtt_client_subscribe(client, TOPIC_QOS0, 0);
    ESP_LOGI(TAG, "Subscribed to %s, msg_id=%d", TOPIC_QOS0, msg_id);

    msg_id = esp_mqtt_client_subscribe(client, TOPIC_QOS1, 1);
    ESP_LOGI(TAG, "Subscribed to %s, msg_id=%d", TOPIC_QOS1, msg_id);
}

// Function to publish GPIO state
static void publish_gpio_state(esp_mqtt_client_handle_t client)
{
    char message[10];
    snprintf(message, sizeof(message), "state:%d", gpio_state);

    int msg_id = esp_mqtt_client_publish(client, TOPIC_GPIO_STATE, message, 0, 1, 0);
    ESP_LOGI(TAG, "Published GPIO state to %s, state=%d, msg_id=%d", TOPIC_GPIO_STATE, gpio_state, msg_id);
}

// Function to publish sensor readings
static void publish_sensor_readings(float temp, float pres, float hum)
{
    char message[50];

    // Publish temperature
    snprintf(message, sizeof(message), "%.2f", temp);
    esp_mqtt_client_publish(mqtt_client, SENSOR_TEMPERATURE, message, 0, 1, 0);
    ESP_LOGI(TAG, "Published temperature to %s: %.2f", SENSOR_TEMPERATURE, temp);

    // Publish pressure
    snprintf(message, sizeof(message), "%.2f", pres);
    esp_mqtt_client_publish(mqtt_client, SENSOR_PRESSURE, message, 0, 1, 0);
    ESP_LOGI(TAG, "Published pressure to %s: %.2f", SENSOR_PRESSURE, pres);

    // Publish humidity
    snprintf(message, sizeof(message), "%.2f", hum);
    esp_mqtt_client_publish(mqtt_client, SENSOR_HUMIDITY, message, 0, 1, 0);
    ESP_LOGI(TAG, "Published humidity to %s: %.2f", SENSOR_HUMIDITY, hum);
}

// Task to get sensor data and publish it
static void getSensorData()
{
    // Entry Point
    i2c_config_t i2c_cfg = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = GPIO_NUM_21,
        .scl_io_num = GPIO_NUM_22,
        .sda_pullup_en = false,
        .scl_pullup_en = false,

        .master = {
            .clk_speed = 100000
        }
    };

    ESP_ERROR_CHECK(i2c_param_config(I2C_NUM_0, &i2c_cfg));
    ESP_ERROR_CHECK(i2c_driver_install(I2C_NUM_0, I2C_MODE_MASTER, 0, 0, 0));

    bmx280_t* bmx280 = bmx280_create(I2C_NUM_0);

    if (!bmx280) { 
        ESP_LOGE("test", "Could not create bmx280 driver.");
        return;
    }

    ESP_ERROR_CHECK(bmx280_init(bmx280));

    bmx280_config_t bmx_cfg = BMX280_DEFAULT_CONFIG;
    ESP_ERROR_CHECK(bmx280_configure(bmx280, &bmx_cfg));
    while (1)
    {
        ESP_ERROR_CHECK(bmx280_setMode(bmx280, BMX280_MODE_FORCE));
        do {
            vTaskDelay(pdMS_TO_TICKS(1));
        } while(bmx280_isSampling(bmx280));

        float temp = 0, pres = 0, hum = 0;
        ESP_ERROR_CHECK(bmx280_readoutFloat(bmx280, &temp, &pres, &hum));

        ESP_LOGI("LogSensor", "Read Values: temp = %f", temp);
        ESP_LOGI("LogSensor", "Read Values: pres = %f", pres);
        ESP_LOGI("LogSensor", "Read Values: hum = %f", hum);

        publish_sensor_readings(temp, pres, hum); // Publish sensor readings

        vTaskDelay(10000 / portTICK_PERIOD_MS); // Delay for 10 seconds
    }
}

// Function to initialize MQTT client and start it
static void mqtt_app_start(void)
{
    const esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = CONFIG_BROKER_URI,
    };

    esp_mqtt_client_handle_t client = esp_mqtt_client_init(&mqtt_cfg);
    esp_mqtt_client_register_event(client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    esp_mqtt_client_start(client);
    mqtt_client = client; // Store the client handle globally
}

// Task to publish GPIO state periodically
static void gpio_state_publisher_task(void *pvParameters)
{
    while (1) {
        publish_gpio_state(mqtt_client);
        vTaskDelay(10000 / portTICK_PERIOD_MS); // Publish every 10 seconds
    }
}

// Main application entry point
void app_main(void)
{
    ESP_LOGI(TAG, "[APP] Startup..");
    ESP_LOGI(TAG, "[APP] Free memory: %" PRIu32 " bytes", esp_get_free_heap_size());
    ESP_LOGI(TAG, "[APP] IDF version: %s", esp_get_idf_version());

    esp_log_level_set("*", ESP_LOG_INFO);
    esp_log_level_set("mqtt_client", ESP_LOG_VERBOSE);
    esp_log_level_set("mqttws_example", ESP_LOG_VERBOSE);

    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // Initialize and configure GPIO
    gpio_config_t io_conf;
    io_conf.intr_type = GPIO_INTR_DISABLE;
    io_conf.mode = GPIO_MODE_OUTPUT;
    io_conf.pin_bit_mask = GPIO_OUTPUT_PIN_SEL;
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
    gpio_config(&io_conf);

    // Initially set GPIO2 to low
    gpio_set_level(14, 0);
    gpio_state = 0; // Track the initial state
    ESP_LOGI(TAG, "GPIO2 initialized to 0");

    // Connect to Wi-Fi and start MQTT
    ESP_ERROR_CHECK(example_connect());
    mqtt_app_start();

    // Create a task to publish GPIO state periodically
    xTaskCreate(getSensorData, "DataAquire", 4096, NULL, 5, NULL);
    xTaskCreate(gpio_state_publisher_task, "gpio_state_publisher_task", 4096, NULL, 5, NULL);
}
