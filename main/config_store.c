#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <sys/stat.h>

#include "esp_log.h"
#include "esp_system.h"
#include "esp_partition.h"
#include "wear_levelling.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "tinyusb.h"
#include "tinyusb_default_config.h"
#include "tinyusb_msc.h"
#include "tinyusb_cdc_acm.h"
#include "tinyusb_console.h"

#include "config_store.h"
#include "sdkconfig.h"

static const char *TAG = "config";

#define CFG_BASE_PATH  "/cfg"
#define CFG_FILE_PATH  CFG_BASE_PATH "/config.txt"

app_config_t g_cfg;

static tinyusb_msc_storage_handle_t s_storage_hdl;

static void set_defaults(void)
{
    memset(&g_cfg, 0, sizeof(g_cfg));
    strlcpy(g_cfg.wifi_ssid, CONFIG_WIFI_SSID, sizeof(g_cfg.wifi_ssid));
    strlcpy(g_cfg.wifi_password, CONFIG_WIFI_PASSWORD, sizeof(g_cfg.wifi_password));
    strlcpy(g_cfg.mqtt_uri, CONFIG_MQTT_BROKER_URI, sizeof(g_cfg.mqtt_uri));
    strlcpy(g_cfg.mqtt_user, CONFIG_MQTT_USERNAME, sizeof(g_cfg.mqtt_user));
    strlcpy(g_cfg.mqtt_password, CONFIG_MQTT_PASSWORD, sizeof(g_cfg.mqtt_password));
    strlcpy(g_cfg.device_name, CONFIG_DEVICE_NAME, sizeof(g_cfg.device_name));
    g_cfg.interval_s = CONFIG_SAMPLE_INTERVAL_S;
    g_cfg.pullup_ohms = CONFIG_PULLUP_OHMS;
    g_cfg.kty_r25_ohms = CONFIG_KTY_R25_OHMS;
    g_cfg.offset_c = CONFIG_TEMP_OFFSET_CENTI / 100.0f;
}

static void write_config(void)
{
    ESP_LOGI(TAG, "Erzeuge %s", CFG_FILE_PATH);
    FILE *f = fopen(CFG_FILE_PATH, "w");
    if (!f) {
        ESP_LOGE(TAG, "Konnte %s nicht anlegen", CFG_FILE_PATH);
        return;
    }
    fprintf(f,
            "# ESP32 Temp Sensor - Konfiguration\r\n"
            "# Eine Zeile pro Einstellung: NAME=Wert\r\n"
            "# Nach dem Speichern das Laufwerk am PC auswerfen (Sicher entfernen):\r\n"
            "# das Geraet startet dann automatisch mit den neuen Werten neu.\r\n"
            "\r\n"
            "WIFI_SSID=%s\r\n"
            "WIFI_PASSWORD=%s\r\n"
            "MQTT_URI=%s\r\n"
            "MQTT_USER=%s\r\n"
            "MQTT_PASSWORD=%s\r\n"
            "DEVICE_NAME=%s\r\n"
            "# Messintervall in Sekunden\r\n"
            "INTERVAL_S=%d\r\n"
            "# Vorwiderstand nach 3,3 V in Ohm\r\n"
            "PULLUP_OHMS=%d\r\n"
            "# KTY-Nennwiderstand bei 25 Grad C (1000 = KTY81-1xx, 2000 = KTY81-2xx)\r\n"
            "KTY_R25_OHMS=%d\r\n"
            "# Kalibrier-Offset in Grad C\r\n"
            "OFFSET_C=%.2f\r\n",
            g_cfg.wifi_ssid, g_cfg.wifi_password, g_cfg.mqtt_uri, g_cfg.mqtt_user,
            g_cfg.mqtt_password, g_cfg.device_name, g_cfg.interval_s, g_cfg.pullup_ohms,
            g_cfg.kty_r25_ohms, (double)g_cfg.offset_c);
    fclose(f);
}

static void apply_line(const char *key, const char *val)
{
#define STR(name, field) \
    if (!strcasecmp(key, name)) { strlcpy(g_cfg.field, val, sizeof(g_cfg.field)); return; }
    STR("WIFI_SSID", wifi_ssid)
    STR("WIFI_PASSWORD", wifi_password)
    STR("MQTT_URI", mqtt_uri)
    STR("MQTT_USER", mqtt_user)
    STR("MQTT_PASSWORD", mqtt_password)
    STR("DEVICE_NAME", device_name)
#undef STR
    if (!strcasecmp(key, "INTERVAL_S")) {
        int v = atoi(val);
        if (v >= 1 && v <= 3600) g_cfg.interval_s = v;
    } else if (!strcasecmp(key, "PULLUP_OHMS")) {
        int v = atoi(val);
        if (v > 0) g_cfg.pullup_ohms = v;
    } else if (!strcasecmp(key, "KTY_R25_OHMS")) {
        int v = atoi(val);
        if (v > 0) g_cfg.kty_r25_ohms = v;
    } else if (!strcasecmp(key, "OFFSET_C")) {
        g_cfg.offset_c = strtof(val, NULL);
    } else {
        ESP_LOGW(TAG, "Unbekannte Einstellung '%s'", key);
    }
}

/* Nur aufrufen, solange der Speicher der Anwendung gehoert (MOUNT_APP). */
static void load_config(void)
{
    struct stat st;
    if (stat(CFG_FILE_PATH, &st) != 0) {
        write_config();
    }
    FILE *f = fopen(CFG_FILE_PATH, "r");
    if (!f) {
        ESP_LOGE(TAG, "Konnte %s nicht lesen, verwende Vorgaben", CFG_FILE_PATH);
        return;
    }
    char line[200];
    while (fgets(line, sizeof(line), f)) {
        char *p = line;
        while (isspace((unsigned char)*p)) p++;
        if (*p == '#' || *p == '\0') continue;
        char *eq = strchr(p, '=');
        if (!eq) continue;
        *eq = '\0';
        char *key = p;
        char *val = eq + 1;
        for (char *k = eq - 1; k >= key && isspace((unsigned char)*k); k--) *k = '\0';
        for (char *e = val + strlen(val); e > val && isspace((unsigned char)e[-1]); e--) e[-1] = '\0';
        apply_line(key, val);
    }
    fclose(f);
    ESP_LOGI(TAG, "Konfiguration: SSID='%s' MQTT='%s' Intervall=%ds Pullup=%d R25=%d Offset=%.2f",
             g_cfg.wifi_ssid, g_cfg.mqtt_uri, g_cfg.interval_s, g_cfg.pullup_ohms,
             g_cfg.kty_r25_ohms, (double)g_cfg.offset_c);
}

static void storage_event_cb(tinyusb_msc_storage_handle_t handle, tinyusb_msc_event_t *event, void *arg)
{
    (void)handle; (void)arg;
    if (event->id == TINYUSB_MSC_EVENT_MOUNT_COMPLETE && event->mount_point == TINYUSB_MSC_STORAGE_MOUNT_APP) {
        ESP_LOGI(TAG, "Laufwerk ausgeworfen - Neustart mit neuer Konfiguration");
        vTaskDelay(pdMS_TO_TICKS(300));
        esp_restart();
    }
}

void config_store_init(void)
{
    set_defaults();

    static wl_handle_t wl_handle = WL_INVALID_HANDLE;
    const esp_partition_t *part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_FAT, "storage");
    if (!part || wl_mount(part, &wl_handle) != ESP_OK) {
        ESP_LOGE(TAG, "FAT-Partition 'storage' nicht verfuegbar, verwende Vorgaben");
        return;
    }

    tinyusb_msc_storage_config_t storage_cfg = {
        .mount_point = TINYUSB_MSC_STORAGE_MOUNT_APP,
        .medium.wl_handle = wl_handle,
        .fat_fs = {
            .base_path = CFG_BASE_PATH,
            .config = {
                .format_if_mount_failed = true,
                .max_files = 2,
                .allocation_unit_size = 0,
            },
            .do_not_format = false,
            .format_flags = 0,
        },
    };
    ESP_ERROR_CHECK(tinyusb_msc_new_storage_spiflash(&storage_cfg, &s_storage_hdl));
    ESP_ERROR_CHECK(tinyusb_msc_set_storage_callback(storage_event_cb, NULL));

    load_config();

    /* Laufwerk dem PC anbieten */
    ESP_ERROR_CHECK(tinyusb_msc_set_storage_mount_point(s_storage_hdl, TINYUSB_MSC_STORAGE_MOUNT_USB));

    tinyusb_config_t tusb_cfg = TINYUSB_DEFAULT_CONFIG();
    ESP_ERROR_CHECK(tinyusb_driver_install(&tusb_cfg));

    /* Logs ueber die serielle USB-Schnittstelle ausgeben */
    tinyusb_config_cdcacm_t acm_cfg = { .cdc_port = TINYUSB_CDC_ACM_0 };
    ESP_ERROR_CHECK(tinyusb_cdcacm_init(&acm_cfg));
    ESP_ERROR_CHECK(tinyusb_console_init(TINYUSB_CDC_ACM_0));
}
