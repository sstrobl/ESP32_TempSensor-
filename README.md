# ESP32 Temp Sensor

ESP32-S3 misst mit einem KTY-Sensor die Temperatur und meldet sie per WLAN/MQTT an Home Assistant (ESP-IDF 5.5.5).

## Hardware

```
3V3 --[ 2k4 ]--+-- GPIO1 (ADC1_CH0)
               |
             [KTY]
               |
              GND
```

U = 3,3 V · R_KTY / (2k4 + R_KTY) → R_KTY = 2k4 · U / (3,3 V − U) → Temperatur über Kennlinie (`main/kty.c`, lineare Interpolation).

Der ADC läuft mit 12 dB Dämpfung, kalibriert (Curve Fitting), 64-fach gemittelt.

## Konfiguration

Das Gerät meldet sich am PC als **USB-Laufwerk** mit der Datei `CONFIG.TXT` (FAT-Partition `storage`). Dort stehen WLAN, MQTT-Broker/Login, Messintervall, Vorwiderstand, KTY-Typ (R25 = 1000 für KTY81-1xx, 2000 für KTY81-2xx) und Offset als `NAME=Wert`.

1. Datei bearbeiten und speichern.
2. Laufwerk am PC **auswerfen** – das Gerät startet automatisch mit den neuen Werten neu.

Beim ersten Start wird die Datei mit den Vorgaben aus `idf.py menuconfig` (*Temp Sensor Konfiguration*) angelegt. Danach gilt nur noch die Datei; Änderungen im menuconfig wirken erst, wenn `CONFIG.TXT` gelöscht wird.

Zusätzlich erscheint eine **serielle USB-Schnittstelle** (CDC) mit den Log-Ausgaben.

## Home Assistant

- MQTT-Integration mit Mosquitto-Broker einrichten.
- Das Gerät sendet beim Verbinden eine **MQTT-Discovery**-Nachricht (`homeassistant/sensor/<id>/temperature/config`, retained) – die Entität erscheint automatisch.
- Topics: `<id>/state` (Wert in °C), `<id>/status` (`online`/`offline` per Last Will). `<id>` = `esp32_temp_<letzte 3 MAC-Bytes>`.

## Bauen / Flashen

```
idf.py set-target esp32s3
idf.py menuconfig
idf.py -p COM5 flash
```

> ⚠️ Sobald die Firmware läuft, gehört der native USB-Port TinyUSB (Laufwerk + CDC). Der Port des Bootloaders verschwindet; zum erneuten Flashen das Board in den Download-Modus versetzen: BOOT halten → kurz RESET → BOOT loslassen.

## Hinweise

- Fehlererkennung: Widerstand außerhalb der Kennlinie (Leitungsbruch/Kurzschluss) → keine Veröffentlichung, Warnung im Log.
- Die Kennlinie ist eine Datenblatt-Näherung für KTY81-1xx; bei anderem Typ (KTY84, KTY10 …) die Tabelle in `kty.c` ersetzen.
- Genauigkeit hängt an der Stabilität der 3,3 V-Versorgung und der Toleranz der 2k4 (1 % empfohlen); Feinabgleich über den Offset.
