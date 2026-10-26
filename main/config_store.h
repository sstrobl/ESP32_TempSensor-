#pragma once

#include <stdint.h>

typedef struct {
    char  wifi_ssid[33];
    char  wifi_password[65];
    char  mqtt_uri[128];
    char  mqtt_user[64];
    char  mqtt_password[64];
    char  device_name[48];
    int   interval_s;
    int   pullup_ohms;
    int   kty_r25_ohms;
    float offset_c;
} app_config_t;

extern app_config_t g_cfg;

/**
 * Startet USB (Massenspeicher + serielle Konsole), legt config.txt bei Bedarf
 * mit den Kconfig-Vorgaben an und liest sie in g_cfg ein. Wird das Laufwerk
 * am PC ausgeworfen, startet das Geraet mit der neuen Konfiguration neu.
 */
void config_store_init(void);
