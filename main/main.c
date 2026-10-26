#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "esp_log.h"
#include "esp_mac.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "mqtt_client.h"

#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"

#include "kty.h"
#include "config_store.h"

static const char *TAG = "temp_sensor";

#define SENSOR_GPIO       1       /* ADC1_CH0 beim ESP32-S3 */
#define VCC_MV            3300    /* Versorgung des Spannungsteilers */
#define ADC_OVERSAMPLE    64
#define WIFI_CONNECTED_BIT BIT0

static EventGroupHandle_t s_wifi_events;
static esp_mqtt_client_handle_t s_mqtt;
static volatile bool s_mqtt_connected;

static char s_id[32];
static char s_topic_state[80];
static char s_topic_avail[80];
static char s_topic_config[96];

/* ---------------------------------------------------------------- WLAN */

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "WLAN getrennt, neuer Verbindungsversuch");
        xEventGroupClearBits(s_wifi_events, WIFI_CONNECTED_BIT);
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *ev = data;
        ESP_LOGI(TAG, "IP erhalten: " IPSTR, IP2STR(&ev->ip_info.ip));
        xEventGroupSetBits(s_wifi_events, WIFI_CONNECTED_BIT);
    }
}

static void wifi_start(void)
{
    s_wifi_events = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL));

    wifi_config_t wc = { 0 };
    strlcpy((char *)wc.sta.ssid, g_cfg.wifi_ssid, sizeof(wc.sta.ssid));
    strlcpy((char *)wc.sta.password, g_cfg.wifi_password, sizeof(wc.sta.password));
    wc.sta.threshold.authmode = strlen(g_cfg.wifi_password) ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    ESP_ERROR_CHECK(esp_wifi_start());
}

/* ---------------------------------------------------------------- MQTT */

static void publish_discovery(void)
{
    char payload[640];
    snprintf(payload, sizeof(payload),
             "{\"name\":\"Temperatur\","
             "\"unique_id\":\"%s_temp\","
             "\"device_class\":\"temperature\","
             "\"state_class\":\"measurement\","
             "\"unit_of_measurement\":\"\\u00b0C\","
             "\"state_topic\":\"%s\","
             "\"availability_topic\":\"%s\","
             "\"device\":{\"identifiers\":[\"%s\"],"
             "\"name\":\"%s\",\"model\":\"ESP32-S3 + KTY\","
             "\"manufacturer\":\"DIY\"}}",
             s_id, s_topic_state, s_topic_avail, s_id, g_cfg.device_name);
    esp_mqtt_client_publish(s_mqtt, s_topic_config, payload, 0, 1, 1);
}

static void mqtt_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    switch ((esp_mqtt_event_id_t)id) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "MQTT verbunden");
        s_mqtt_connected = true;
        publish_discovery();
        esp_mqtt_client_publish(s_mqtt, s_topic_avail, "online", 0, 1, 1);
        break;
    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "MQTT getrennt");
        s_mqtt_connected = false;
        break;
    default:
        break;
    }
}

static void mqtt_start(void)
{
    esp_mqtt_client_config_t cfg = {
        .broker.address.uri = g_cfg.mqtt_uri,
        .session.last_will = {
            .topic = s_topic_avail,
            .msg = "offline",
            .qos = 1,
            .retain = 1,
        },
    };
    if (strlen(g_cfg.mqtt_user)) {
        cfg.credentials.username = g_cfg.mqtt_user;
        cfg.credentials.authentication.password = g_cfg.mqtt_password;
    }
    s_mqtt = esp_mqtt_client_init(&cfg);
    esp_mqtt_client_register_event(s_mqtt, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    esp_mqtt_client_start(s_mqtt);
}

/* ----------------------------------------------------------------- ADC */

static adc_oneshot_unit_handle_t s_adc;
static adc_cali_handle_t s_cali;
static adc_channel_t s_chan;

static void adc_start(void)
{
    adc_unit_t unit;
    ESP_ERROR_CHECK(adc_oneshot_io_to_channel(SENSOR_GPIO, &unit, &s_chan));

    adc_oneshot_unit_init_cfg_t ucfg = { .unit_id = unit };
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&ucfg, &s_adc));

    adc_oneshot_chan_cfg_t ccfg = { .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT };
    ESP_ERROR_CHECK(adc_oneshot_config_channel(s_adc, s_chan, &ccfg));

    adc_cali_curve_fitting_config_t cal = {
        .unit_id = unit,
        .chan = s_chan,
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    ESP_ERROR_CHECK(adc_cali_create_scheme_curve_fitting(&cal, &s_cali));
}

/* Gemittelte, kalibrierte Spannung am GPIO in mV */
static int adc_read_mv(void)
{
    int sum = 0;
    for (int i = 0; i < ADC_OVERSAMPLE; i++) {
        int raw = 0;
        ESP_ERROR_CHECK(adc_oneshot_read(s_adc, s_chan, &raw));
        sum += raw;
    }
    int mv = 0;
    ESP_ERROR_CHECK(adc_cali_raw_to_voltage(s_cali, sum / ADC_OVERSAMPLE, &mv));
    return mv;
}

/* ------------------------------------------------------------ main */

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    uint8_t mac[6];
    ESP_ERROR_CHECK(esp_read_mac(mac, ESP_MAC_WIFI_STA));
    snprintf(s_id, sizeof(s_id), "esp32_temp_%02x%02x%02x", mac[3], mac[4], mac[5]);
    snprintf(s_topic_state, sizeof(s_topic_state), "%s/state", s_id);
    snprintf(s_topic_avail, sizeof(s_topic_avail), "%s/status", s_id);
    snprintf(s_topic_config, sizeof(s_topic_config), "homeassistant/sensor/%s/temperature/config", s_id);

    config_store_init();
    adc_start();
    wifi_start();
    xEventGroupWaitBits(s_wifi_events, WIFI_CONNECTED_BIT, pdFALSE, pdTRUE, portMAX_DELAY);
    mqtt_start();

    while (1) {
        const int mv = adc_read_mv();
        float temp_c;

        /* Teiler: 3V3 - Rpullup - Messpunkt - KTY - GND */
        if (mv > 0 && mv < VCC_MV) {
            const float r_kty = (float)g_cfg.pullup_ohms * mv / (float)(VCC_MV - mv);
            if (kty_ohms_to_celsius(r_kty, g_cfg.kty_r25_ohms, &temp_c)) {
                temp_c += g_cfg.offset_c;
                ESP_LOGI(TAG, "U=%d mV, R=%.0f Ohm, T=%.2f C", mv, r_kty, temp_c);
                if (s_mqtt_connected) {
                    char buf[16];
                    snprintf(buf, sizeof(buf), "%.2f", temp_c);
                    esp_mqtt_client_publish(s_mqtt, s_topic_state, buf, 0, 0, 0);
                }
            } else {
                ESP_LOGW(TAG, "R=%.0f Ohm ausserhalb der Kennlinie - Sensorfehler?", r_kty);
            }
        } else {
            ESP_LOGW(TAG, "U=%d mV ungueltig - Sensor nicht angeschlossen?", mv);
        }
        vTaskDelay(pdMS_TO_TICKS(g_cfg.interval_s * 1000));
    }
}
