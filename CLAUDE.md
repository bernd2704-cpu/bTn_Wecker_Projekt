# bTn Wecker – Projektkontext für Claude Code

## Hardware
ESP32 Dev Kit C V4, SSD1306 OLED 128x64 I2C (0x3C, SDA=21, SCL=22),
DFPlayer Mini (Serial2, RX=16, TX=17), Touch T0=GPIO4/T2=GPIO2/T3=GPIO15/T4=GPIO13,
Taster S1=GPIO32/S2=GPIO33/S3=GPIO0, Ausgänge E1=GPIO25/E2=GPIO26/E3=GPIO27

## Aktuelle Version
Firmware 10v04, Konfiguration SysConf_10v04.h

## Architektur
FreeRTOS, 9 Tasks auf 2 Cores, Arduino IDE / ESP32 Core 3.3.8
Stack-Größen als STACK_*-Konstanten in SysConf_*.h

## Wichtige Regeln
- Niemals vTaskDelay() unter gehaltenem Mutex aufrufen
- Alle Serial-Ausgaben nach WiFi-Connect über webLogf() statt Serial.*
- Versionsnummer bei jeder Änderung inkrementieren
- Stack-Größen nur über STACK_*-Konstanten in SysConf ändern
- SysConf immer mit Versionsnummer benennen (SysConf_Xv0.h)
