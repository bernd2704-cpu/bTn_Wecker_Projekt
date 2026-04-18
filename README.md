# bTn Wecker – Projektstruktur

## Übersicht

ESP32-basierter Wecker mit OLED-Anzeige, DFPlayer Mini (MP3),
kapazitiven Touch-Pads, FreeRTOS und WiFi/NTP.

**Aktuelle Firmware:** 11v00

---

## Ordnerstruktur

```
bTn_Wecker_Projekt/
│
├── Hardware/
│   ├── Schaltplan/        Schaltplan und PCB-Layout
│   ├── Stückliste/        Bauteilliste (BOM)
│   └── Fotos/             Fotos des Aufbaus
│
├── Software/
│   ├── Firmware_aktuell/  Aktuelle Firmware (11v00) + Konfiguration
│   ├── Firmware_Versionshistorie/  Alle Vorgängerversionen
│   └── Bibliotheken/      Verwendete Arduino-Bibliotheken (Hinweise)
│
└── Dokumentation/
    ├── Bedienungsanleitung/  Kurzanleitung für den Endnutzer
    ├── Technisch/            Funktionsreferenz, Changelog
    └── Diagramme/            State-Machine-Diagramme (PPTX + DOCX)
```

---

## Hardware

| Komponente        | Beschreibung                          |
|-------------------|---------------------------------------|
| ESP32 DevKit C V4 | Mikrocontroller, Dual-Core, WiFi      |
| SSD1306 OLED      | 128×64 px, I2C (0x3C, SDA=21, SCL=22) |
| DFPlayer Mini     | MP3-Player, Serial2 (RX=16, TX=17)    |
| Touch T0–T4       | GPIO4, GPIO2, GPIO15, GPIO13          |
| Taster S1–S3      | GPIO32, GPIO33, GPIO0                 |
| Ausgänge E1–E3    | GPIO25 (Kuckuck)                      |
|                   | GPIO26 (Mühlrad)                      |
|                   | GPIO27 (Licht)                        |

---

## Software – Abhängigkeiten

| Bibliothek                | Version   | Quelle             |
|---------------------------|-----------|--------------------|
| ESP32 Arduino Core        | 3.3.8     | Espressif          |
| SSD1306Wire ESP32-ESP8266 | aktuell   | Arduino Library    |
| DFRobotDFPlayerMini       | aktuell   | Arduino Library    |
| FreeRTOS                  | (im Core) | Espressif          |
