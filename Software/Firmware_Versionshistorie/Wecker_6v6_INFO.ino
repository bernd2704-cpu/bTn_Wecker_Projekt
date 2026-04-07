// bTn Wecker mit OLED-Anzeige und MP3-Player
// Basis: bTn_Alarm_6v6 – FreeRTOS + State Machine + WiFi-Konfigurator
// Boardverwalter: esp32 3.3.7 von Espressif Systems
//
// ─── State Machines ──────────────────────────────────────────
//
//  UI-State-Machine  (inputTask / displayTask)
//  ┌──────────┐  T0   ┌──────────┐  T0   ┌──────────┐
//  │ UI_CLOCK │──────▶│UI_ALARM1 │──────▶│UI_ALARM2 │
//  └────▲─────┘       └──────────┘       └──────────┘
//    T0 │ S3(off)          T0 ▼               T0 ▼
//  ┌────┴─────┐       ┌──────────┐       ┌──────────┐
//  │ UI_FUNCS │◀──────│UI_SOUND2 │◀──────│UI_SOUND1 │
//  └──────────┘  T0   └──────────┘  T0   └──────────┘
//  T0 (von UI_FUNCS) ──────────────────────▶ UI_CUCKOO_TIME
//  T0 (von UI_CUCKOO_TIME) ──────────────▶ UI_CLOCK
//  S3 (beliebig) ─────────────────────────▶ UI_INFO
//  S3 (von INFO) ──────────────────────────▶ UI_CLOCK
//  T0 (von INFO) ──────────────────────────▶ WiFi-Konfigurator
//
//  Alarm-State-Machine  (alarmTask)
//  ALARM_IDLE ──── Alarmzeit erreicht ──▶ ALARM_RUNNING
//  ALARM_RUNNING ── MP3 beendet (delay6) ─▶ ALARM_IDLE
//
//  Kuckuck-State-Machine  (alarmTask)
//  CUCKOO_IDLE ──── t_min==0, cuckoo_on ──▶ CUCKOO_RUNNING
//  CUCKOO_RUNNING ── delay4 abgelaufen ────▶ CUCKOO_IDLE
//
// ─── Task-Architektur ────────────────────────────────────────
//  touchTask   Core 0  Pri 2  ESP-IDF touch_pad_* → inputQueue
//                             State Machine: TS_IDLE → TS_PRESSED → TS_REPEAT
//                             Exklusiv: nur ein Pad aktiv, andere gesperrt
//  inputTask   Core 1  Pri 2  Dispatch → UI-State-Machine
//  displayTask Core 1  Pri 1  Zeitanzeige, Auto-Rückkehr
//  alarmTask   Core 1  Pri 2  Alarm- + Kuckuck-State-Machine
//  wifiTask    Core 0  Pri 1  WiFi-Reconnect
//  nvrTask     Core 0  Pri 1  Flash-Sicherung bei Änderung
//  stackMonTask Core 0 Pri 1  Stack-Überwachung (Serial)
// ─────────────────────────────────────────────────────────────

const char PGMInfo[] = "bTn_Alarm_6v6_INFO";                                             // Versionsstring – auf OLED + Serial ausgegeben

// ── Bibliotheken ─────────────────────────────────────────────
#include <WiFi.h>                  // ESP32 WiFi-Treiber (STA + AP)
#include <WebServer.h>             // Einfacher HTTP-Server für WiFi-Konfigurator
#include <time.h>                  // POSIX time(), localtime_r(), struct tm
#include <esp_sntp.h>              // ESP-IDF SNTP-Client (esp_sntp_init etc.)
#include <SSD1306Wire.h>           // ThingPulse OLED-Bibliothek (I2C)
#include <DFRobotDFPlayerMini.h>   // DFPlayer Mini MP3-Modul (Serial2)
#include <Preferences.h>           // Arduino-NVS-Wrapper (Key-Value im Flash)
#include <nvs_flash.h>             // ESP-IDF Low-Level NVS (Werksreset: nvs_flash_erase)
#include <driver/touch_pad.h>      // ESP-IDF Touch-Sensor-Treiber
#include <freertos/semphr.h>       // FreeRTOS Semaphore / Mutex API

// ── Konfiguration ────────────────────────────────────────────
#include "SystemConfig.h"          // Pin-Belegung, Timing-Konstanten, Touch-Schwellwerte
#include "WEB.h"                   // PROGMEM HTML-Seiten für WiFi-Konfigurator

// ── WiFi-Laufzeit-Zugangsdaten (aus NVR, ab 4v0) ─────────────
// Werden in loadWifiCredentials() gefüllt und danach in
// WiFi.begin() sowie wifiTask verwendet.
static char sta_ssid[33] = "";     // SSID des Heim-WLAN (max. 32 Zeichen + Nulltermin.)
static char sta_psk [64] = "";     // WPA2-Passwort (max. 63 Zeichen + Nulltermin.)
// ── Touch-Task ───────────────────────────────────────────────

static const touch_pad_t TOUCH_PADS[4] = { // Zuordnung Array-Index → ESP32 Touch-Kanal
  TOUCH_PAD_NUM0,   // T0 – GPIO4
  TOUCH_PAD_NUM2,   // T2 – GPIO2
  TOUCH_PAD_NUM3,   // T3 – GPIO15
  TOUCH_PAD_NUM4    // T4 – GPIO13
};

// ── State-Machine-Typen ──────────────────────────────────────
// Enum-Werte 0–7 entsprechen direkt den menu()-Seitennummern.
enum UiState : uint8_t {
  UI_CLOCK       = 0,   // Seite 0: Zeitanzeige
  UI_ALARM1      = 1,   // Seite 1: Alarm 1 einstellen
  UI_ALARM2      = 2,   // Seite 2: Alarm 2 einstellen
  UI_SOUND1      = 3,   // Seite 3: Sound 1 wählen
  UI_SOUND2      = 4,   // Seite 4: Sound 2 wählen
  UI_FUNCS       = 5,   // Seite 5: Funktionen wählen
  UI_CUCKOO_TIME = 6,   // Seite 6: Kuckuck-Aktivzeit einstellen
  UI_INFO        = 7    // Seite 7: Info (nur via S3 erreichbar)
};

enum AlarmState  : uint8_t { ALARM_IDLE,  ALARM_RUNNING  }; // IDLE: kein Alarm aktiv; RUNNING: MP3 läuft
enum CuckooState : uint8_t { CUCKOO_IDLE, CUCKOO_RUNNING }; // IDLE: wartet; RUNNING: Relais aktiv (delay4)

// Touch-Task State Machine
// TS_IDLE    – kein Touch aktiv
// TS_PRESSED – Touch erkannt, wartet auf HOLD-Schwelle (750 ms)
// TS_REPEAT  – HOLD erreicht, sendet EVT alle TOUCH_REPEAT_MS (250 ms)
enum TouchState  : uint8_t { TS_IDLE, TS_PRESSED, TS_REPEAT }; // Touch-Ablauf: Erstauslösung → Hold → Repeat

volatile UiState     uiState     = UI_CLOCK;  // aktiver Menü-Zustand (inputTask/displayTask)
volatile AlarmState  alarmState  = ALARM_IDLE; // Alarm-State-Machine (alarmTask)
volatile CuckooState cuckooState = CUCKOO_IDLE; // Kuckuck-State-Machine (alarmTask + inputTask S1)

// ── FreeRTOS Objekte ─────────────────────────────────────────
static QueueHandle_t     inputQueue   = nullptr; // Event-Queue: Touch/Taster → inputTask (Tiefe 32)
static SemaphoreHandle_t displayMutex = nullptr;                                         // Mutex: exklusiver Display-Zugriff
static SemaphoreHandle_t playerMutex  = nullptr;                                         // Mutex: exklusiver DFPlayer-Zugriff (thread-safe Serial2)
static SemaphoreHandle_t nvrSemaphore = nullptr; // Binär-Semaphor: signalisiert nvrTask zum Speichern

// Task-Handles – werden in setup() befüllt, von stackMonTask gelesen
static TaskHandle_t hTouchTask   = nullptr; // Handle für stackMonTask-Abfrage
static TaskHandle_t hWifiTask    = nullptr; // Handle für stackMonTask-Abfrage
static TaskHandle_t hNvrTask     = nullptr; // Handle für stackMonTask-Abfrage
static TaskHandle_t hInputTask   = nullptr; // Handle für stackMonTask-Abfrage
static TaskHandle_t hDisplayTask = nullptr; // Handle für stackMonTask-Abfrage
static TaskHandle_t hAlarmTask   = nullptr; // Handle für stackMonTask-Abfrage

// ── Hardware ─────────────────────────────────────────────────
SSD1306Wire         display(0x3C, SDA, SCL, GEOMETRY_128_64); // OLED: I2C-Adresse 0x3C, 128×64 px
DFRobotDFPlayerMini player;  // MP3-Player-Objekt (kommuniziert über Serial2)
Preferences         data;    // NVS-Namespace "varSafe" – Alarm, Sound, Vol, Kuckuck

// ── Zeit / Datum ─────────────────────────────────────────────
time_t  now;         // UNIX-Timestamp (Sekunden seit 1970-01-01)
tm      tm;          // Aufgeschlüsselte Lokalzeit – wird von showTime() beschrieben
char    datum[11];   // Anzeigestring Datum: "TT.MM.JJJJ" + \0
char    zeit[9];     // Anzeigestring Zeit:  "HH:MM:SS" + \0
char    datum_sync[9];  // Datum der letzten NTP-Synchronisation ("JJJJMMTT")
char    zeit_sync[9];   // Uhrzeit der letzten NTP-Synchronisation ("HH:MM:SS")
char    datum_WiFi[9];  // Datum des letzten WiFi-Verbindungsaufbaus
char    zeit_WiFi[9];   // Uhrzeit des letzten WiFi-Verbindungsaufbaus

// NTP-Callback schreibt in tmp-Puffer + setzt Flag.
// displayTask überträgt unter displayMutex in datum_sync/zeit_sync.
// Verhindert Race Condition zwischen SNTP-Task und menu(6).
static char          datum_sync_tmp[9]; // NTP-Callback-Puffer (Datum) – unter Mutex nach datum_sync kopiert
static char          zeit_sync_tmp[9];  // NTP-Callback-Puffer (Zeit)  – unter Mutex nach zeit_sync kopiert
static volatile bool ntpSyncPending = false; // true: NTP-Callback hat neue Daten, displayTask überträgt
volatile uint8_t t_hour;  // aktuelle Stunde (0–23) – von showTime() gesetzt, von alarmTask gelesen
volatile uint8_t t_min;   // aktuelle Minute (0–59)
volatile uint8_t t_sec;   // aktuelle Sekunde (0–59)
volatile uint8_t t_sec_alt; // Vorwert von t_sec – erkennt Sekundenwechsel für partielles Display-Update

// ── Alarm ────────────────────────────────────────────────────
bool    a1_on   = true;   // Alarm 1 aktiv (true = ein)
uint8_t a1_hour = 6;      // Weckstunde Alarm 1 (0–23)
uint8_t a1_min  = 0;      // Weckminute Alarm 1 (0–59)
char    str_a1[6];         // Anzeigestring "HH:MM" + \0 für Alarm 1
bool    a2_on   = true;   // Alarm 2 aktiv
uint8_t a2_hour = 6;      // Weckstunde Alarm 2
uint8_t a2_min  = 0;      // Weckminute Alarm 2
char    str_a2[6];         // Anzeigestring "HH:MM" + \0 für Alarm 2
volatile bool    ax_live = false; // true: Alarm läuft gerade (Ausgänge E2/E3 aktiv)

volatile uint32_t t_start4 = 0;  // Startzeitpunkt Kuckuck-Relais (millis)
         uint32_t lastTouchMs = 0;  // Zeitstempel letzter Touch-Event – Basis für Auto-Rückkehr (delay5=20 s)
volatile uint32_t t_start6 = 0;  // Startzeitpunkt Alarm-Polling-Intervall (millis)
         uint32_t t_start7 = 0;  // Startzeitpunkt WiFi-Reconnect-Intervall (millis, delay7)

// ── Sound ────────────────────────────────────────────────────
bool    sound1_on   = false; // Vorschau-Checkbox Sound 1 (Seite 3)
uint8_t sound1      = 1;    // aktuell ausgewählte Dateinummer Sound 1 (1–mp3Count)
char    str_s1[4];           // Anzeigestring "NNN" Sound 1 Auswahl
uint8_t sound1_play = 1;    // bestätigte Alarm-1-Dateinummer (gespeichert im NVS)
char    str_s1_play[4];      // Anzeigestring bestätigte Sound-1-Dateinummer
bool    sound2_on   = false; // Vorschau-Checkbox Sound 2 (Seite 4)
uint8_t sound2      = 1;    // aktuell ausgewählte Dateinummer Sound 2
char    str_s2[4];           // Anzeigestring "NNN" Sound 2 Auswahl
uint8_t sound2_play = 1;    // bestätigte Alarm-2-Dateinummer
char    str_s2_play[4];      // Anzeigestring bestätigte Sound-2-Dateinummer
uint8_t vol         = 9;    // aktuelle Lautstärke (0–max_vol)
uint8_t max_vol     = 20;   // obere Lautstärkegrenze (DFPlayer: 0–30, hier begrenzt)
char    str_vol[3];          // Anzeigestring "NN" Lautstärke
volatile int16_t playerStatus; // letzter readState()-Rückgabewert: >0 spielt, 0 idle, -1 Standby
int16_t mp3Count    = 0;    // Anzahl MP3-Dateien auf SD-Karte (readFileCounts()-1)
char    str_mp3[4];          // Anzeigestring "NNN" MP3-Anzahl für Info-Seite
uint32_t resetCount = 0;    // Anzahl Neustarts seit letztem Werksreset (aus NVS)
char     str_reset[5];       // Anzeigestring Reset-Zähler "NNNN" + \0

bool       varState;         // NVS-Flag: true = gültige Daten vorhanden, false = Erststart
volatile bool safeChange = false; // true: Einstellung geändert, nvrTask soll speichern

// ── Taster Toggle-Status ─────────────────────────────────────
bool          S2_SW = false;  // Toggle-Zustand S2: true = Licht+Mühlrad ein

// ── Funktion-Vorwahl ─────────────────────────────────────────
bool    cuckoo_on      = false; // Kuckuck-Checkbox (Seite 5) – globaler Kuckuck-Schalter
uint8_t cuckoo_onTime  = 6;             // erste Stunde (von hh), Default 06:00
uint8_t cuckoo_offTime = 22;            // letzte Stunde (bis hh), Default 22:00
char    str_cot[3];                     // "hh" von-Zeit
char    str_coff[3];                    // "hh" bis-Zeit
bool    light_on   = true;   // Licht-Checkbox: E3 bei Alarm einschalten
bool    wheel_on   = false;  // Mühlrad-Checkbox: E2 bei Alarm einschalten

// pageselect: spiegelt (uint8_t)uiState – wird von checkboxAlarm/Sound genutzt
volatile uint8_t pageselect = 0; // Spiegelt (uint8_t)uiState – checkboxAlarm/Sound lesen diesen Wert

// ── Taster-Debounce ──────────────────────────────────────────
static uint32_t lastBtnMs[3] = {}; // Zeitstempel letzter S1/S2/S3-Druck für Software-Entprellung (delay3)



// =============================================================
//  Hilfsfunktionen
// =============================================================

void bTn_info() {
  Serial.println("\n----------------------------------------"); // optische Trennlinie im Serial-Monitor
  Serial.println(PGMInfo);  // Versionsstring ausgeben
}

bool delayFunction(uint32_t lastTime, uint32_t actualDelay) {
  return (millis() - lastTime >= actualDelay); // true wenn actualDelay ms seit lastTime vergangen sind
}

void showTime() {
  time(&now);               // aktuelle UNIX-Zeit lesen
  localtime_r(&now, &tm);   // in Lokalzeit umrechnen (thread-safe re-entrant)
  snprintf(datum, sizeof(datum), "%02u.%02u.%04u", tm.tm_mday, tm.tm_mon + 1, tm.tm_year + 1900); // Datum formatieren
  snprintf(zeit,  sizeof(zeit),  "%02u:%02u:%02u",  tm.tm_hour, tm.tm_min,    tm.tm_sec); // Zeit formatieren
  t_sec  = tm.tm_sec;       // volatile Schnellzugriff für alarmTask/displayTask
  t_min  = tm.tm_min;       // volatile Schnellzugriff
  t_hour = tm.tm_hour;      // volatile Schnellzugriff
}

// NTP-Synchronisations-Callback (wird vom SNTP-Task aufgerufen, nicht vom
// Haupt-Task). Schreibt nur in thread-lokale tmp-Puffer und setzt ein Flag.
// displayTask überträgt die Daten sicher unter displayMutex.
void timeavailable(struct timeval *t) {
  time(&now);
  localtime_r(&now, &tm);
  snprintf(datum_sync_tmp, sizeof(datum_sync_tmp), "%04u%02u%02u", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
  snprintf(zeit_sync_tmp,  sizeof(zeit_sync_tmp),  "%02u:%02u:%02u", tm.tm_hour, tm.tm_min, tm.tm_sec);
  ntpSyncPending = true;    // displayTask überträgt tmp-Puffer sicher unter displayMutex
}



// =============================================================
//  Display-Hilfsfunktionen
//  Unverändert. Alle Aufrufe nur unter displayMutex (außer setup()).
// =============================================================

// Löscht einen Bildschirmbereich (Füllrechteck in Schwarz, dann Farbe zurück auf Weiß)
void cleanTXT(int xPos, int yPos, int dx, int dy) {
  display.setColor(BLACK);  // Füllfarbe auf Schwarz setzen (OLED: Pixel aus)
  display.fillRect(xPos, yPos, dx, dy); // Bereich löschen
  display.setColor(WHITE);  // Zeichenfarbe zurück auf Weiß (OLED: Pixel an)
}

// Schreibt Text in Arial 10pt, zentriert an (xPos, yPos)
void zeigeZ10C(int xPos, int yPos, const char* TXT) {
  display.setTextAlignment(TEXT_ALIGN_CENTER);
  display.setFont(ArialMT_Plain_10);
  display.drawString(xPos, yPos, TXT);
  display.display();
}

// Schreibt Text in Arial 10pt, linksbündig
void zeigeZ10L(int xPos, int yPos, const char* TXT) {
  display.setTextAlignment(TEXT_ALIGN_LEFT);
  display.setFont(ArialMT_Plain_10);
  display.drawString(xPos, yPos, TXT);
  display.display();
}

// Schreibt Text in Arial 10pt, rechtsbündig (xPos = rechter Rand)
void zeigeZ10R(int xPos, int yPos, const char* TXT) {
  display.setTextAlignment(TEXT_ALIGN_RIGHT);
  display.setFont(ArialMT_Plain_10);
  display.drawString(xPos, yPos, TXT);
  display.display();
}

// Schreibt Text in Arial 16pt, zentriert
void zeigeZ16C(int xPos, int yPos, const char* TXT) {
  display.setTextAlignment(TEXT_ALIGN_CENTER);
  display.setFont(ArialMT_Plain_16);
  display.drawString(xPos, yPos, TXT);
  display.display();
}

// Schreibt Text in Arial 16pt, linksbündig
void zeigeZ16L(int xPos, int yPos, const char* TXT) {
  display.setTextAlignment(TEXT_ALIGN_LEFT);
  display.setFont(ArialMT_Plain_16);
  display.drawString(xPos, yPos, TXT);
  display.display();
}

// Alarm-Checkboxen (nutzt pageselect – wird von uiTransition synchron gesetzt)
// Zeichnet Alarm-Checkboxen (gefüllt = ein, Rahmen = aus) passend zur aktuellen Seite (pageselect)
void checkboxAlarm() {
  display.setColor(BLACK);
  display.fillRect(68, 38, 7, 7);
  display.fillRect(68, 55, 7, 7);
  display.display();
  display.setColor(WHITE);
  switch (pageselect) {
    case 0:
      if (a1_on) { display.fillRect(68, 38, 7, 7); } else { display.drawRect(68, 38, 7, 7); }
      if (a2_on) { display.fillRect(68, 55, 7, 7); } else { display.drawRect(68, 55, 7, 7); }
      break;
    case 1:
      if (a1_on) { display.fillRect(68, 38, 7, 7); } else { display.drawRect(68, 38, 7, 7); }
      break;
    case 2:
      if (a2_on) { display.fillRect(68, 55, 7, 7); } else { display.drawRect(68, 55, 7, 7); }
      break;
  }
  display.display();
}

// Zeichnet Sound-Checkboxen; bei Aktivierung wird Sound sofort abgespielt (Vorschau)
void checkboxSound() {
  display.setColor(BLACK);
  display.fillRect(68, 38, 7, 7);
  display.fillRect(68, 55, 7, 7);
  display.display();
  display.setColor(WHITE);
  switch (pageselect) {
    case 3:
      if (sound1_on) {
        display.fillRect(68, 38, 7, 7); display.display();
        sound1_play = sound1;
        if (xSemaphoreTake(playerMutex, pdMS_TO_TICKS(50)) == pdTRUE) {                  // 50 ms < 100 ms displayMutex-Timeout
          player.playFolder(1, sound1_play); // Ordner 01, Datei sound1_play abspielen
          xSemaphoreGive(playerMutex); // Player-Mutex freigeben
        }
      } else {                   // Erststart → Standardwerte in NVS schreiben
        display.drawRect(68, 38, 7, 7); display.display();
        if (xSemaphoreTake(playerMutex, pdMS_TO_TICKS(50)) == pdTRUE) {                  // 50 ms < 100 ms displayMutex-Timeout
          player.stop();
          xSemaphoreGive(playerMutex);
        }
      }
      break;
    case 4:
      if (sound2_on) {
        display.fillRect(68, 55, 7, 7); display.display();
        sound2_play = sound2;
        if (xSemaphoreTake(playerMutex, pdMS_TO_TICKS(50)) == pdTRUE) {                  // 50 ms < 100 ms displayMutex-Timeout
          player.playFolder(1, sound2_play); // Ordner 01, Datei sound2_play
          xSemaphoreGive(playerMutex);
        }
      } else {
        display.drawRect(68, 55, 7, 7); display.display();
        if (xSemaphoreTake(playerMutex, pdMS_TO_TICKS(50)) == pdTRUE) {                  // 50 ms < 100 ms displayMutex-Timeout
          player.stop();
          xSemaphoreGive(playerMutex);
        }
      }
      break;
  }
}

// Zeichnet alle drei Funktions-Checkboxen: Kuckuck (y=21), Licht (y=38), Mühlrad (y=55)
void checkboxFunction() {
  display.setColor(BLACK);
  display.fillRect(35, 21, 7, 7);
  display.fillRect(35, 38, 7, 7);
  display.fillRect(35, 55, 7, 7);
  display.display();
  display.setColor(WHITE);
  if (cuckoo_on) { display.fillRect(35, 21, 7, 7); } else { display.drawRect(35, 21, 7, 7); }
  if (light_on)  { display.fillRect(35, 38, 7, 7); } else { display.drawRect(35, 38, 7, 7); }
  if (wheel_on)  { display.fillRect(35, 55, 7, 7); } else { display.drawRect(35, 55, 7, 7); }
  display.display();
}

void menu(int page) {
  switch (page) {
    case 0:
      display.clear();           // Framebuffer leeren
      zeigeZ16C(64, 0,  zeit);
      zeigeZ10L(1,  17, datum);
      zeigeZ10L(77, 17, "Volume");
      zeigeZ10L(115,17, str_vol);
      zeigeZ16C(30, 32, "Alarm 1");
      zeigeZ16C(30, 49, "Alarm 2");
      zeigeZ16C(105,32, str_a1);
      zeigeZ16C(105,49, str_a2);
      checkboxAlarm();
      break;
    case 1:
      display.clear();
      zeigeZ16C(64, 0,  "Alarm einstellen");
      zeigeZ16C(5,  32, "\x3E");
      zeigeZ16C(15, 32, "\x3E");
      zeigeZ16C(40, 32, "A1");
      zeigeZ16C(40, 49, "A2");
      zeigeZ16C(105,32, str_a1);
      zeigeZ16C(105,49, str_a2);
      checkboxAlarm();
      break;
    case 2:
      cleanTXT(0, 32, 25, 15);
      zeigeZ16C(5,  49, "\x3E");
      zeigeZ16C(15, 49, "\x3E");
      zeigeZ16C(105,49, str_a2);
      checkboxAlarm();
      break;
    case 3:
      display.clear();
      zeigeZ16C(64, 0,  "Sound wählen");
      zeigeZ10L(0,  17, "Alarm 1:");
      zeigeZ10L(66, 17, "Alarm 2:");
      zeigeZ10L(42, 17, str_s1_play);
      zeigeZ10L(108,17, str_s2_play);
      zeigeZ16C(5,  32, "\x3E");
      zeigeZ16C(15, 32, "\x3E");
      zeigeZ16C(40, 32, "A1");
      zeigeZ16C(40, 49, "A2");
      zeigeZ16C(105,32, str_s1);
      zeigeZ16C(105,49, str_s2);
      sound1_on = false;
      sound2_on = false;
      checkboxSound();
      break;
    case 4:
      cleanTXT(108, 17, 20, 10);
      zeigeZ10L(108, 17, str_s2_play);
      cleanTXT(0,   32, 25, 15);
      zeigeZ16C(5,  49, "\x3E");
      zeigeZ16C(15, 49, "\x3E");
      zeigeZ16C(105,49, str_s2);
      checkboxSound();
      break;
    case 5:
      if (xSemaphoreTake(playerMutex, pdMS_TO_TICKS(50)) == pdTRUE) {                   // 50 ms < 100 ms displayMutex-Timeout
        player.stop();
        xSemaphoreGive(playerMutex);
      }
      display.clear();
      zeigeZ10C(64, 0,  "Funktion wählen");
      zeigeZ16C(5,  15, "\x3E");
      zeigeZ16C(15, 15, "\x3E");
      zeigeZ16L(50, 15, "Kuckuck");
      zeigeZ16C(5,  32, "\x3E");
      zeigeZ16C(15, 32, "\x3E");
      zeigeZ16L(50, 32, "Licht");
      zeigeZ16C(5,  49, "\x3E");
      zeigeZ16C(15, 49, "\x3E");
      zeigeZ16L(50, 49, "Mühlrad");
      checkboxFunction();
      break;
case 6:
      display.clear();
      zeigeZ16C(64,  0, "Kuckuck aktiv");
      zeigeZ10L(1,  36, "Taste +");
      zeigeZ16C(60, 32, "von");
      zeigeZ16C(91, 32, str_cot);
      zeigeZ16L(100, 32, ":00");
      zeigeZ10L(1,  52, "Taste -");
      zeigeZ16C(60, 49, "bis");
      zeigeZ16C(91, 49, str_coff);
      zeigeZ16L(100, 49, ":00");
      break;
    case 7:
      display.clear();
      zeigeZ10C(63, 0, PGMInfo);
      zeigeZ10L(1,  16, "WiFi");
      zeigeZ10L(28, 16, datum_WiFi);
      zeigeZ10L(82, 16, zeit_WiFi);
      zeigeZ10L(1,  28, "NTP");
      zeigeZ10L(28, 28, datum_sync);
      zeigeZ10L(82, 28, zeit_sync);
      zeigeZ10L(1,  40, "MP3");
      zeigeZ10L(28, 40, str_mp3);
      zeigeZ10C(78,  40, "RESET");
      zeigeZ10R(127, 40, str_reset);                                                     // rechtsbündig
      zeigeZ10L(1,  54, "WLAN");
      zeigeZ10R(127, 54, sta_ssid);         // aktive SSID anzeigen
      // Hinweis: T0 drücken startet WiFi-Konfigurator
      // (wird unterhalb der Box angezeigt, blinkt nicht – statisch)
      // T4 löst Werksreset aus (NVS löschen + Neustart)
      break;
  }
}



// =============================================================
//  NVR (Flash-Persistenz)  –  unverändert
// =============================================================

// Schreibt alle einstellbaren Werte in den NVS-Namespace "varSafe"
void writeNVR() {
  data.putBool("a1_on",       a1_on);
  data.putInt ("a1_hour",     a1_hour);
  data.putInt ("a1_min",      a1_min);
  data.putBool("a2_on",       a2_on);
  data.putInt ("a2_hour",     a2_hour);
  data.putInt ("a2_min",      a2_min);
  data.putInt ("sound1_play", sound1_play);
  data.putInt ("sound2_play", sound2_play);
  data.putBool("cuckoo_on",   cuckoo_on);
  data.putInt ("cuckoo_on_h",  cuckoo_onTime);
  data.putInt ("cuckoo_off_h", cuckoo_offTime);
  data.putBool("light_on",    light_on);
  data.putBool("wheel_on",    wheel_on);
  data.putInt ("vol",         vol);
}

// Liest alle Werte aus dem NVS; behält Standardwert wenn Schlüssel nicht vorhanden
void readNVR() {
  a1_on       = data.getBool("a1_on",       a1_on);
  a1_hour     = data.getInt ("a1_hour",      a1_hour);
  a1_min      = data.getInt ("a1_min",       a1_min);
  a2_on       = data.getBool("a2_on",        a2_on);
  a2_hour     = data.getInt ("a2_hour",      a2_hour);
  a2_min      = data.getInt ("a2_min",       a2_min);
  sound1_play = data.getInt ("sound1_play",  sound1_play);
  sound2_play = data.getInt ("sound2_play",  sound2_play);
  cuckoo_on      = data.getBool("cuckoo_on",    cuckoo_on);
  cuckoo_onTime  = data.getInt ("cuckoo_on_h",  cuckoo_onTime);
  cuckoo_offTime = data.getInt ("cuckoo_off_h", cuckoo_offTime);
  light_on    = data.getBool("light_on",     light_on);
  wheel_on    = data.getBool("wheel_on",     wheel_on);
  vol         = data.getInt ("vol",          vol);
  if (vol > max_vol) vol = max_vol;  // Sicherheitsclamp: verhindert überhöhte Lautstärke nach Parameteränderung
  resetCount  = data.getUInt("resetCount",   0);
  resetCount++;              // bei jedem Start inkrementieren (zählt auch ungewollte Resets)
  data.putUInt("resetCount", resetCount); // sofort zurückschreiben (vor Task-Start, kein Race möglich)
}



// =============================================================
//  WiFi-Konfigurator  (ab 4v0)
//
//  loadWifiCredentials()
//    Liest SSID und PSK aus NVR-Namespace "wifiCfg".
//    Gibt true zurück wenn gültige Daten vorliegen (SSID ≥ 1 Zeichen).
//
//  runWifiConfigServer()
//    Wird beim ersten Start (kein "wifiCfg"-Eintrag) oder auf
//    Anforderung (T0 auf Info-Seite) aufgerufen – VOR dem Start
//    der FreeRTOS-Tasks, da er die Arduino-loop-Ebene blockiert.
//    ESP32 öffnet Access Point WIFI_AP_SSID ("bTn-Wecker"),
//    startet WebServer auf Port 80.
//    Nach erfolgreicher Eingabe: SSID+PSK in NVR speichern → ESP.restart().
// =============================================================

// ── NVR-Zugangsdaten laden ───────────────────────────────────
// Gibt true zurück wenn SSID vorhanden (min. 1 Zeichen).
// Lädt SSID + PSK aus NVS-Namespace "wifiCfg"; gibt true zurück wenn gültig
bool loadWifiCredentials() {
  Preferences wifiPrefs;    // separater Namespace von "varSafe" – kein gegenseitiges Überschreiben
  wifiPrefs.begin("wifiCfg", ReadOnly); // Namespace schreibgeschützt öffnen
  bool valid = wifiPrefs.getBool("valid", false); // Schlüssel fehlt beim Erststart → false
  if (valid) {               // nur lesen wenn Daten als gültig markiert wurden
    wifiPrefs.getString("ssid", sta_ssid, sizeof(sta_ssid)); // SSID in statischen Puffer lesen
    wifiPrefs.getString("psk",  sta_psk,  sizeof(sta_psk));  // PSK in statischen Puffer lesen
  }
  wifiPrefs.end();           // NVS-Handle freigeben
  return valid && (strlen(sta_ssid) > 0); // leer = ungültig, auch wenn Flag gesetzt
}

// ── NVR-Zugangsdaten löschen (erzwingt Config-Mode beim nächsten Start) ──
// Setzt das "valid"-Flag zurück → nächster Start startet WiFi-Konfigurator
void clearWifiCredentials() {
  Preferences wifiPrefs;
  wifiPrefs.begin("wifiCfg", ReadWrite);
  wifiPrefs.putBool("valid", false); // Flag löschen – SSID/PSK bleiben erhalten aber werden ignoriert
  wifiPrefs.end();
}

// ── Serverseitige Validierung ────────────────────────────────
// SSID: 1–32 Zeichen; PSK: leer (offen) oder 8–63 Zeichen.
// Gibt leeren String bei Erfolg, sonst Fehlermeldung zurück.
// Prüft SSID (1–32 Zeichen) und PSK (leer oder 8–63 Zeichen); gibt "" bei Erfolg zurück
static String validateWifiInput(const String& ssid, const String& psk) {
  if (ssid.length() < 1 || ssid.length() > 32)
    return "SSID ungueltig: 1-32 Zeichen erforderlich."; // WPA2-Spec: SSID max. 32 Bytes
  if (psk.length() > 0 && psk.length() < 8)
    return "Passwort ungueltig: Leer lassen oder 8-63 Zeichen."; // WPA2 min. 8 Zeichen
  if (psk.length() > 63)
    return "Passwort zu lang (max. 63 Zeichen)."; // WPA2-Spec: PSK max. 63 ASCII-Zeichen
  return ""; // kein Fehler
}

// ── WiFi-Konfigurations-Webserver ───────────────────────────
// Blockiert bis Daten gespeichert wurden, dann ESP.restart().
void runWifiConfigServer() {
  Serial.println("\n[WiFi-Config] Starte Access Point: " WIFI_AP_SSID);

  // OLED: Hinweis anzeigen
  display.clear();
  zeigeZ10C(64,  2, PGMInfo);
  zeigeZ10C(64, 16, "WiFi Einrichtung");
  zeigeZ10C(64, 28, "WLAN: " WIFI_AP_SSID);
  zeigeZ10C(64, 40, "Browser oeffnen:");
  zeigeZ10C(64, 52, "192.168.4.1");
  display.display();

  // Access Point starten (kein Passwort → offenes AP-Netz)
  WiFi.mode(WIFI_AP);        // Access-Point-Modus (kein STA)
  WiFi.softAP(WIFI_AP_SSID, nullptr, WIFI_AP_CHANNEL); // AP ohne Passwort, Kanal aus SystemConfig.h
  Serial.print("[WiFi-Config] AP-IP: ");
  Serial.println(WiFi.softAPIP());

  WebServer server(80);      // HTTP-Server auf Port 80

  // GET / → Konfigurationsseite (aus WEB.h, PROGMEM)
  server.on("/", HTTP_GET, [&server]() {
    server.send_P(200, "text/html", WIFI_CONFIG_PAGE); // HTML aus PROGMEM senden
  });

  // POST /save → Daten prüfen und speichern
  server.on("/save", HTTP_POST, [&server]() {
    String ssid = server.arg("ssid");
    String psk  = server.arg("psk");
    ssid.trim();               // führende/nachfolgende Leerzeichen entfernen
    // PSK NICHT trimmen: Passwörter können Leerzeichen enthalten

    // Serverseitige Validierung
    String err = validateWifiInput(ssid, psk);
    if (err.length() > 0) {
      Serial.println("[WiFi-Config] Validierungsfehler: " + err);
      server.send(200, "text/html", wifiErrorRedirect(err.c_str()));
      return;
    }

    // In NVR speichern
    Preferences wifiPrefs;
    wifiPrefs.begin("wifiCfg", ReadWrite);
    wifiPrefs.putString("ssid", ssid);
    wifiPrefs.putString("psk",  psk);
    wifiPrefs.putBool  ("valid", true); // Daten als gültig markieren
    wifiPrefs.end();

    Serial.println("[WiFi-Config] Gespeichert: SSID=" + ssid);

    // Erfolgsseite senden (aus WEB.h, PROGMEM)
    server.send_P(200, "text/html", WIFI_SUCCESS_PAGE);

    // Kurze Pause damit Browser die Seite empfangen kann
    delay(3500);               // warten bis Browser Erfolgsseite empfangen hat

    // OLED: Neustart-Meldung
    display.clear();
    zeigeZ16C(64, 22, "Gespeichert!");
    zeigeZ16C(64, 42, "Neustart ...");
    display.display();
    delay(500);                // kurze Pause für OLED-Anzeige
    ESP.restart();             // Neustart → STA-Modus mit neuen Credentials
  });

  // Alle anderen Pfade → zurück zur Hauptseite
  server.onNotFound([&server]() {
    server.sendHeader("Location", "/", true);
    server.send(302, "text/plain", ""); // Redirect zurück zur Hauptseite
  });

  server.begin();            // HTTP-Server starten
  Serial.println("[WiFi-Config] HTTP-Server gestartet, warte auf Eingabe ...");

  // Blockiere bis POST /save verarbeitet wurde
  while (true) {
    server.handleClient();   // eingehende HTTP-Requests verarbeiten
    delay(5);                 // kurze Pause – verhindert WDT-Reset im AP-Modus
  }
}



// =============================================================
//  UI-State-Machine
//
//  uiTransition(next)  –  Zustandswechsel + Bildschirm zeichnen
//  uiDispatch(evt)     –  Event → richtiger State-Handler
//
//  Globale Events (T0, S1, S2, S3) werden vor dem per-State-
//  Handler behandelt. T2/T3/T4 sind State-spezifisch.
// =============================================================

// ── Zustandswechsel ──────────────────────────────────────────
// Muss unter displayMutex aufgerufen werden.
void uiTransition(UiState next) {
  uiState    = next;         // neuen Zustand übernehmen
  pageselect = (uint8_t)next; // pageselect synchron halten – checkboxAlarm/Sound lesen diesen Wert
  menu(pageselect);          // Entry-Aktion: Bildschirm für neuen Zustand zeichnen
}

// ── Per-State-Handler: T2/T3/T4 ─────────────────────────────
// Jeder Handler gibt den Folge-Zustand zurück.
// Bleibt der Zustand gleich, wird uiTransition NICHT aufgerufen
// (kein unnötiges Neuzeichnen; nur partielle Display-Updates).

// — UI_CLOCK: Lautstärke mit T3/T4 ──────────────────────────
static UiState onClock(uint8_t evt) {
  switch (evt) {
    case EVT_T3:                                                                         // Lautstärke +
      if (vol < max_vol) {
        vol++;
        if (xSemaphoreTake(playerMutex, pdMS_TO_TICKS(50)) == pdTRUE) {                 // 50 ms < 100 ms displayMutex-Timeout
          player.volume(vol);      // Lautstärke aus NVS setzen
          xSemaphoreGive(playerMutex);
        }
        snprintf(str_vol, sizeof(str_vol), "%02u", vol); // Anzeigestring aktualisieren
        cleanTXT(115, 17, 15, 10); // alten Lautstärkewert löschen
        zeigeZ10L(115, 17, str_vol);
        safeChange = true;  // NVS-Speicherung anfordern
      }
      break;
    case EVT_T4:                                                                         // Lautstärke –
      if (vol > 0) {
        vol--;
        if (xSemaphoreTake(playerMutex, pdMS_TO_TICKS(50)) == pdTRUE) {                 // 50 ms < 100 ms displayMutex-Timeout
          player.volume(vol);
          xSemaphoreGive(playerMutex);
        }
        snprintf(str_vol, sizeof(str_vol), "%02u", vol);
        cleanTXT(115, 17, 15, 10);
        zeigeZ10L(115, 17, str_vol);
        safeChange = true;
      }
      break;
  }
  return UI_CLOCK;           // Zustand bleibt – kein uiTransition-Aufruf
}

// — UI_ALARM1: Alarm 1 Ein/Aus, Stunde+, Minute+ ─────────────
static UiState onAlarm1(uint8_t evt) {
  switch (evt) {
    case EVT_T2:
      a1_on = !a1_on;        // Alarm 1 umschalten
      checkboxAlarm();
      safeChange = true;
      break;
    case EVT_T3:                                                                         // Stunde +
      if (a1_hour < 23) { a1_hour++; } else { a1_hour = 0; } // Überlauf 23→0
      snprintf(str_a1, sizeof(str_a1), "%02u:%02u", a1_hour, a1_min);
      cleanTXT(82, 34, 46, 13);
      zeigeZ16C(105, 32, str_a1);
      safeChange = true;
      break;
    case EVT_T4:                                                                         // Minute +
      if (a1_min < 59) { a1_min++; } else { a1_min = 0; }   // Überlauf 59→0
      snprintf(str_a1, sizeof(str_a1), "%02u:%02u", a1_hour, a1_min);
      cleanTXT(82, 34, 46, 13);
      zeigeZ16C(105, 32, str_a1);
      safeChange = true;
      break;
  }
  return UI_ALARM1;          // Zustand bleibt
}

// — UI_ALARM2: Alarm 2 Ein/Aus, Stunde+, Minute+ ─────────────
static UiState onAlarm2(uint8_t evt) {
  switch (evt) {
    case EVT_T2:
      a2_on = !a2_on;        // Alarm 2 umschalten
      checkboxAlarm();
      safeChange = true;
      break;
    case EVT_T3:                                                                         // Stunde +
      if (a2_hour < 23) { a2_hour++; } else { a2_hour = 0; } // Überlauf 23→0
      snprintf(str_a2, sizeof(str_a2), "%02u:%02u", a2_hour, a2_min);
      cleanTXT(82, 51, 46, 13);                                                          // A2-Zeile (Y=49) – war fälschlich 34 (A1-Zeile)
      zeigeZ16C(105, 49, str_a2);
      safeChange = true;
      break;
    case EVT_T4:                                                                         // Minute +
      if (a2_min < 59) { a2_min++; } else { a2_min = 0; }   // Überlauf 59→0
      snprintf(str_a2, sizeof(str_a2), "%02u:%02u", a2_hour, a2_min);
      cleanTXT(82, 51, 46, 13);
      zeigeZ16C(105, 49, str_a2);
      safeChange = true;
      break;
  }
  return UI_ALARM2;          // Zustand bleibt
}

// — UI_SOUND1: Sound 1 OK/+/– ────────────────────────────────
static UiState onSound1(uint8_t evt) {
  switch (evt) {
    case EVT_T2:                                                                         // Sound 1 OK (Vorschau)
      sound1_on = !sound1_on; // Vorschau-Checkbox toggeln
      checkboxSound();
      if (sound1_on) {
        snprintf(str_s1_play, sizeof(str_s1_play), "%03u", sound1);
        cleanTXT(42, 17, 20, 10);
        zeigeZ10L(42, 17, str_s1_play);
      }
      break;
    case EVT_T3:                                                                         // Sound 1 +
      if (sound1 < mp3Count) { sound1++; } else { sound1 = 1; } // Überlauf auf 1
      snprintf(str_s1, sizeof(str_s1), "%03u", sound1);
      cleanTXT(82, 34, 46, 13);
      zeigeZ16C(105, 32, str_s1);
      sound1_on = false;
      checkboxSound();
      safeChange = true;
      break;
    case EVT_T4:                                                                         // Sound 1 –
      if (sound1 > 1) { sound1--; } else { sound1 = mp3Count; } // Unterlauf auf mp3Count
      snprintf(str_s1, sizeof(str_s1), "%03u", sound1);
      cleanTXT(82, 34, 46, 13);
      zeigeZ16C(105, 32, str_s1);
      sound1_on = false;
      checkboxSound();
      safeChange = true;
      break;
  }
  return UI_SOUND1;          // Zustand bleibt
}

// — UI_SOUND2: Sound 2 OK/+/– ────────────────────────────────
static UiState onSound2(uint8_t evt) {
  switch (evt) {
    case EVT_T2:                                                                         // Sound 2 OK (Vorschau)
      sound2_on = !sound2_on; // Vorschau-Checkbox toggeln
      checkboxSound();
      if (sound2_on) {
        snprintf(str_s2_play, sizeof(str_s2_play), "%03u", sound2);
        cleanTXT(108, 17, 20, 10);
        zeigeZ10L(108, 17, str_s2_play);
      }
      break;
    case EVT_T3:                                                                         // Sound 2 +
      if (sound2 < mp3Count) { sound2++; } else { sound2 = 1; } // Überlauf auf 1
      snprintf(str_s2, sizeof(str_s2), "%03u", sound2);
      cleanTXT(82, 51, 46, 13);                                                          // S2-Zeile (Y=49)
      zeigeZ16C(105, 49, str_s2);                                                        // S2-Zeile – war fälschlich 32 (S1-Zeile)
      sound2_on = false;
      checkboxSound();
      safeChange = true;
      break;
    case EVT_T4:                                                                         // Sound 2 –
      if (sound2 > 1) { sound2--; } else { sound2 = mp3Count; } // Unterlauf auf mp3Count
      snprintf(str_s2, sizeof(str_s2), "%03u", sound2);
      cleanTXT(82, 51, 46, 13);
      zeigeZ16C(105, 49, str_s2);
      sound2_on = false;
      checkboxSound();
      safeChange = true;
      break;
  }
  return UI_SOUND2;          // Zustand bleibt
}

// — UI_FUNCS: Kuckuck / Licht / Mühlrad toggle ───────────────
static UiState onFuncs(uint8_t evt) {
  switch (evt) {
    case EVT_T2:
      cuckoo_on = !cuckoo_on; // Kuckuck-Checkbox toggeln
      checkboxFunction();
      safeChange = true;
      break;
    case EVT_T3:
      light_on = !light_on;  // Licht-Checkbox toggeln
      checkboxFunction();
      safeChange = true;
      break;
    case EVT_T4:
      wheel_on = !wheel_on;  // Mühlrad-Checkbox toggeln
      checkboxFunction();
      safeChange = true;
      break;
  }
  return UI_FUNCS;           // Zustand bleibt
}

// — UI_CUCKOO_TIME: T3 → von hh +, T4 → bis hh + ───────────
static UiState onCuckooTime(uint8_t evt) {
  switch (evt) {
    case EVT_T3:                                                                         // von hh +
      if (cuckoo_onTime < 23) { cuckoo_onTime++; } else { cuckoo_onTime = 0; } // Von-Stunde, Überlauf 23→0
      snprintf(str_cot, sizeof(str_cot), "%02u", cuckoo_onTime);
      cleanTXT(78, 35, 23, 13);
      zeigeZ16C(91, 32, str_cot);
      safeChange = true;
      break;
    case EVT_T4:                                                                         // bis hh +
      if (cuckoo_offTime < 23) { cuckoo_offTime++; } else { cuckoo_offTime = 0; } // Bis-Stunde, Überlauf 23→0
      snprintf(str_coff, sizeof(str_coff), "%02u", cuckoo_offTime);
      cleanTXT(78, 52, 23, 13);
      zeigeZ16C(91, 49, str_coff);
      safeChange = true;
      break;
  }
  return UI_CUCKOO_TIME;     // Zustand bleibt
}

// Flag: wird von onInfo() gesetzt; inputTask wertet es NACH xSemaphoreGive aus.
// So liegt kein delay() unter displayMutex.
static volatile bool wifiConfigRequested  = false;
static volatile bool factoryResetRequested = false;  // T4 auf UI_INFO → NVS löschen + Neustart

// — UI_INFO: T0 → WiFi-Konfig-Modus; T4 → Werksreset; andere ignoriert ──
static UiState onInfo(uint8_t evt) {
  if (evt == EVT_T0) {
    clearWifiCredentials();                                                               // NVR-Flag löschen
    wifiConfigRequested = true;                                                          // inputTask führt Neustart durch
  }
  if (evt == EVT_T4) {
    factoryResetRequested = true;                                                        // inputTask: NVS löschen + Neustart
  }
  return UI_INFO;            // Info-Seite bleibt – nur S3 (Toggle) oder ESP.restart verlässt sie
}

// ── Haupt-Dispatcher ─────────────────────────────────────────
// Behandelt globale Events, dann per-State-Handler.
// Gibt den Folge-Zustand zurück; Aufrufer ruft uiTransition wenn nötig.
static UiState uiDispatch(UiState s, uint8_t evt) {

  // ── T0: Seitenwechsel (globaler Zyklus, State-unabhängig) ──
  // Ausnahme: T0 auf UI_INFO wird von onInfo() behandelt (→ WiFi-Konfig)
  if (evt == EVT_T0 && s != UI_INFO) {
    static const UiState cycle[] = {
      UI_ALARM1,       // von UI_CLOCK
      UI_ALARM2,       // von UI_ALARM1
      UI_SOUND1,       // von UI_ALARM2
      UI_SOUND2,       // von UI_SOUND1
      UI_FUNCS,        // von UI_SOUND2
      UI_CUCKOO_TIME,  // von UI_FUNCS
      UI_CLOCK,        // von UI_CUCKOO_TIME
    };
    return cycle[(uint8_t)s]; // Nächster Zustand im Zyklus (Enum-Wert = Array-Index)
  }

  // ── S3: Info-Seite ein/aus (global, mit Taster-Entprellung) ─
  if (evt == EVT_S3) {
    uint32_t t_now = millis(); // Entprell-Zeitstempel
    if (t_now - lastBtnMs[2] >= delay3) {
      lastBtnMs[2] = t_now;
      return (s == UI_INFO) ? UI_CLOCK : UI_INFO; // Info-Seite ein/aus toggeln
    }
    return s;                 // S3 zu früh gedrückt – Entprellung aktiv, Zustand unverändert
  }

  // ── T2/T3/T4: State-spezifisch ──────────────────────────────
  switch (s) {
    case UI_CLOCK:  return onClock (evt);
    case UI_ALARM1: return onAlarm1(evt);
    case UI_ALARM2: return onAlarm2(evt);
    case UI_SOUND1: return onSound1(evt);
    case UI_SOUND2: return onSound2(evt);
    case UI_FUNCS:       return onFuncs      (evt);
    case UI_CUCKOO_TIME: return onCuckooTime(evt);
    case UI_INFO:        return onInfo      (evt);
    default:        return s; // unbekannter Zustand – kein Übergang
  }
}



// =============================================================
//  System-State-Machines  (werden von alarmTask aufgerufen)
// =============================================================

static uint8_t lastA1Min = 0xFF;  // Alarm-1-Minutensperre: verhindert Mehrfachauslösung im 500ms-Takt
                                  // 0xFF = nicht ausgelöst; bei Auslösung = t_min gesetzt
static uint8_t lastA2Min = 0xFF;  // Alarm-2-Minutensperre (analog lastA1Min)

// ── Alarm-State-Machine ──────────────────────────────────────
static void runAlarmMachine() {
  switch (alarmState) {

    case ALARM_IDLE:
      // Alarm 1 prüfen
      if (a1_on && t_sec == 0 && t_min == a1_min && t_hour == a1_hour
          && t_min != lastA1Min) {                                                        // nicht dieselbe Minute wiederholen
        lastA1Min = t_min;    // Sperre setzen: dieselbe Minute löst nicht nochmal aus
        if (xSemaphoreTake(playerMutex, pdMS_TO_TICKS(200)) == pdTRUE) {
          player.playFolder(1, sound1_play);
          xSemaphoreGive(playerMutex);
        }
        if (wheel_on) { digitalWrite(E2, HIGH); } // Mühlrad-Relais bei Alarm einschalten
        if (light_on) { digitalWrite(E3, HIGH); } // Licht-Relais bei Alarm einschalten
        t_start6   = millis(); // Polling-Timer starten
        ax_live    = true;     // Alarm läuft – S1 kann abbrechen
        alarmState = ALARM_RUNNING;                                                      // → ALARM_RUNNING
      }
      // Alarm 2 prüfen (else if → Alarm 1 hat Vorrang bei gleicher Zeit)
      else if (a2_on && t_sec == 0 && t_min == a2_min && t_hour == a2_hour
          && t_min != lastA2Min) {                                                        // nicht dieselbe Minute wiederholen
        lastA2Min = t_min;    // Sperre Alarm 2 setzen
        if (xSemaphoreTake(playerMutex, pdMS_TO_TICKS(200)) == pdTRUE) {
          player.playFolder(1, sound2_play);
          xSemaphoreGive(playerMutex);
        }
        if (wheel_on) { digitalWrite(E2, HIGH); }
        if (light_on) { digitalWrite(E3, HIGH); }
        t_start6   = millis();
        ax_live    = true;
        alarmState = ALARM_RUNNING;                                                      // → ALARM_RUNNING
      }
      break;

    case ALARM_RUNNING:
      if (delayFunction(t_start6, delay6)) {
        int16_t st = -1;
        if (xSemaphoreTake(playerMutex, pdMS_TO_TICKS(200)) == pdTRUE) {
          st = player.readState(); // Player-Status abfragen (1=spielt, 0=idle, -1=Standby)
          st = player.readState(); // zweite Abfrage nötig: erster Aufruf löst oft Standby-Exit aus
          xSemaphoreGive(playerMutex);
        }
        playerStatus = st;     // für Statusanzeige / externe Abfrage
        t_start6 = millis();   // Polling-Intervall zurücksetzen
        if (playerStatus < 1) {                                                          // MP3 beendet
          if (wheel_on) { digitalWrite(E2, LOW); } // Mühlrad ausschalten
          if (light_on) { digitalWrite(E3, LOW); } // Licht ausschalten
          ax_live    = false;   // Alarm-Flag löschen
          lastA1Min  = 0xFF;                                                             // Sperre aufheben → nächster Alarm möglich
          lastA2Min  = 0xFF;
          alarmState = ALARM_IDLE;                                                       // → ALARM_IDLE
        }
      }
      break;
  }
}

// ── Kuckuck-State-Machine ────────────────────────────────────
static uint8_t lastCuckooMin = 0xFF;   // Kuckuck-Minutensperre: 0xFF=frei; bei Auslösung=t_min
                                       // file-scope damit S1-Handler (inputTask) ebenfalls setzen kann

static void runCuckooMachine() {
  switch (cuckooState) {

    case CUCKOO_IDLE:
      if (t_min == 0 && t_sec == 0 && cuckoo_on && t_min != lastCuckooMin) {
        // Unterdrücken wenn Alarm 1 oder Alarm 2 auf diese volle Stunde eingestellt ist
        bool alarmThisHour = (a1_on && a1_min == 0 && t_hour == a1_hour)
                          || (a2_on && a2_min == 0 && t_hour == a2_hour);
        // Zeitfenster-Prüfung mit Mitternacht-Überlauf:
        // normal (z.B. 06–22):    t_hour >= on && t_hour <= off
        // über Mitternacht (z.B. 22–06): t_hour >= on || t_hour <= off
        bool inTimeWindow = (cuckoo_onTime <= cuckoo_offTime)
                          ? (t_hour >= cuckoo_onTime && t_hour <= cuckoo_offTime)
                          : (t_hour >= cuckoo_onTime || t_hour <= cuckoo_offTime);
        if (!alarmThisHour && inTimeWindow) {
          lastCuckooMin = t_min;  // Sperre setzen – nächste Auslösung erst wenn 0xFF
          digitalWrite(E1, HIGH); // Kuckuck-Relais einschalten
          t_start4    = millis(); // Ablaufzeit starten (delay4 = Kuckuck-Dauer)
          cuckooState = CUCKOO_RUNNING;                                                  // → CUCKOO_RUNNING
        }
      }
      break;

    case CUCKOO_RUNNING:
      if (delayFunction(t_start4, delay4)) {
        digitalWrite(E1, LOW);   // Kuckuck-Relais ausschalten
        lastCuckooMin = 0xFF;    // Sperre freigeben: nächste volle Stunde im Zeitfenster kann auslösen
        cuckooState = CUCKOO_IDLE;                                                       // → CUCKOO_IDLE
      }
      break;
  }
}



// =============================================================
//  Task 1 – touchTask  (Core 0, Pri 2)
//
//  Touch-State-Machine:
//
//  ┌─────────┐  Touch ON        ┌───────────┐  ≥ TOUCH_HOLD_MS  ┌──────────┐
//  │ TS_IDLE │─────────────────▶│TS_PRESSED │──────────────────▶│TS_REPEAT │
//  └─────────┘  + EVT sofort    └───────────┘  + EVT            └──────────┘
//       ▲                            │                                │
//       │         Touch OFF          │         Touch OFF              │ alle TOUCH_REPEAT_MS
//       └────────────────────────────┘                               │  → EVT senden
//       ▲                                                             │
//       └─────────────────────────────────────────────────────────────┘
//
//  Exklusivität: Sobald ein Pad aktiv ist (TS_PRESSED / TS_REPEAT),
//  werden alle anderen Pads vollständig ignoriert.
// =============================================================
static void touchTask(void *pvParam) {
  touch_pad_init();          // Touch-Sensor-Hardware initialisieren
  touch_pad_set_voltage(TOUCH_HVOLT_2V7, TOUCH_LVOLT_0V5, TOUCH_HVOLT_ATTEN_1V); // Messpegel aus SystemConfig.h
  for (int i = 0; i < 4; i++) { touch_pad_config(TOUCH_PADS[i], 0); } // alle Pads konfigurieren (Threshold=0: nur lesen)

  vTaskDelay(pdMS_TO_TICKS(300)); // Einschwingenzeit: Kondensatoren aufladen lassen

  uint16_t baseline[4];      // Ruhewert jedes Pads (höher = weniger Kapazität = kein Touch)
  for (int i = 0; i < 4; i++) {
    touch_pad_read(TOUCH_PADS[i], &baseline[i]); // Baseline messen
    Serial.printf("Touch Pad %d  Baseline: %u  Threshold: %u\n",
                  i, baseline[i],
                  (baseline[i] > TOUCH_DROP) ? baseline[i] - TOUCH_DROP : baseline[i] - baseline[i] / 5);
  }

  static const uint8_t EVT_ID[4] = { EVT_T0, EVT_T2, EVT_T3, EVT_T4 }; // Array-Index → Event-Code

  // ── State-Machine-Variablen ──────────────────────────────────
  TouchState tsState     = TS_IDLE;   // aktueller Touch-Zustand
  int8_t     activeIdx   = -1;        // aktives Pad (0–3), -1 = keines aktiv
  uint32_t   pressStart  = 0;         // millis() beim ersten Padkontakt → Hold-Erkennung
  uint32_t   lastRepeat  = 0;         // millis() des letzten Repeat-EVT → Repeat-Intervall
  uint32_t   lastRecal   = millis();  // millis() der letzten Baseline-Rekalibrierung

  while (true) {
    // ── Baseline-Rekalibrierung (nur im Ruhezustand) ────────────
    // Kapazitive Touch-Pads driften thermisch. Eine periodische
    // Neumessung der Baseline verhindert Fehlauslösungen nach
    // langem Betrieb. Nur in TS_IDLE: kein aktiver Touch stört.
    if (tsState == TS_IDLE && millis() - lastRecal >= TOUCH_RECAL_MS) {
      for (int i = 0; i < 4; i++) {
        touch_pad_read(TOUCH_PADS[i], &baseline[i]);
      }
      Serial.println("\nTouch Baseline rekalibriert:");
      for (int i = 0; i < 4; i++) {
        Serial.printf("  Pad %d  Baseline: %u  Threshold: %u\n",
                      i, baseline[i],
                      (baseline[i] > TOUCH_DROP) ? baseline[i] - TOUCH_DROP : baseline[i] - baseline[i] / 5);
      }
      lastRecal = millis();   // Rekalibrierungs-Timer zurücksetzen
    }

    // Alle vier Pads einlesen
    uint16_t val[4];          // aktuell gemessene Rohwerte (niedrig = Touch erkannt)
    bool     padPressed[4];   // true wenn Pad berührt (Rohwert unter Schwellwert)
    for (int i = 0; i < 4; i++) {
      touch_pad_read(TOUCH_PADS[i], &val[i]);
      uint16_t thr  = (baseline[i] > TOUCH_DROP) // Schwellwert: Baseline minus Drop
                     ? baseline[i] - TOUCH_DROP          // Normalfall: TOUCH_DROP aus SystemConfig.h
                     : baseline[i] - baseline[i] / 5;    // Fallback: 80% wenn Baseline < TOUCH_DROP
      padPressed[i] = (val[i] < thr); // Touch erkannt wenn Rohwert unter Schwelle
    }

    uint32_t now = millis();   // Zeitstempel für Hold- und Repeat-Berechnung

    switch (tsState) {

      // ── TS_IDLE: alle Pads beobachten, erstes aktives gewinnt ──
      case TS_IDLE:
        for (int i = 0; i < 4; i++) {
          if (padPressed[i]) {
            activeIdx  = i;    // dieses Pad als aktiv merken
            pressStart = now;  // Haltezeitpunkt merken
            lastRepeat = now;   // Repeat-Timer für erstes Wiederholintervall setzen  // Repeat-Timer initialisieren
            uint8_t evt = EVT_ID[i]; // Event-Code für dieses Pad
            xQueueSend(inputQueue, &evt, 0); // sofortiger Erstauslöse-Event (kein Warten)
            tsState = TS_PRESSED; // Zustand: Pad gedrückt, warte auf Hold
            break;             // Exklusivität: nur das erste erkannte Pad aktiv
          }
        }
        break;

      // ── TS_PRESSED: nur activeIdx prüfen, auf HOLD-Schwelle warten
      case TS_PRESSED:
        if (!padPressed[activeIdx]) {                                                    // losgelassen vor HOLD → kein weiterer EVT
          tsState   = TS_IDLE;  // Pad losgelassen → zurück in Ruhezustand
          activeIdx = -1;       // kein aktives Pad
        } else if (now - pressStart >= TOUCH_HOLD_MS) { // HOLD erkannt (TOUCH_HOLD_MS aus SystemConfig.h)
          uint8_t evt = EVT_ID[activeIdx];
          if (xQueueSend(inputQueue, &evt, 0) == pdTRUE) {                               // nur weitermachen wenn EVT angenommen
            lastRepeat = now;
            tsState    = TS_REPEAT; // Wiederholmodus aktivieren
          }
        }
        break;

      // ── TS_REPEAT: alle TOUCH_REPEAT_MS weiteren EVT senden ────
      case TS_REPEAT:
        if (!padPressed[activeIdx]) {                                                    // losgelassen → zurück zu IDLE
          tsState   = TS_IDLE;
          activeIdx = -1;
        } else if (now - lastRepeat >= TOUCH_REPEAT_MS) { // Repeat-Intervall (TOUCH_REPEAT_MS)
          uint8_t evt = EVT_ID[activeIdx];
          if (xQueueSend(inputQueue, &evt, 0) == pdTRUE) {                               // lastRepeat nur bei Erfolg aktualisieren
            lastRepeat = now;
          }
        }
        break;
    }

    vTaskDelay(pdMS_TO_TICKS(TOUCH_POLL_MS)); // Abtastrate (TOUCH_POLL_MS aus SystemConfig.h)
  }
}



// =============================================================
//  Taster-ISRs  (S1, S2, S3)
// =============================================================
// S1-ISR: Event EVT_S1 in Queue, sofortiger Task-Wechsel wenn höher-priorisierter Task bereit
void IRAM_ATTR isrS1() { uint8_t e=EVT_S1; BaseType_t hp=pdFALSE; xQueueSendFromISR(inputQueue,&e,&hp); portYIELD_FROM_ISR(hp); }
void IRAM_ATTR isrS2() { uint8_t e=EVT_S2; BaseType_t hp=pdFALSE; xQueueSendFromISR(inputQueue,&e,&hp); portYIELD_FROM_ISR(hp); } // analog S1
void IRAM_ATTR isrS3() { uint8_t e=EVT_S3; BaseType_t hp=pdFALSE; xQueueSendFromISR(inputQueue,&e,&hp); portYIELD_FROM_ISR(hp); } // analog S1



// =============================================================
//  Task 2 – inputTask  (Core 1, Pri 2)
//
//  Dispatch-Loop:
//    EVT_S1, EVT_S2 → vor displayMutex behandeln (kein Display nötig,
//                     kein vTaskDelay unter Mutex)
//    alle anderen   → displayMutex holen → uiDispatch → uiTransition
//    safeChange     → nvrSemaphore
// =============================================================
static void inputTask(void *pvParam) {
  uint8_t evt;               // empfangener Event-Code aus inputQueue

  while (true) {
    if (xQueueReceive(inputQueue, &evt, pdMS_TO_TICKS(50)) != pdTRUE) { // 50 ms warten, dann Timeout
      if (safeChange) { safeChange = false; xSemaphoreGive(nvrSemaphore); } // ausstehende NVS-Speicherung auslösen
      continue;              // kein Event – nächste Queue-Warteperiode
    }

    // ── S1: Alarm/Sound stoppen oder Kuckuck einmalig ───────────
    // Kein Display nötig → außerhalb displayMutex.
    // vTaskDelay (player.readState-Schleife) ist damit nie unter Mutex.
    if (evt == EVT_S1) {
      uint32_t t_now = millis(); // Entprellzeitstempel
      if (t_now - lastBtnMs[0] >= delay3) {
        lastBtnMs[0] = t_now;  // Entprell-Timer für S1 aktualisieren
        if (xSemaphoreTake(playerMutex, pdMS_TO_TICKS(500)) == pdTRUE) { // Player-Zugriff sichern (max. 500 ms warten)
          int16_t st = player.readState(); // Erster Status-Aufruf
          uint32_t rs_start = millis();    // Timeout-Timer für readState-Schleife
          while (st == -1) {                                                             // Standby-Status auflösen
            if (millis() - rs_start >= 200) {                                           // Timeout 200 ms → DFPlayer antwortet nicht
              st = 0;           // Timeout: DFPlayer antwortet nicht → idle annehmen
              break;
            }
            st = player.readState(); // erneut abfragen bis Standby aufgelöst
            vTaskDelay(pdMS_TO_TICKS(1));                                               // ← niemals unter displayMutex
          }
          if (st > 0) {        // Player spielt → Alarm stoppen
            player.stop();     // MP3-Wiedergabe stoppen
            digitalWrite(E2, LOW); // Mühlrad ausschalten // Mühlrad-Relais ausschalten
            digitalWrite(E3, LOW); // Licht ausschalten // Licht-Relais ausschalten
            ax_live    = false;
            alarmState = ALARM_IDLE;
            lastA1Min  = 0xFF;                                                           // Sperren aufheben → Alarm in gleicher Minute neu auslösbar
            lastA2Min  = 0xFF;
          } else {             // Player idle → manuellen Kuckuck auslösen
            digitalWrite(E1, HIGH); // Kuckuck-Relais einschalten
            t_start4    = millis();  // Kuckuck-Timer starten
            cuckooState = CUCKOO_RUNNING; // Kuckuck-State-Machine aktivieren
            lastCuckooMin = t_min;                                                       // Minuten-Sperre setzen → kein Doppelstart durch Auto-Trigger
          }
          xSemaphoreGive(playerMutex);
        }
      }
      if (safeChange) { safeChange = false; xSemaphoreGive(nvrSemaphore); }
      continue;
    }

    // ── S2: Zugschalter – Licht + Mühlrad EIN/AUS ───────────────
    // Kein Display nötig → außerhalb displayMutex.
    if (evt == EVT_S2) {
      uint32_t t_now = millis();
      if (t_now - lastBtnMs[1] >= delay3) {
        lastBtnMs[1] = t_now;  // Entprell-Timer S2
        S2_SW = !S2_SW;        // Zugschalter-Zustand toggeln
        if (S2_SW) {
          if (wheel_on) { digitalWrite(E2, HIGH); } // Mühlrad einschalten wenn aktiviert
          if (light_on) { digitalWrite(E3, HIGH); } // Licht einschalten wenn aktiviert
        } else {
          digitalWrite(E2, LOW);
          digitalWrite(E3, LOW);
        }
      }
      if (safeChange) { safeChange = false; xSemaphoreGive(nvrSemaphore); }
      continue;
    }

    // ── alle anderen Events: displayMutex holen ───────────────────
    // Touch-Events (T0–T4) aktualisieren den Auto-Rückkehr-Timer.
    if (evt <= EVT_T4) {
      lastTouchMs = millis();  // Auto-Rückkehr-Timer zurücksetzen
    }

    if (xSemaphoreTake(displayMutex, pdMS_TO_TICKS(100)) != pdTRUE) { // Display-Zugriff sichern (max. 100 ms)
      continue;              // Display belegt – Event verwerfen (Display-Update hat Vorrang)
    }

    UiState next = uiDispatch(uiState, evt); // Event verarbeiten → Folge-Zustand berechnen
    if (next != uiState) {   // nur bei Zustandswechsel neu zeichnen
      uiTransition(next);                                                               // Zustand wechseln + Bildschirm zeichnen
    }

    xSemaphoreGive(displayMutex); // Display-Mutex freigeben

    // ── WiFi-Konfig angefordert (von onInfo/EVT_T0) ───────────
    // Mutex erneut holen – displayTask könnte sonst dazwischenfunken.
    // vTaskDelay liegt bewusst AUSSERHALB des Mutex-Blocks.
    if (wifiConfigRequested) {
      wifiConfigRequested = false; // Flag zurücksetzen
      if (xSemaphoreTake(displayMutex, pdMS_TO_TICKS(200)) == pdTRUE) {
        display.clear();     // Display für Meldetext löschen
        zeigeZ16C(64, 16, "WiFi-Setup");
        zeigeZ16C(64, 32, "Neustart ...");
        display.display();
        xSemaphoreGive(displayMutex); // Display freigeben für inputTask // sofort freigeben – vTaskDelay folgt außerhalb
      }
      vTaskDelay(pdMS_TO_TICKS(1500)); // Meldung 1,5 s lesbar halten (außerhalb Mutex!)
      ESP.restart();
    }

    // ── Werksreset angefordert (von onInfo/EVT_T4) ────────────
    if (factoryResetRequested) {
      factoryResetRequested = false; // Flag zurücksetzen
      if (xSemaphoreTake(displayMutex, pdMS_TO_TICKS(200)) == pdTRUE) {
        display.clear();
        zeigeZ16C(64, 10, "Werksreset");
        zeigeZ16C(64, 26, "NVS wird");
        zeigeZ16C(64, 42, "gelöscht ...");
        display.display();
        xSemaphoreGive(displayMutex);
      }
      vTaskDelay(pdMS_TO_TICKS(2000)); // Meldung 2 s lesbar halten
      nvs_flash_erase();                                                                 // NVS-Partition vollständig löschen
      nvs_flash_init();                                                                  // NVS neu initialisieren
      ESP.restart();
    }

    if (safeChange) { safeChange = false; xSemaphoreGive(nvrSemaphore); }
  }
}



// =============================================================
//  Task 3 – displayTask  (Core 1, Pri 1)
//
//  showTime() wird jetzt unter displayMutex aufgerufen:
//  datum[], zeit[], t_* werden konsistent mit menu() geschrieben.
//  NTP-Pending-Flag wird unter Mutex in echte Sync-Buffer übertragen.
//  Auto-Rückkehr zu UI_CLOCK nach delay5 (20 s) ohne Touch-Eingabe.
//  UI_INFO ist ausgenommen – nur S3 verlässt die Info-Seite.
// =============================================================
static void displayTask(void *pvParam) {
  while (true) {

    if (xSemaphoreTake(displayMutex, pdMS_TO_TICKS(100)) == pdTRUE) { // Display-Zugriff anfordern

      showTime(); // Zeit lesen + t_hour/t_min/t_sec setzen – unter Mutex konsistent mit menu()

      // NTP-Sync-Daten sicher übertragen (Callback schrieb in tmp-Puffer)
      if (ntpSyncPending) {
        ntpSyncPending = false; // Flag quittieren
        memcpy(datum_sync, datum_sync_tmp, sizeof(datum_sync)); // atomarer Puffer-Tausch unter Mutex
        memcpy(zeit_sync,  zeit_sync_tmp,  sizeof(zeit_sync));  // atomarer Puffer-Tausch
      }

      if (uiState == UI_CLOCK) {
        if (t_hour == 0 && t_min == 0 && t_sec < 2) { // Mitternacht: Datum wechselt → Vollneuzeichnung
          uiTransition(UI_CLOCK);                                                          // Mitternacht: Seite 0 komplett neu
        } else if (t_sec != t_sec_alt) { // neue Sekunde → nur Zeitzeile partiell aktualisieren
          t_sec_alt = t_sec;   // Vorwert merken
          cleanTXT(20, 0, 120, 16); // Zeitzeile löschen
          zeigeZ16C(64, 0, zeit);                                                          // nur Uhrzeit partiell aktualisieren
        }
      }

      // Auto-Rückkehr: nur wenn nicht UI_CLOCK und nicht UI_INFO,
      // und letzter Touch-Event mindestens delay5 (20 s) zurückliegt.
      if (uiState != UI_CLOCK && uiState != UI_INFO && // nicht auf Uhr- und Info-Seite
          (millis() - lastTouchMs >= delay5)) { // kein Touch seit delay5 ms (20 s)
        uiTransition(UI_CLOCK);                                                            // Auto-Rückkehr
      }

      xSemaphoreGive(displayMutex);
    }

    vTaskDelay(pdMS_TO_TICKS(delay2)); // Wartezeit zwischen Display-Updates (delay2 aus SystemConfig.h)
  }
}



// =============================================================
//  Task 4 – alarmTask  (Core 1, Pri 2)
//
//  Führt Alarm- und Kuckuck-State-Machine aus.
// =============================================================
static void alarmTask(void *pvParam) {
  while (true) {
    runAlarmMachine();         // Alarm-Zustand prüfen und ggf. auslösen/beenden
    runCuckooMachine();        // Kuckuck-Zustand prüfen
    vTaskDelay(pdMS_TO_TICKS(500)); // 500 ms Takt – ausreichend für Sekundengenauigkeit
  }
}



// =============================================================
//  Task 5 – wifiTask  (Core 0, Pri 1)
//
//  WiFi.begin() statt disconnect()+reconnect(): reconnect() ruft
//  intern disconnect() auf und unterbricht dabei den SNTP-Client.
//  WiFi.begin() mit denselben Credentials verbindet sauber neu,
//  ohne den SNTP-Task zu stören.
// =============================================================
static void wifiTask(void *pvParam) {
  while (true) {
    if (WiFi.status() != WL_CONNECTED && delayFunction(t_start7, delay7)) { // Verbindung verloren + Wartezeit abgelaufen
      // Lokaler Snapshot via localtime_r() – thread-safe, kein Mutex nötig.
      // Verhindert Torn-Read der globalen tm-Struct (wird auf Core 1 von showTime() beschrieben).
      time_t    now_local;     // lokale Variable – kein Zugriff auf globale tm-Struct
      struct tm tm_local;      // thread-sicherer Snapshot der Lokalzeit
      time(&now_local);        // UNIX-Zeit lesen
      localtime_r(&now_local, &tm_local); // in Lokalzeit umrechnen (re-entrant, kein Mutex nötig)
      snprintf(datum_WiFi, sizeof(datum_WiFi), "%04u%02u%02u", tm_local.tm_year + 1900, tm_local.tm_mon + 1, tm_local.tm_mday);
      snprintf(zeit_WiFi,  sizeof(zeit_WiFi),  "%02u:%02u:%02u", tm_local.tm_hour, tm_local.tm_min, tm_local.tm_sec);
      WiFi.begin(sta_ssid, sta_psk); // Reconnect mit gespeicherten Credentials
      t_start7 = millis();     // Reconnect-Timer zurücksetzen
    }
    vTaskDelay(pdMS_TO_TICKS(delay7)); // delay7 = Reconnect-Intervall (aus SystemConfig.h)
  }
}



// =============================================================
//  Task 6 – nvrTask  (Core 0, Pri 1)
// =============================================================
static void nvrTask(void *pvParam) {
  while (true) {
    xSemaphoreTake(nvrSemaphore, portMAX_DELAY); // blockiert bis inputTask signalisiert
    data.begin("varSafe", ReadWrite); // NVS-Namespace öffnen // NVS-Namespace öffnen
    writeNVR();              // Standardwerte in NVS schreiben                // alle geänderten Werte in NVS schreiben
    data.end();                // NVS schließen                // NVS-Handle freigeben (flush + close)
  }
}



// =============================================================
//  Task 7 – stackMonTask  (Core 0, Pri 1)
//
//  Gibt alle STACK_MON_INTERVAL_MS (60 s) den verbleibenden
//  Stack-Platz jedes Tasks per Serial aus (in 4-Byte-Words).
//  Ein Wert nahe 0 zeigt drohenden Stack-Überlauf an.
//  Gibt zusätzlich den freien Heap aus.
// =============================================================
static void stackMonTask(void *pvParam) {
  while (true) {
    vTaskDelay(pdMS_TO_TICKS(STACK_MON_INTERVAL_MS)); // 60 s warten
    Serial.println("\n── Stack High-Water Marks (Words frei) ──────────");
    Serial.printf("  touchTask   : %4u\n", uxTaskGetStackHighWaterMark(hTouchTask));
    Serial.printf("  wifiTask    : %4u\n", uxTaskGetStackHighWaterMark(hWifiTask));
    Serial.printf("  nvrTask     : %4u\n", uxTaskGetStackHighWaterMark(hNvrTask));
    Serial.printf("  inputTask   : %4u\n", uxTaskGetStackHighWaterMark(hInputTask));
    Serial.printf("  displayTask : %4u\n", uxTaskGetStackHighWaterMark(hDisplayTask));
    Serial.printf("  alarmTask   : %4u\n", uxTaskGetStackHighWaterMark(hAlarmTask));
    Serial.printf("  stackMonTask: %4u\n", uxTaskGetStackHighWaterMark(nullptr));
    Serial.printf("  Freier Heap : %u Bytes\n", esp_get_free_heap_size()); // Heap-Fragmentierung beobachten
    Serial.println("─────────────────────────────────────────────────");
  }
}



// =============================================================
//  rtosPanic() – FreeRTOS-Objekt- / Task-Erstellungsfehler
//
//  Wird aufgerufen wenn xQueueCreate, xSemaphoreCreate* oder
//  xTaskCreatePinnedToCore nullptr / pdFAIL zurückgeben.
//  Zeigt Fehlertext auf OLED + Serial, startet nach 3 s neu.
//  Heap-Mangel ist die häufigste Ursache – Neustart schafft
//  einen sauberen Zustand.
// =============================================================
static void rtosPanic(const char* what) {
  Serial.print("\n[PANIC] FreeRTOS-Fehler: "); // Fehler auf Serial ausgeben
  Serial.println(what);      // betroffenes Objekt nennen
  display.clear();
  zeigeZ10C(64,  8, "RTOS FEHLER");
  zeigeZ10C(64, 24, what);
  zeigeZ10C(64, 40, "Neustart in 3s");
  display.display();
  delay(3000);               // 3 s Anzeigezeit vor Neustart
  ESP.restart();
}

// =============================================================
//  setup()
// =============================================================
void setup() {
  Serial.begin(115200);      // UART0 für Debug-Ausgabe (115200 Baud)
  Serial2.begin(9600, SERIAL_8N1, RXD2, TXD2); // DFPlayer: 9600 Baud, Pins aus SystemConfig.h
  delay(2000);               // warten bis Serial-Monitor verbunden ist
  bTn_info();                // Versionsstring auf Serial ausgeben

  // ── NVR laden ────────────────────────────────────────────
  data.begin("varSafe", ReadWrite);
  varState = data.getBool("state", false); // Erststart-Flag lesen (false = noch nie geschrieben)
  if (varState) {            // gültige Daten vorhanden → laden
    Serial.println("gespeicherte Variable aus dem NVR lesen"); // Statusmeldung
    readNVR();               // alle Einstellungen aus NVS laden
  } else {
    varState = true;         // Flag setzen
    data.putBool("state", varState); // Erststart-Flag dauerhaft setzen
    writeNVR();
  }
  data.end();

  // ── Display init (wird auch von runWifiConfigServer genutzt) ─
  display.init();            // SSD1306 initialisieren (I2C)
  display.flipScreenVertically(); // Display um 180° drehen (Einbaulage)
  display.clear();
  zeigeZ10C(64, 16, PGMInfo);

  // ── WiFi-Credentials aus NVR laden ───────────────────────
  // Erster Start oder NVR-Flag gelöscht (z.B. via T0 auf Info):
  // → WiFi-Konfigurator starten (blockiert bis Neustart).
  if (!loadWifiCredentials()) {
    Serial.println("[WiFi-Config] Keine Zugangsdaten – starte Konfigurator");
    runWifiConfigServer();   // blockiert bis SSID+PSK eingegeben, dann ESP.restart()
  }
  Serial.printf("[WiFi] Credentials geladen: SSID=%s\n", sta_ssid);

  // ── NTP ──────────────────────────────────────────────────
  sntp_set_time_sync_notification_cb(timeavailable); // Callback bei NTP-Sync registrieren
  configTime(0, 0, MY_NTP_SERVER); // NTP-Server aus SystemConfig.h, UTC-Offset=0 (TZ via setenv)
  setenv("TZ", MY_TZ, 1);   // Zeitzone setzen (MY_TZ aus SystemConfig.h, z.B. "CET-1CEST,M3.5.0,M10.5.0/3")
  tzset();                   // TZ-Einstellung aktivieren

  // ── WiFi verbinden ───────────────────────────────────────
  display.clear();
  zeigeZ10C(64, 8, PGMInfo);
  zeigeZ16C(64, 32, "warte auf");
  zeigeZ16C(64, 49, "WLAN ...");
  WiFi.persistent(false);    // WiFi-Credentials NICHT im internen Flash speichern (NVS übernimmt das)
  WiFi.mode(WIFI_STA);       // Station-Modus (Client)
  WiFi.begin(sta_ssid, sta_psk); // Verbindung mit geladenen Credentials aufbauen
  Serial.println("\nwarte auf WiFi");
  {
    uint32_t t0 = millis();
    while (WiFi.status() != WL_CONNECTED) {
      if (millis() - t0 >= SETUP_WIFI_TIMEOUT_MS) { // Timeout aus SystemConfig.h
        Serial.println("\nWiFi Timeout – starte ohne WLAN");
        cleanTXT(0, 32, 128, 32);
        zeigeZ16C(64, 32, "kein WLAN");
        zeigeZ10C(64, 49, "weiter ohne NTP");
        delay(2000);
        goto wifi_skip;      // Weiter ohne WLAN (NTP entfällt, Uhr nicht gestellt)
      }
      delay(500);
      Serial.print(".");
    }
  }
  Serial.println("\nWiFi connected");
  Serial.println(WiFi.localIP());

  // ── NTP warten ───────────────────────────────────────────
  cleanTXT(0, 49, 128, 15);
  zeigeZ16C(64, 49, "NTP ...");
  Serial.println("\nwarte auf NTP");
  {
    uint32_t t0 = millis();
    while (tm.tm_year < 71) { // tm_year=71 entspricht 1971 – Wert <71 = nicht synchronisiert
      if (millis() - t0 >= SETUP_NTP_TIMEOUT_MS) { // Timeout aus SystemConfig.h
        Serial.println("\nNTP Timeout – Uhr nicht gestellt");
        cleanTXT(0, 49, 128, 15);
        zeigeZ10C(64, 49, "NTP Timeout");
        delay(2000);
        break;
      }
      showTime();
      delay(500);
      Serial.print(".");
    }
  }
  Serial.println("\nNTP ready");

  wifi_skip:

  // ── GPIO ─────────────────────────────────────────────────
  pinMode(S1, INPUT_PULLUP);  // S1: Taster mit internem Pull-up
  pinMode(S2, INPUT_PULLUP);  // S2: Taster mit internem Pull-up
  pinMode(S3, INPUT_PULLUP);  // S3: Taster mit internem Pull-up
  pinMode(E1, OUTPUT);        // E1: Kuckuck-Relais
  pinMode(E2, OUTPUT);        // E2: Mühlrad-Relais
  pinMode(E3, OUTPUT);        // E3: Licht-Relais

  // ── FreeRTOS Objekte ─────────────────────────────────────
  inputQueue   = xQueueCreate(32, sizeof(uint8_t)); // 32 Events à 1 Byte – reicht für schnelle Touch-Folgen
  displayMutex = xSemaphoreCreateMutex(); // Mutex: exklusiver SSD1306-Zugriff
  playerMutex  = xSemaphoreCreateMutex(); // Mutex: exklusiver DFPlayer-Zugriff (Serial2)
  nvrSemaphore = xSemaphoreCreateBinary(); // Binär-Semaphor: Trigger für nvrTask
  if (!inputQueue)   rtosPanic("inputQueue");
  if (!displayMutex) rtosPanic("displayMutex");
  if (!playerMutex)  rtosPanic("playerMutex");
  if (!nvrSemaphore) rtosPanic("nvrSemaphore");

  // ── Taster-Interrupts ────────────────────────────────────
  attachInterrupt(S1, isrS1, FALLING); // S1: fallende Flanke (Pull-up → Tasterdruck)
  attachInterrupt(S2, isrS2, FALLING); // S2: fallende Flanke
  attachInterrupt(S3, isrS3, FALLING); // S3: fallende Flanke

  // ── DFPlayer ─────────────────────────────────────────────
  cleanTXT(0, 49, 128, 15);
  zeigeZ16C(64, 49, "Sound ...");
  if (player.begin(Serial2, true, true)) { // ACK=true, Reset=true – wartet auf DFPlayer-Antwort
    delay(3000);
    Serial.println("\nDFPlayer Serial2 OK");
    player.volume(vol);
    player.EQ(DFPLAYER_EQ_BASS); // Equalizer-Preset
    player.playFolder(2, 1); // Startton: Ordner 02, Datei 001
    delay(1000);             // Startton kurz spielen lassen
    playerStatus = 1;        // Player als aktiv markieren
  } else {
    Serial.println("\nConnecting to DFPlayer Mini failed!");
  }

  // ── Startseite ───────────────────────────────────────────
  snprintf(datum_WiFi,  sizeof(datum_WiFi), "%04u%02u%02u", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
  snprintf(zeit_WiFi,   sizeof(zeit_WiFi),  "%02u:%02u:%02u", tm.tm_hour, tm.tm_min, tm.tm_sec);
  showTime();
  snprintf(str_a1,      sizeof(str_a1),      "%02u:%02u", a1_hour, a1_min);
  snprintf(str_a2,      sizeof(str_a2),      "%02u:%02u", a2_hour, a2_min);
  snprintf(str_s1,      sizeof(str_s1),      "%03u",      sound1);
  snprintf(str_s2,      sizeof(str_s2),      "%03u",      sound2);
  snprintf(str_s1_play, sizeof(str_s1_play), "%03u",      sound1_play);
  snprintf(str_s2_play, sizeof(str_s2_play), "%03u",      sound2_play);
  snprintf(str_vol,     sizeof(str_vol),     "%02u",      vol);
  snprintf(str_cot,     sizeof(str_cot),     "%02u",      cuckoo_onTime);
  snprintf(str_coff,    sizeof(str_coff),    "%02u",      cuckoo_offTime);
  uiTransition(UI_CLOCK);    // Startzustand setzen und Uhrzeitseite zeichnen

  // ── MP3-Anzahl ───────────────────────────────────────────
  {
    uint32_t t0 = millis();
    while (mp3Count < 1) {
      if (millis() - t0 >= SETUP_MP3_TIMEOUT_MS) {
        Serial.println("\nDFPlayer Timeout – mp3Count unbekannt");
        mp3Count = 99;                                                                   // Fallback: MP3-Auswahl bis Datei 99 erlauben
        break;
      }
      int16_t c = player.readFileCounts(); // Anzahl Dateien auf SD lesen
      if (c > 0) mp3Count = c - 1; // -1 weil readFileCounts() 1-basiert zählt; c==0 → Unterlaufschutz
    }
  }
  snprintf(str_mp3, sizeof(str_mp3), "%03u", mp3Count); // für Info-Seite
  snprintf(str_reset, sizeof(str_reset), "%04lu", (unsigned long)resetCount); // 4-stellig für rechtsbündige Anzeige
  Serial.printf("Anzahl mp3-Files: %d\n", mp3Count); // Debug-Ausgabe

  // ── FreeRTOS Tasks starten ───────────────────────────────
  if (xTaskCreatePinnedToCore(touchTask,    "touchTask",    4096, nullptr, 2, &hTouchTask,   0) != pdPASS) rtosPanic("touchTask");   // Core 0, Prio 2
  if (xTaskCreatePinnedToCore(wifiTask,     "wifiTask",     3072, nullptr, 1, &hWifiTask,    0) != pdPASS) rtosPanic("wifiTask");    // Core 0, Prio 1
  if (xTaskCreatePinnedToCore(nvrTask,      "nvrTask",      3072, nullptr, 1, &hNvrTask,     0) != pdPASS) rtosPanic("nvrTask");     // Core 0, Prio 1
  if (xTaskCreatePinnedToCore(stackMonTask, "stackMonTask", 3072, nullptr, 1, nullptr,       0) != pdPASS) rtosPanic("stackMonTask"); // Core 0, Prio 1
  if (xTaskCreatePinnedToCore(inputTask,    "inputTask",    6144, nullptr, 2, &hInputTask,   1) != pdPASS) rtosPanic("inputTask");   // Core 1, Prio 2
  if (xTaskCreatePinnedToCore(displayTask,  "displayTask",  4096, nullptr, 1, &hDisplayTask, 1) != pdPASS) rtosPanic("displayTask"); // Core 1, Prio 1
  if (xTaskCreatePinnedToCore(alarmTask,    "alarmTask",    3072, nullptr, 2, &hAlarmTask,   1) != pdPASS) rtosPanic("alarmTask");   // Core 1, Prio 2
}



// =============================================================
//  loop() – leer
// =============================================================
void loop() {
  vTaskDelete(nullptr);      // Arduino-Loop-Task löschen – alle Arbeit in FreeRTOS-Tasks
}
