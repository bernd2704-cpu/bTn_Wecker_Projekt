// bTn Wecker mit OLED-Anzeige und MP3-Player
// Basis: bTn_Alarm_9v4 – FreeRTOS + State Machine + WiFi-Konfigurator
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
//  ALARM_RUNNING ── MP3 beendet (ALARM_POLL_MS) ─▶ ALARM_IDLE
//
//  Kuckuck-State-Machine  (alarmTask)
//  CUCKOO_IDLE ──── t_min==0, cuckoo_on ──▶ CUCKOO_RUNNING
//  CUCKOO_RUNNING ── CUCKOO_DURATION_MS abgelaufen ────▶ CUCKOO_IDLE
//
// ─── Task-Architektur ────────────────────────────────────────
//  touchTask   Core 0  Pri 2  ESP-IDF touch_pad_* → inputQueue
//                             State Machine: TS_IDLE → TS_PRESSED → TS_REPEAT
//                             Exklusiv: nur ein Pad aktiv, andere gesperrt
//  alarmTask   Core 0  Pri 2  Alarm- + Kuckuck-State-Machine
//                             Core 0: physisch getrennt von inputTask → kein CPU-Scheduling-Konflikt
//  wifiTask    Core 0  Pri 1  WiFi-Reconnect
//  nvrTask     Core 0  Pri 1  Flash-Sicherung bei Änderung
//  stackMonTask Core 0 Pri 1  Stack-Überwachung (Serial)
//  watchdogTask Core 0 Pri 1  Anwendungs-Watchdog: inputTask / displayTask / alarmTask
//  webLogTask   Core 0 Pri 1  HTTP-Server Port WEBLOG_PORT → Ring-Puffer als Web-Seite
//
//  Hardware-TWDT (ESP32): inputTask, displayTask, alarmTask abonniert.
//  Timeout WDT_HARDWARE_MS – hardware-basiert, unabhängig vom FreeRTOS-Scheduler.
//  Verhindert CPU-Lock durch hängenden Task auf Hardware-Ebene.
//  inputTask   Core 1  Pri 2  Dispatch → UI-State-Machine
//  displayTask Core 1  Pri 1  Zeitanzeige, Auto-Rückkehr
// ─────────────────────────────────────────────────────────────

const char PGMInfo[] = "bTn_Alarm_9v4";                                                  // PROGMEM-fähig; kein String-Heap-Fragment

// ── Bibliotheken ─────────────────────────────────────────────
#include <WiFi.h>
#include <WebServer.h>
#include <time.h>
#include <esp_sntp.h>
#include <SSD1306Wire.h>
#include <DFRobotDFPlayerMini.h>
#include <Preferences.h>
#include <nvs_flash.h>
#include <driver/touch_pad.h>
#include <freertos/semphr.h>          // FreeRTOS Semaphore / Mutex API
#include <esp_task_wdt.h>             // ESP32 Hardware Task Watchdog Timer (TWDT)

// ── Konfiguration ────────────────────────────────────────────
#include "SysConf_9v4.h"           // Pin-Belegung, Timing-Konstanten, Touch-Schwellwerte
#include "WEB.h"

// ── WiFi-Laufzeit-Zugangsdaten (aus NVR, ab 4v0) ─────────────
// Werden in loadWifiCredentials() gefüllt und danach in
// WiFi.begin() sowie wifiTask verwendet.
static char sta_ssid[33] = "";                                                           // max. 32 Zeichen + '\0'
static char sta_psk [64] = "";                                                           // max. 63 Zeichen + '\0'
// ── Touch-Task ───────────────────────────────────────────────

static const touch_pad_t TOUCH_PADS[4] = {
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

enum AlarmState  : uint8_t { ALARM_IDLE,  ALARM_RUNNING  };
enum CuckooState : uint8_t { CUCKOO_IDLE, CUCKOO_RUNNING };

// Touch-Task State Machine
// TS_IDLE    – kein Touch aktiv
// TS_PRESSED – Touch erkannt, wartet auf HOLD-Schwelle (750 ms)
// TS_REPEAT  – HOLD erreicht, sendet EVT alle TOUCH_REPEAT_MS (250 ms)
enum TouchState  : uint8_t { TS_IDLE, TS_PRESSED, TS_REPEAT };

volatile UiState     uiState     = UI_CLOCK;
volatile AlarmState  alarmState  = ALARM_IDLE;
volatile CuckooState cuckooState = CUCKOO_IDLE;

// ── FreeRTOS Objekte ─────────────────────────────────────────
static QueueHandle_t     inputQueue   = nullptr;
static SemaphoreHandle_t displayMutex = nullptr;                                         // Mutex: exklusiver Display-Zugriff
static SemaphoreHandle_t playerMutex  = nullptr;                                         // Mutex: exklusiver DFPlayer-Zugriff (thread-safe Serial2)
static SemaphoreHandle_t nvrSemaphore = nullptr;

// Task-Handles – werden in setup() befüllt, von stackMonTask gelesen
static TaskHandle_t hTouchTask   = nullptr;
static TaskHandle_t hWifiTask    = nullptr;
static TaskHandle_t hNvrTask     = nullptr;
static TaskHandle_t hInputTask   = nullptr;
static TaskHandle_t hDisplayTask = nullptr;
static TaskHandle_t hAlarmTask      = nullptr; // Handle für stackMonTask-Abfrage
static TaskHandle_t hWatchdogTask   = nullptr; // Handle für watchdogTask

// ── Hardware ─────────────────────────────────────────────────
SSD1306Wire         display(0x3C, SDA, SCL, GEOMETRY_128_64);
DFRobotDFPlayerMini player;
Preferences         data;

// ── Zeit / Datum ─────────────────────────────────────────────
time_t  now;
tm      timeinfo;
char    datum[11];
char    zeit[9];
char    datum_sync[9];
char    zeit_sync[9];
char    datum_WiFi[9];
char    zeit_WiFi[9];

// NTP-Callback schreibt in tmp-Puffer + setzt Flag.
// displayTask überträgt unter displayMutex in datum_sync/zeit_sync.
// Verhindert Race Condition zwischen SNTP-Task und menu(6).
static char          datum_sync_tmp[9];
static char          zeit_sync_tmp[9];
static volatile bool ntpSyncPending  = false; // true: NTP-Callback hat neue Daten, displayTask überträgt
static char          datum_WiFi_tmp[9];        // Double-Buffer: wifiTask schreibt hier (Core 0, kein Mutex nötig)
static char          zeit_WiFi_tmp[9];         // displayTask überträgt unter displayMutex nach datum_WiFi/zeit_WiFi
static volatile bool wifiSyncPending = false;  // true: wifiTask hat neue Verbindungsdaten
volatile uint8_t t_hour;
volatile uint8_t t_min;
volatile uint8_t t_sec;
volatile uint8_t t_sec_alt;

// ── Alarm ────────────────────────────────────────────────────
bool    a1_on   = true;
uint8_t a1_hour = 6;
uint8_t a1_min  = 0;
char    str_a1[6];
bool    a2_on   = true;
uint8_t a2_hour = 6;
uint8_t a2_min  = 0;
char    str_a2[6];
volatile bool    ax_live = false;

volatile uint32_t t_start4 = 0;
         uint32_t lastTouchMs = 0;                                                       // Zeitstempel letzter Touch-Event (EVT_T0–T4)
volatile uint32_t t_start6 = 0;
         uint32_t t_start7 = 0;

// ── Sound ────────────────────────────────────────────────────
bool    sound1_on   = false;
uint8_t sound1_selected      = 1;
char    str_s1[4];
uint8_t sound1_assigned = 1;
char    str_s1_play[4];
bool    sound2_on   = false;
uint8_t sound2_selected      = 1;
char    str_s2[4];
uint8_t sound2_assigned = 1;
char    str_s2_play[4];
uint8_t vol         = 9;
uint8_t MAX_VOL     = 20;
char    str_vol[3];
int16_t playerStatus = 0;      // nur Core 1: alarmTask schreibt, alarmTask liest – kein volatile nötig
int16_t mp3Count    = 0;
char    str_mp3[4];
uint32_t resetCount = 0;
char     str_reset[5];                                                                   // "nnn" + null (max 999 Resets sichtbar)

volatile bool safeChange = false;

// ── Taster Toggle-Status ─────────────────────────────────────
bool          S2_SW = false;                                                             // Toggle-Status Zugschalter

// ── Funktion-Vorwahl ─────────────────────────────────────────
bool    cuckoo_on      = false;
uint8_t cuckoo_onTime  = 6;             // erste Stunde (von hh), Default 06:00
uint8_t cuckoo_offTime = 22;            // letzte Stunde (bis hh), Default 22:00
char    str_cot[3];                     // "hh" von-Zeit
char    str_coff[3];                    // "hh" bis-Zeit
bool    light_on   = true;
bool    wheel_on   = false;

// pageselect: spiegelt (uint8_t)uiState – wird von checkboxAlarm/Sound genutzt
volatile uint8_t pageselect = 0;

// ── Taster-Debounce ──────────────────────────────────────────
// Zwei-Stufen-Debouncing:
//   isrBtnMs[]  – ISR-Ebene:      filtert Hardware-Prellen (BTN_LOCKOUT_MS  =  30 ms)
//   lastBtnMs[] – Task-Ebene:     Aktionssperre            (BTN_LOCKOUT_MS   = 1000 ms)
static volatile uint32_t isrBtnMs[3] = {};  // IRAM-zugänglich: von ISR gelesen/geschrieben
static uint32_t          lastBtnMs[3] = {}; // von inputTask gelesen/geschrieben

// ── Anwendungs-Watchdog Alive-Timestamps ─────────────────────
// Jeder überwachte Task setzt seinen Wert in jedem Zyklus.
// watchdogTask prüft alle WDG_CHECK_MS ob der Wert jünger als WDG_TIMEOUT_MS ist.
static volatile uint32_t wdg_inputTask   = 0; // gesetzt von inputTask   (alle ~50 ms)
static volatile uint32_t wdg_displayTask = 0; // gesetzt von displayTask (alle DISPLAY_UPDATE_MS)
static volatile uint32_t wdg_alarmTask   = 0; // gesetzt von alarmTask   (alle 500 ms)




// =============================================================
//  Web-Logger  (ab 9v1)
//
//  webLog(msg) ersetzt Serial.*-Ausgaben nach WiFi-Connect.
//  Schreibt in einen Ring-Puffer (WEBLOG_LINES Einträge).
//  webLogTask startet einen HTTP-Server auf Port WEBLOG_PORT.
//  Browser ruft / auf → HTML-Seite mit Auto-Refresh alle 10 s.
//  /log liefert den aktuellen Pufferinhalt als plain text.
//  webLogReady-Flag: webLog() puffert erst wenn Task gestartet.
// =============================================================

static SemaphoreHandle_t webLogMutex  = nullptr;          // schützt den Ring-Puffer
static volatile bool     webLogReady  = false;             // true: webLogTask läuft

// Ring-Puffer
static char   webLogBuf[WEBLOG_LINES][WEBLOG_LINE_LEN];   // Zeilen-Array
static uint16_t webLogHead = 0;                            // nächste Schreibposition
static uint16_t webLogCount = 0;                           // bisher eingetragene Zeilen

// ── Snapshots: jeweils letzter Wert mit Timestamp ─────────────
// Werden unter webLogMutex geschrieben und in der Web-Seite
// als dedizierte Sektionen angezeigt (nicht im Ring-Puffer).
#define SNAP_BUF_LEN  480
static char snapTouchBuf[SNAP_BUF_LEN] = "(noch keine Daten)";
static char snapTouchTime[20]          = "";
static char snapStackBuf[SNAP_BUF_LEN] = "(noch keine Daten)";
static char snapStackTime[20]          = "";

// Schreibt eine Nachricht in den Ring-Puffer (thread-safe).
// Bleibt still wenn Mutex noch nicht initialisiert.
void webLog(const char* msg) {
  if (!webLogMutex) return;
  if (xSemaphoreTake(webLogMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
    snprintf(webLogBuf[webLogHead], WEBLOG_LINE_LEN, "%s", msg);
    webLogHead  = (webLogHead + 1) % WEBLOG_LINES;        // Überlauf: älteste Zeile überschreiben
    if (webLogCount < WEBLOG_LINES) webLogCount++;
    xSemaphoreGive(webLogMutex);
  }
}

// Printf-Variante für komfortablen Aufruf
void webLogf(const char* fmt, ...) {
  char buf[WEBLOG_LINE_LEN];
  va_list args;
  va_start(args, fmt);
  vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  webLog(buf);
}

// Schreibt den aktuellen Zeitstempel in buf (NTP-Uhrzeit oder Uptime)
static void snapTimeStr(char* buf, size_t len) {
  time_t t = time(nullptr);
  if (t > 1700000000UL) {
    struct tm tm_val;
    localtime_r(&t, &tm_val);
    strftime(buf, len, "%d.%m.%Y %H:%M:%S", &tm_val);
  } else {
    snprintf(buf, len, "+%lus", millis() / 1000UL);
  }
}

// Aktualisiert den Touch-Baseline-Snapshot (thread-safe)
static void updateSnapTouch(const uint16_t* baseline) {
  char tmp[SNAP_BUF_LEN];
  int pos = 0;
  for (int i = 0; i < 4 && pos < (int)sizeof(tmp) - 1; i++) {
    uint16_t thr = (baseline[i] > TOUCH_DROP)
                 ? baseline[i] - TOUCH_DROP
                 : baseline[i] - baseline[i] / 5;
    pos += snprintf(tmp + pos, sizeof(tmp) - pos,
                    "  Pad %d  Baseline: %u  Threshold: %u\n", i, baseline[i], thr);
  }
  if (pos > 0 && tmp[pos - 1] == '\n') tmp[pos - 1] = '\0';
  if (webLogMutex && xSemaphoreTake(webLogMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
    strncpy(snapTouchBuf, tmp, SNAP_BUF_LEN - 1);
    snapTouchBuf[SNAP_BUF_LEN - 1] = '\0';
    snapTimeStr(snapTouchTime, sizeof(snapTouchTime));
    xSemaphoreGive(webLogMutex);
  }
}

// Aktualisiert den Stack-HWM-Snapshot (thread-safe)
static void updateSnapStack() {
  char tmp[SNAP_BUF_LEN];
  int pos = 0;
  pos += snprintf(tmp + pos, sizeof(tmp) - pos,
    "  touchTask   : %4u\n", uxTaskGetStackHighWaterMark(hTouchTask));
  pos += snprintf(tmp + pos, sizeof(tmp) - pos,
    "  wifiTask    : %4u\n", uxTaskGetStackHighWaterMark(hWifiTask));
  pos += snprintf(tmp + pos, sizeof(tmp) - pos,
    "  nvrTask     : %4u\n", uxTaskGetStackHighWaterMark(hNvrTask));
  pos += snprintf(tmp + pos, sizeof(tmp) - pos,
    "  inputTask   : %4u\n", uxTaskGetStackHighWaterMark(hInputTask));
  pos += snprintf(tmp + pos, sizeof(tmp) - pos,
    "  displayTask : %4u\n", uxTaskGetStackHighWaterMark(hDisplayTask));
  pos += snprintf(tmp + pos, sizeof(tmp) - pos,
    "  alarmTask   : %4u\n", uxTaskGetStackHighWaterMark(hAlarmTask));
  pos += snprintf(tmp + pos, sizeof(tmp) - pos,
    "  watchdogTask: %4u\n", uxTaskGetStackHighWaterMark(hWatchdogTask));
  pos += snprintf(tmp + pos, sizeof(tmp) - pos,
    "  stackMonTask: %4u\n", uxTaskGetStackHighWaterMark(nullptr));
  pos += snprintf(tmp + pos, sizeof(tmp) - pos,
    "  Freier Heap : %u Bytes", esp_get_free_heap_size());
  if (webLogMutex && xSemaphoreTake(webLogMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
    strncpy(snapStackBuf, tmp, SNAP_BUF_LEN - 1);
    snapStackBuf[SNAP_BUF_LEN - 1] = '\0';
    snapTimeStr(snapStackTime, sizeof(snapStackTime));
    xSemaphoreGive(webLogMutex);
  }
}

// =============================================================
//  Hilfsfunktionen
// =============================================================

void bTn_info() {
  Serial.println("\n----------------------------------------");
  Serial.println(PGMInfo);
}

bool delayFunction(uint32_t lastTime, uint32_t actualDelay) {
  return (millis() - lastTime >= actualDelay);
}

void showTime() {
  time(&now);
  localtime_r(&now, &timeinfo);
  snprintf(datum, sizeof(datum), "%02u.%02u.%04u", timeinfo.tm_mday, timeinfo.tm_mon + 1, timeinfo.tm_year + 1900);
  snprintf(zeit,  sizeof(zeit),  "%02u:%02u:%02u",  timeinfo.tm_hour, timeinfo.tm_min,    timeinfo.tm_sec);
  t_sec  = timeinfo.tm_sec;
  t_min  = timeinfo.tm_min;
  t_hour = timeinfo.tm_hour;
}

// NTP-Synchronisations-Callback (wird vom SNTP-Task aufgerufen, nicht vom
// Haupt-Task). Schreibt nur in thread-lokale tmp-Puffer und setzt ein Flag.
// displayTask überträgt die Daten sicher unter displayMutex.
void timeavailable(struct timeval *t) {
  time(&now);
  localtime_r(&now, &timeinfo);
  snprintf(datum_sync_tmp, sizeof(datum_sync_tmp), "%04u%02u%02u", timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday);
  snprintf(zeit_sync_tmp,  sizeof(zeit_sync_tmp),  "%02u:%02u:%02u", timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
  ntpSyncPending = true;                                                                   // displayTask überträgt unter Mutex
}



// =============================================================
//  Display-Hilfsfunktionen
//  Unverändert. Alle Aufrufe nur unter displayMutex (außer setup()).
// =============================================================

void cleanTXT(uint8_t xPos, uint8_t yPos, uint8_t dx, uint8_t dy) {
  display.setColor(BLACK);
  display.fillRect(xPos, yPos, dx, dy);
  display.setColor(WHITE);
}

void zeigeZ10C(uint8_t xPos, uint8_t yPos, const char* TXT) {
  display.setTextAlignment(TEXT_ALIGN_CENTER);
  display.setFont(ArialMT_Plain_10);
  display.drawString(xPos, yPos, TXT);
}

void zeigeZ10L(uint8_t xPos, uint8_t yPos, const char* TXT) {
  display.setTextAlignment(TEXT_ALIGN_LEFT);
  display.setFont(ArialMT_Plain_10);
  display.drawString(xPos, yPos, TXT);
}

void zeigeZ10R(uint8_t xPos, uint8_t yPos, const char* TXT) {
  display.setTextAlignment(TEXT_ALIGN_RIGHT);
  display.setFont(ArialMT_Plain_10);
  display.drawString(xPos, yPos, TXT);
}

void zeigeZ16C(uint8_t xPos, uint8_t yPos, const char* TXT) {
  display.setTextAlignment(TEXT_ALIGN_CENTER);
  display.setFont(ArialMT_Plain_16);
  display.drawString(xPos, yPos, TXT);
}

void zeigeZ16L(uint8_t xPos, uint8_t yPos, const char* TXT) {
  display.setTextAlignment(TEXT_ALIGN_LEFT);
  display.setFont(ArialMT_Plain_16);
  display.drawString(xPos, yPos, TXT);
}

// Alarm-Checkboxen (nutzt pageselect – wird von uiTransition synchron gesetzt)
void checkboxAlarm() {
  display.setColor(BLACK);
  display.fillRect(68, 38, 7, 7);
  display.fillRect(68, 55, 7, 7);
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
  display.display();                                                                   // einmaliger Flush nach allen Zeichenoperationen
}

void checkboxSound() {
  display.setColor(BLACK);
  display.fillRect(68, 38, 7, 7);
  display.fillRect(68, 55, 7, 7);
  display.setColor(WHITE);
  switch (pageselect) {
    case 3:
      if (sound1_on) {
        display.fillRect(68, 38, 7, 7);
        display.display();                                                               // Checkbox anzeigen bevor Audio startet
        sound1_assigned = sound1_selected;
        if (xSemaphoreTake(playerMutex, pdMS_TO_TICKS(50)) == pdTRUE) {                  // 50 ms < 100 ms displayMutex-Timeout
          player.playFolder(1, sound1_assigned);
          xSemaphoreGive(playerMutex);
        }
      } else {
        display.drawRect(68, 38, 7, 7);
        display.display();                                                               // Checkbox anzeigen bevor Player gestoppt
        if (xSemaphoreTake(playerMutex, pdMS_TO_TICKS(50)) == pdTRUE) {                  // 50 ms < 100 ms displayMutex-Timeout
          player.stop();
          xSemaphoreGive(playerMutex);
        }
      }
      break;
    case 4:
      if (sound2_on) {
        display.fillRect(68, 55, 7, 7);
        display.display();                                                               // Checkbox anzeigen bevor Audio startet
        sound2_assigned = sound2_selected;
        if (xSemaphoreTake(playerMutex, pdMS_TO_TICKS(50)) == pdTRUE) {                  // 50 ms < 100 ms displayMutex-Timeout
          player.playFolder(1, sound2_assigned);
          xSemaphoreGive(playerMutex);
        }
      } else {
        display.drawRect(68, 55, 7, 7);
        display.display();                                                               // Checkbox anzeigen bevor Player gestoppt
        if (xSemaphoreTake(playerMutex, pdMS_TO_TICKS(50)) == pdTRUE) {                  // 50 ms < 100 ms displayMutex-Timeout
          player.stop();
          xSemaphoreGive(playerMutex);
        }
      }
      break;
  }
}

void checkboxFunction() {
  display.setColor(BLACK);
  display.fillRect(35, 21, 7, 7);
  display.fillRect(35, 38, 7, 7);
  display.fillRect(35, 55, 7, 7);
  display.setColor(WHITE);
  if (cuckoo_on) { display.fillRect(35, 21, 7, 7); } else { display.drawRect(35, 21, 7, 7); }
  if (light_on)  { display.fillRect(35, 38, 7, 7); } else { display.drawRect(35, 38, 7, 7); }
  if (wheel_on)  { display.fillRect(35, 55, 7, 7); } else { display.drawRect(35, 55, 7, 7); }
  display.display();                                                                   // einmaliger Flush nach allen Zeichenoperationen
}

void menu(uint8_t page) {   // uint8_t: Koordinatenbereich 0–7 entspricht UiState-Werten
  switch (page) {
    case 0:
      display.clear();
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
      { char webLogUrl[24];
        snprintf(webLogUrl, sizeof(webLogUrl), "%s:%u",
                 WiFi.localIP().toString().c_str(), (unsigned)WEBLOG_PORT);
        zeigeZ10C(63,  54, webLogUrl); }    // Web-Log-Adresse anzeigen
      // Hinweis: T0 drücken startet WiFi-Konfigurator
      // (wird unterhalb der Box angezeigt, blinkt nicht – statisch)
      // T4 löst Werksreset aus (NVS löschen + Neustart)
      break;
  }
  display.display();                                                                     // einmaliger Flush nach vollständiger Seitenzeichnung
}



// =============================================================
//  NVR (Flash-Persistenz)  –  unverändert
// =============================================================

void writeNVR() {
  data.putBool("a1_on",       a1_on);
  data.putInt ("a1_hour",     a1_hour);
  data.putInt ("a1_min",      a1_min);
  data.putBool("a2_on",       a2_on);
  data.putInt ("a2_hour",     a2_hour);
  data.putInt ("a2_min",      a2_min);
  data.putInt ("sound1_assigned", sound1_assigned);
  data.putInt ("sound2_assigned", sound2_assigned);
  data.putBool("cuckoo_on",   cuckoo_on);
  data.putInt ("cuckoo_on_h",  cuckoo_onTime);
  data.putInt ("cuckoo_off_h", cuckoo_offTime);
  data.putBool("light_on",    light_on);
  data.putBool("wheel_on",    wheel_on);
  data.putInt ("vol",         vol);
}

void readNVR() {
  a1_on       = data.getBool("a1_on",       a1_on);
  a1_hour     = data.getInt ("a1_hour",      a1_hour);
  a1_min      = data.getInt ("a1_min",       a1_min);
  a2_on       = data.getBool("a2_on",        a2_on);
  a2_hour     = data.getInt ("a2_hour",      a2_hour);
  a2_min      = data.getInt ("a2_min",       a2_min);
  sound1_assigned = data.getInt ("sound1_assigned",  sound1_assigned);
  sound2_assigned = data.getInt ("sound2_assigned",  sound2_assigned);
  cuckoo_on      = data.getBool("cuckoo_on",    cuckoo_on);
  cuckoo_onTime  = data.getInt ("cuckoo_on_h",  cuckoo_onTime);
  cuckoo_offTime = data.getInt ("cuckoo_off_h", cuckoo_offTime);
  light_on    = data.getBool("light_on",     light_on);
  wheel_on    = data.getBool("wheel_on",     wheel_on);
  vol         = data.getInt ("vol",          vol);

  // ── Wertebereich-Clamp: korrupte NVS-Daten abfangen ─────────
  // Fehlerhafter NVS-Inhalt (z.B. nach Flash-Fehler oder Versionsänderung)
  // könnte ohne Clamp zu falschen Display-Strings oder DFPlayer-Fehlern führen.
  a1_hour        = min(a1_hour,        (uint8_t)23);   // Stunde 0–23
  a1_min         = min(a1_min,         (uint8_t)59);   // Minute 0–59
  a2_hour        = min(a2_hour,        (uint8_t)23);
  a2_min         = min(a2_min,         (uint8_t)59);
  cuckoo_onTime  = min(cuckoo_onTime,  (uint8_t)23);   // Von-Stunde 0–23
  cuckoo_offTime = min(cuckoo_offTime, (uint8_t)23);   // Bis-Stunde 0–23
  if (sound1_assigned < 1) sound1_assigned = 1;                // DFPlayer: Dateinummer min. 1
  if (sound2_assigned < 1) sound2_assigned = 1;                // Obergrenze erst nach mp3Count bekannt
  if (vol > MAX_VOL)   vol = MAX_VOL;                  // Lautstärke 0–MAX_VOL

  resetCount  = data.getUInt("resetCount",   0);
  resetCount++;                                                                          // Neustart zählen
  data.putUInt("resetCount", resetCount);
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
bool loadWifiCredentials() {
  Preferences wifiPrefs;
  wifiPrefs.begin("wifiCfg", ReadOnly);
  bool valid = wifiPrefs.getBool("valid", false);
  if (valid) {
    wifiPrefs.getString("ssid", sta_ssid, sizeof(sta_ssid));
    wifiPrefs.getString("psk",  sta_psk,  sizeof(sta_psk));
  }
  wifiPrefs.end();
  return valid && (strlen(sta_ssid) > 0);
}

// ── NVR-Zugangsdaten löschen (erzwingt Config-Mode beim nächsten Start) ──
void clearWifiCredentials() {
  Preferences wifiPrefs;
  wifiPrefs.begin("wifiCfg", ReadWrite);
  wifiPrefs.putBool("valid", false);
  wifiPrefs.end();
}

// ── Serverseitige Validierung ────────────────────────────────
// SSID: 1–32 Zeichen; PSK: leer (offen) oder 8–63 Zeichen.
// Gibt leeren String bei Erfolg, sonst Fehlermeldung zurück.
static String validateWifiInput(const String& ssid, const String& psk) {
  if (ssid.length() < 1 || ssid.length() > 32)
    return "SSID ungueltig: 1-32 Zeichen erforderlich.";
  if (psk.length() > 0 && psk.length() < 8)
    return "Passwort ungueltig: Leer lassen oder 8-63 Zeichen.";
  if (psk.length() > 63)
    return "Passwort zu lang (max. 63 Zeichen).";
  return "";
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
  WiFi.mode(WIFI_AP);
  WiFi.softAP(WIFI_AP_SSID, nullptr, WIFI_AP_CHANNEL);
  Serial.print("[WiFi-Config] AP-IP: ");
  Serial.println(WiFi.softAPIP());

  WebServer server(80);

  // GET / → Konfigurationsseite (aus WEB.h, PROGMEM)
  server.on("/", HTTP_GET, [&server]() {
    server.send_P(200, "text/html", WIFI_CONFIG_PAGE);
  });

  // POST /save → Daten prüfen und speichern
  server.on("/save", HTTP_POST, [&server]() {
    String ssid = server.arg("ssid");
    String psk  = server.arg("psk");
    ssid.trim();
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
    wifiPrefs.putBool  ("valid", true);
    wifiPrefs.end();

    Serial.println("[WiFi-Config] Gespeichert: SSID=" + ssid);

    // Erfolgsseite senden (aus WEB.h, PROGMEM)
    server.send_P(200, "text/html", WIFI_SUCCESS_PAGE);

    // Kurze Pause damit Browser die Seite empfangen kann.
    // delay() ist hier korrekt: runWifiConfigServer() läuft VOR Task-Start,
    // kein FreeRTOS-Scheduler aktiv – vTaskDelay() wäre hier nicht verfügbar.
    delay(3500);

    // OLED: Neustart-Meldung
    display.clear();
    zeigeZ16C(64, 22, "Gespeichert!");
    zeigeZ16C(64, 42, "Neustart ...");
    display.display();
    delay(500);
    ESP.restart();
  });

  // Alle anderen Pfade → zurück zur Hauptseite
  server.onNotFound([&server]() {
    server.sendHeader("Location", "/", true);
    server.send(302, "text/plain", "");
  });

  server.begin();
  Serial.println("[WiFi-Config] HTTP-Server gestartet, warte auf Eingabe ...");

  // Blockiere bis POST /save verarbeitet wurde
  while (true) {
    server.handleClient();
    delay(5);                 // delay() korrekt: vor Task-Start, kein Scheduler aktiv; verhindert WDT-Reset
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
  uiState    = next;
  pageselect = (uint8_t)next;                                                            // checkboxAlarm/Sound nutzen pageselect
  menu(pageselect);                                                                      // Bildschirm zeichnen (Entry-Aktion)
}

// ── Per-State-Handler: T2/T3/T4 ─────────────────────────────
// Jeder Handler gibt den Folge-Zustand zurück.
// Bleibt der Zustand gleich, wird uiTransition NICHT aufgerufen
// (kein unnötiges Neuzeichnen; nur partielle Display-Updates).

// — UI_CLOCK: Lautstärke mit T3/T4 ──────────────────────────
static UiState onClock(uint8_t evt) {
  switch (evt) {
    case EVT_T3:                                                                         // Lautstärke +
      if (vol < MAX_VOL) {
        vol++;
        if (xSemaphoreTake(playerMutex, pdMS_TO_TICKS(50)) == pdTRUE) {                 // 50 ms < 100 ms displayMutex-Timeout
          player.volume(vol);
          xSemaphoreGive(playerMutex);
        }
        snprintf(str_vol, sizeof(str_vol), "%02u", vol);
        cleanTXT(115, 17, 15, 10);
        zeigeZ10L(115, 17, str_vol);
        display.display();                                                               // partielles Update übertragen
        safeChange = true;
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
        display.display();                                                               // partielles Update übertragen
        safeChange = true;
      }
      break;
  }
  return UI_CLOCK;
}

// — UI_ALARM1: Alarm 1 Ein/Aus, Stunde+, Minute+ ─────────────
static UiState onAlarm1(uint8_t evt) {
  switch (evt) {
    case EVT_T2:
      a1_on = !a1_on;
      checkboxAlarm();
      safeChange = true;
      break;
    case EVT_T3:                                                                         // Stunde +
      if (a1_hour < 23) { a1_hour++; } else { a1_hour = 0; }
      snprintf(str_a1, sizeof(str_a1), "%02u:%02u", a1_hour, a1_min);
      cleanTXT(82, 34, 46, 13);
      zeigeZ16C(105, 32, str_a1);
      display.display();                                                                 // partielles Update übertragen
      safeChange = true;
      break;
    case EVT_T4:                                                                         // Minute +
      if (a1_min < 59) { a1_min++; } else { a1_min = 0; }
      snprintf(str_a1, sizeof(str_a1), "%02u:%02u", a1_hour, a1_min);
      cleanTXT(82, 34, 46, 13);
      zeigeZ16C(105, 32, str_a1);
      display.display();                                                                 // partielles Update übertragen
      safeChange = true;
      break;
  }
  return UI_ALARM1;
}

// — UI_ALARM2: Alarm 2 Ein/Aus, Stunde+, Minute+ ─────────────
static UiState onAlarm2(uint8_t evt) {
  switch (evt) {
    case EVT_T2:
      a2_on = !a2_on;
      checkboxAlarm();
      safeChange = true;
      break;
    case EVT_T3:                                                                         // Stunde +
      if (a2_hour < 23) { a2_hour++; } else { a2_hour = 0; }
      snprintf(str_a2, sizeof(str_a2), "%02u:%02u", a2_hour, a2_min);
      cleanTXT(82, 51, 46, 13);                                                          // A2-Zeile (Y=49) – war fälschlich 34 (A1-Zeile)
      zeigeZ16C(105, 49, str_a2);
      display.display();                                                                 // partielles Update übertragen
      safeChange = true;
      break;
    case EVT_T4:                                                                         // Minute +
      if (a2_min < 59) { a2_min++; } else { a2_min = 0; }
      snprintf(str_a2, sizeof(str_a2), "%02u:%02u", a2_hour, a2_min);
      cleanTXT(82, 51, 46, 13);
      zeigeZ16C(105, 49, str_a2);
      display.display();                                                                 // partielles Update übertragen
      safeChange = true;
      break;
  }
  return UI_ALARM2;
}

// — UI_SOUND1: Sound 1 OK/+/– ────────────────────────────────
static UiState onSound1(uint8_t evt) {
  switch (evt) {
    case EVT_T2:                                                                         // Sound 1 OK (Vorschau)
      sound1_on = !sound1_on;
      checkboxSound();
      if (sound1_on) {
        snprintf(str_s1_play, sizeof(str_s1_play), "%03u", sound1_selected);
        cleanTXT(42, 17, 20, 10);
        zeigeZ10L(42, 17, str_s1_play);
        display.display();                                                               // partielles Update übertragen
      }
      break;
    case EVT_T3:                                                                         // Sound 1 +
      if (sound1_selected < mp3Count) { sound1_selected++; } else { sound1_selected = 1; }
      snprintf(str_s1, sizeof(str_s1), "%03u", sound1_selected);
      cleanTXT(82, 34, 46, 13);
      zeigeZ16C(105, 32, str_s1);
      sound1_on = false;
      checkboxSound();
      safeChange = true;
      break;
    case EVT_T4:                                                                         // Sound 1 –
      if (sound1_selected > 1) { sound1_selected--; } else { sound1_selected = mp3Count; }
      snprintf(str_s1, sizeof(str_s1), "%03u", sound1_selected);
      cleanTXT(82, 34, 46, 13);
      zeigeZ16C(105, 32, str_s1);
      sound1_on = false;
      checkboxSound();
      safeChange = true;
      break;
  }
  return UI_SOUND1;
}

// — UI_SOUND2: Sound 2 OK/+/– ────────────────────────────────
static UiState onSound2(uint8_t evt) {
  switch (evt) {
    case EVT_T2:                                                                         // Sound 2 OK (Vorschau)
      sound2_on = !sound2_on;
      checkboxSound();
      if (sound2_on) {
        snprintf(str_s2_play, sizeof(str_s2_play), "%03u", sound2_selected);
        cleanTXT(108, 17, 20, 10);
        zeigeZ10L(108, 17, str_s2_play);
        display.display();                                                               // partielles Update übertragen
      }
      break;
    case EVT_T3:                                                                         // Sound 2 +
      if (sound2_selected < mp3Count) { sound2_selected++; } else { sound2_selected = 1; }
      snprintf(str_s2, sizeof(str_s2), "%03u", sound2_selected);
      cleanTXT(82, 51, 46, 13);                                                          // S2-Zeile (Y=49)
      zeigeZ16C(105, 49, str_s2);                                                        // S2-Zeile – war fälschlich 32 (S1-Zeile)
      sound2_on = false;
      checkboxSound();
      safeChange = true;
      break;
    case EVT_T4:                                                                         // Sound 2 –
      if (sound2_selected > 1) { sound2_selected--; } else { sound2_selected = mp3Count; }
      snprintf(str_s2, sizeof(str_s2), "%03u", sound2_selected);
      cleanTXT(82, 51, 46, 13);
      zeigeZ16C(105, 49, str_s2);
      sound2_on = false;
      checkboxSound();
      safeChange = true;
      break;
  }
  return UI_SOUND2;
}

// — UI_FUNCS: Kuckuck / Licht / Mühlrad toggle ───────────────
static UiState onFuncs(uint8_t evt) {
  switch (evt) {
    case EVT_T2:
      cuckoo_on = !cuckoo_on;
      checkboxFunction();
      safeChange = true;
      break;
    case EVT_T3:
      light_on = !light_on;
      checkboxFunction();
      safeChange = true;
      break;
    case EVT_T4:
      wheel_on = !wheel_on;
      checkboxFunction();
      safeChange = true;
      break;
  }
  return UI_FUNCS;
}

// — UI_CUCKOO_TIME: T3 → von hh +, T4 → bis hh + ───────────
static UiState onCuckooTime(uint8_t evt) {
  switch (evt) {
    case EVT_T3:                                                                         // von hh +
      if (cuckoo_onTime < 23) { cuckoo_onTime++; } else { cuckoo_onTime = 0; }
      snprintf(str_cot, sizeof(str_cot), "%02u", cuckoo_onTime);
      cleanTXT(78, 35, 23, 13);
      zeigeZ16C(91, 32, str_cot);
      display.display();                                                                 // partielles Update übertragen
      safeChange = true;
      break;
    case EVT_T4:                                                                         // bis hh +
      if (cuckoo_offTime < 23) { cuckoo_offTime++; } else { cuckoo_offTime = 0; }
      snprintf(str_coff, sizeof(str_coff), "%02u", cuckoo_offTime);
      cleanTXT(78, 52, 23, 13);
      zeigeZ16C(91, 49, str_coff);
      display.display();                                                                 // partielles Update übertragen
      safeChange = true;
      break;
  }
  return UI_CUCKOO_TIME;
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
  return UI_INFO;                                                                        // Nur S3/T0 verlässt Info
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
    return cycle[(uint8_t)s];
  }

  // ── S3: Info-Seite ein/aus (global, mit Taster-Entprellung) ─
  if (evt == EVT_S3) {
    uint32_t t_now = millis();
    if (t_now - lastBtnMs[2] >= BTN_LOCKOUT_MS) {
      lastBtnMs[2] = t_now;
      return (s == UI_INFO) ? UI_CLOCK : UI_INFO;                                       // Toggle Info
    }
    return s;
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
    default:        return s;
  }
}



// =============================================================
//  System-State-Machines  (werden von alarmTask aufgerufen)
// =============================================================

static uint8_t lastA1Min = 0xFF;  // Alarm-Minuten-Sperre (file-scope: auch vom manuellen Abbruch setzbar)
static uint8_t lastA2Min = 0xFF;

// ── Alarm-State-Machine ──────────────────────────────────────
// sec/min/hour: atomarer Zeitschnappschuss aus alarmTask – keine Race Condition mit displayTask
static void runAlarmMachine(uint8_t sec, uint8_t min, uint8_t hour) {
  switch (alarmState) {

    case ALARM_IDLE:
      // Alarm 1 prüfen
      if (a1_on && sec == 0 && min == a1_min && hour == a1_hour
          && min != lastA1Min) {                                                         // nicht dieselbe Minute wiederholen
        lastA1Min = min;
        if (xSemaphoreTake(playerMutex, pdMS_TO_TICKS(200)) == pdTRUE) {
          player.playFolder(1, sound1_assigned);
          xSemaphoreGive(playerMutex);
        }
        if (wheel_on) { digitalWrite(E2, HIGH); }
        if (light_on) { digitalWrite(E3, HIGH); }
        t_start6   = millis();
        ax_live    = true;
        alarmState = ALARM_RUNNING;                                                      // → ALARM_RUNNING
      }
      // Alarm 2 prüfen (else if → Alarm 1 hat Vorrang bei gleicher Zeit)
      else if (a2_on && sec == 0 && min == a2_min && hour == a2_hour
          && min != lastA2Min) {                                                         // nicht dieselbe Minute wiederholen
        lastA2Min = min;
        if (xSemaphoreTake(playerMutex, pdMS_TO_TICKS(200)) == pdTRUE) {
          player.playFolder(1, sound2_assigned);
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
      if (delayFunction(t_start6, ALARM_POLL_MS)) {
        int16_t st = -1;
        if (xSemaphoreTake(playerMutex, pdMS_TO_TICKS(200)) == pdTRUE) {
          st = player.readState();
          xSemaphoreGive(playerMutex);                                                     // Mutex SOFORT freigeben – nie mit gehaltenem Mutex schlafen
        }
        vTaskDelay(pdMS_TO_TICKS(1));                                                      // 1ms Pause AUSSERHALB Mutex: DFPlayer-Antwort stabilisieren
        if (xSemaphoreTake(playerMutex, pdMS_TO_TICKS(200)) == pdTRUE) {
          st = player.readState();                                                          // zweite Abfrage → korrekter Status
          xSemaphoreGive(playerMutex);
        }
        playerStatus = st;
        t_start6 = millis();
        if (playerStatus == 0) {                                                         // MP3 beendet (0=stopped; -1=UART-Timeout → Alarm läuft weiter)
          if (wheel_on) { digitalWrite(E2, LOW); }
          if (light_on) { digitalWrite(E3, LOW); }
          ax_live    = false;
          lastA1Min  = 0xFF;                                                             // Sperre aufheben → nächster Alarm möglich
          lastA2Min  = 0xFF;
          alarmState = ALARM_IDLE;                                                       // → ALARM_IDLE
        }
      }
      break;
  }
}

// ── Kuckuck-State-Machine ────────────────────────────────────
static uint8_t lastCuckooMin = 0xFF;   // Minuten-Sperre Kuckuck (file-scope für manuellen Zugriff)

// sec/min/hour: atomarer Zeitschnappschuss aus alarmTask
static void runCuckooMachine(uint8_t sec, uint8_t min, uint8_t hour) {
  switch (cuckooState) {

    case CUCKOO_IDLE:
      if (min == 0 && sec == 0 && cuckoo_on && min != lastCuckooMin) {
        // Unterdrücken wenn Alarm 1 oder Alarm 2 auf diese volle Stunde eingestellt ist
        bool alarmThisHour = (a1_on && a1_min == 0 && hour == a1_hour)
                          || (a2_on && a2_min == 0 && hour == a2_hour);
        // Zeitfenster-Prüfung mit Mitternacht-Überlauf:
        // normal (z.B. 06–22):    hour >= on && hour <= off
        // über Mitternacht (z.B. 22–06): hour >= on || hour <= off
        bool inTimeWindow = (cuckoo_onTime <= cuckoo_offTime)
                          ? (hour >= cuckoo_onTime && hour <= cuckoo_offTime)
                          : (hour >= cuckoo_onTime || hour <= cuckoo_offTime);
        if (!alarmThisHour && inTimeWindow) {
          lastCuckooMin = min;
          digitalWrite(E1, HIGH);
          t_start4    = millis();
          cuckooState = CUCKOO_RUNNING;                                                  // → CUCKOO_RUNNING
        }
      }
      break;

    case CUCKOO_RUNNING:
      if (delayFunction(t_start4, CUCKOO_DURATION_MS)) {
        digitalWrite(E1, LOW);
        lastCuckooMin = 0xFF;                                                            // Sperre aufheben → nächste volle Stunde kann auslösen
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
  touch_pad_init();
  touch_pad_set_voltage(TOUCH_HVOLT_2V7, TOUCH_LVOLT_0V5, TOUCH_HVOLT_ATTEN_1V);
  for (int i = 0; i < 4; i++) { touch_pad_config(TOUCH_PADS[i], 0); }

  vTaskDelay(pdMS_TO_TICKS(300));                                                        // Einschwingenzeit

  uint16_t baseline[4];
  for (int i = 0; i < 4; i++) {
    touch_pad_read(TOUCH_PADS[i], &baseline[i]);
  }
  updateSnapTouch(baseline);                                                               // initiale Baseline im Snapshot speichern

  static const uint8_t EVT_ID[4] = { EVT_T0, EVT_T2, EVT_T3, EVT_T4 };

  // ── State-Machine-Variablen ──────────────────────────────────
  TouchState tsState     = TS_IDLE;
  int8_t     activeIdx   = -1;                                                            // Index des aktiven Pads (0–3), -1 = keines
  uint32_t   pressStart  = 0;                                                             // Zeitpunkt des ersten Kontakts
  uint32_t   lastRepeat  = 0;                                                             // Zeitpunkt des letzten EVT im REPEAT-Zustand
  uint32_t   lastRecal   = millis();                                                      // Zeitpunkt der letzten Baseline-Messung

  while (true) {
    // ── Baseline-Rekalibrierung (nur im Ruhezustand) ────────────
    // Kapazitive Touch-Pads driften thermisch. Eine periodische
    // Neumessung der Baseline verhindert Fehlauslösungen nach
    // langem Betrieb. Nur in TS_IDLE: kein aktiver Touch stört.
    if (tsState == TS_IDLE && millis() - lastRecal >= TOUCH_RECAL_MS) {
      for (int i = 0; i < 4; i++) {
        touch_pad_read(TOUCH_PADS[i], &baseline[i]);
      }
      updateSnapTouch(baseline);                                                           // Snapshot aktualisieren (kein Ring-Puffer-Eintrag)
      lastRecal = millis();
    }

    // Alle vier Pads einlesen
    uint16_t val[4];
    bool     padPressed[4];
    for (int i = 0; i < 4; i++) {
      touch_pad_read(TOUCH_PADS[i], &val[i]);
      uint16_t thr  = (baseline[i] > TOUCH_DROP)
                     ? baseline[i] - TOUCH_DROP          // Normalfall: absoluter Schwellwert
                     : baseline[i] - baseline[i] / 5;    // Fallback: 80 % der Baseline (kein float)
      padPressed[i] = (val[i] < thr);
    }

    uint32_t now = millis();

    switch (tsState) {

      // ── TS_IDLE: alle Pads beobachten, erstes aktives gewinnt ──
      case TS_IDLE:
        for (int i = 0; i < 4; i++) {
          if (padPressed[i]) {
            activeIdx  = i;
            pressStart = now;
            lastRepeat = now;
            uint8_t evt = EVT_ID[i];
            xQueueSend(inputQueue, &evt, 0);                                             // sofortiger erster EVT
            tsState = TS_PRESSED;
            break;                                                                       // Exklusiv: nur erstes Pad, restliche ignorieren
          }
        }
        break;

      // ── TS_PRESSED: nur activeIdx prüfen, auf HOLD-Schwelle warten
      case TS_PRESSED:
        if (!padPressed[activeIdx]) {                                                    // losgelassen vor HOLD → kein weiterer EVT
          tsState   = TS_IDLE;
          activeIdx = -1;
        } else if (now - pressStart >= TOUCH_HOLD_MS) {                                 // HOLD-Schwelle überschritten
          uint8_t evt = EVT_ID[activeIdx];
          if (xQueueSend(inputQueue, &evt, 0) == pdTRUE) {                               // nur weitermachen wenn EVT angenommen
            lastRepeat = now;
            tsState    = TS_REPEAT;
          }
        }
        break;

      // ── TS_REPEAT: alle TOUCH_REPEAT_MS weiteren EVT senden ────
      case TS_REPEAT:
        if (!padPressed[activeIdx]) {                                                    // losgelassen → zurück zu IDLE
          tsState   = TS_IDLE;
          activeIdx = -1;
        } else if (now - lastRepeat >= TOUCH_REPEAT_MS) {                               // Wiederholintervall abgelaufen
          uint8_t evt = EVT_ID[activeIdx];
          if (xQueueSend(inputQueue, &evt, 0) == pdTRUE) {                               // lastRepeat nur bei Erfolg aktualisieren
            lastRepeat = now;
          }
        }
        break;
    }

    vTaskDelay(pdMS_TO_TICKS(TOUCH_POLL_MS));
  }
}



// =============================================================
//  Taster-ISRs  (S1, S2, S3)
//
//  Stufe 1 – Hardware-Entprellung im ISR (BTN_DEBOUNCE_MS = 30 ms):
//  Prellimpulse innerhalb des Fensters werden verworfen, die Queue
//  bleibt sauber. millis() ist in ISR-Kontext auf dem ESP32 sicher.
//
//  Stufe 2 – Aktionssperre in inputTask (BTN_LOCKOUT_MS = 1000 ms):
//  Verhindert unbeabsichtigte Doppelaktionen nach bewusstem Druck.
// =============================================================
void IRAM_ATTR isrS1() {
  uint32_t now = millis();
  if (now - isrBtnMs[0] < BTN_DEBOUNCE_MS) return;          // Prellimpuls → verwerfen
  isrBtnMs[0] = now;
  uint8_t e = EVT_S1; BaseType_t hp = pdFALSE;
  xQueueSendFromISR(inputQueue, &e, &hp); portYIELD_FROM_ISR(hp);
}
void IRAM_ATTR isrS2() {
  uint32_t now = millis();
  if (now - isrBtnMs[1] < BTN_DEBOUNCE_MS) return;          // Prellimpuls → verwerfen
  isrBtnMs[1] = now;
  uint8_t e = EVT_S2; BaseType_t hp = pdFALSE;
  xQueueSendFromISR(inputQueue, &e, &hp); portYIELD_FROM_ISR(hp);
}
void IRAM_ATTR isrS3() {
  uint32_t now = millis();
  if (now - isrBtnMs[2] < BTN_DEBOUNCE_MS) return;          // Prellimpuls → verwerfen
  isrBtnMs[2] = now;
  uint8_t e = EVT_S3; BaseType_t hp = pdFALSE;
  xQueueSendFromISR(inputQueue, &e, &hp); portYIELD_FROM_ISR(hp);
}



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
  esp_task_wdt_add(NULL);          // Hardware-TWDT: diesen Task anmelden
  uint8_t evt;

  while (true) {
    esp_task_wdt_reset();          // Hardware-TWDT zurücksetzen – alle ~50 ms
    if (xQueueReceive(inputQueue, &evt, pdMS_TO_TICKS(50)) != pdTRUE) {
      wdg_inputTask = millis();                                                        // Alive-Signal: Task läuft (auch bei leerem Queue)
      if (safeChange) { safeChange = false; xSemaphoreGive(nvrSemaphore); }
      continue;
    }
    wdg_inputTask = millis();                                                          // Alive-Signal: Event empfangen

    // ── S1: Alarm/Sound stoppen oder Kuckuck einmalig ───────────
    // Kein Display nötig → außerhalb displayMutex.
    // vTaskDelay (player.readState-Schleife) ist damit nie unter Mutex.
    if (evt == EVT_S1) {
      uint32_t t_now = millis();
      if (t_now - lastBtnMs[0] >= BTN_LOCKOUT_MS) {
        lastBtnMs[0] = t_now;
        if (xSemaphoreTake(playerMutex, pdMS_TO_TICKS(500)) == pdTRUE) {
          int16_t st = player.readState();
          uint32_t rs_start = millis();
          while (st == -1) {                                                             // Standby-Status auflösen
            if (millis() - rs_start >= 200) {                                           // Timeout 200 ms → DFPlayer antwortet nicht
              st = 0;                                                                   // als "idle" behandeln → kein stop(), Kuckuck auslösen
              break;
            }
            st = player.readState();
            vTaskDelay(pdMS_TO_TICKS(1));                                               // ← niemals unter displayMutex
          }
          if (st > 0) {
            player.stop();
            digitalWrite(E2, LOW);
            digitalWrite(E3, LOW);
            ax_live    = false;
            alarmState = ALARM_IDLE;
            lastA1Min  = 0xFF;                                                           // Sperren aufheben → Alarm in gleicher Minute neu auslösbar
            lastA2Min  = 0xFF;
          } else {
            digitalWrite(E1, HIGH);
            t_start4    = millis();
            cuckooState = CUCKOO_RUNNING;
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
      if (t_now - lastBtnMs[1] >= BTN_LOCKOUT_MS) {
        lastBtnMs[1] = t_now;
        S2_SW = !S2_SW;
        if (S2_SW) {
          if (wheel_on) { digitalWrite(E2, HIGH); }
          if (light_on) { digitalWrite(E3, HIGH); }
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
      lastTouchMs = millis();
    }

    if (xSemaphoreTake(displayMutex, pdMS_TO_TICKS(100)) != pdTRUE) {
      continue;                                                                          // Display belegt – Event verwerfen
    }

    UiState next = uiDispatch(uiState, evt);                                            // State-Machine auswerten
    if (next != uiState) {
      uiTransition(next);                                                               // Zustand wechseln + Bildschirm zeichnen
    }

    xSemaphoreGive(displayMutex);

    // ── WiFi-Konfig angefordert (von onInfo/EVT_T0) ───────────
    // Mutex erneut holen – displayTask könnte sonst dazwischenfunken.
    // vTaskDelay liegt bewusst AUSSERHALB des Mutex-Blocks.
    if (wifiConfigRequested) {
      wifiConfigRequested = false;
      if (xSemaphoreTake(displayMutex, pdMS_TO_TICKS(200)) == pdTRUE) {
        display.clear();
        zeigeZ16C(64, 16, "WiFi-Setup");
        zeigeZ16C(64, 32, "Neustart ...");
        display.display();
        xSemaphoreGive(displayMutex);
      }
      vTaskDelay(pdMS_TO_TICKS(1500));                                                   // Meldung lesbar halten – außerhalb Mutex
      ESP.restart();
    }

    // ── Werksreset angefordert (von onInfo/EVT_T4) ────────────
    if (factoryResetRequested) {
      factoryResetRequested = false;
      if (xSemaphoreTake(displayMutex, pdMS_TO_TICKS(200)) == pdTRUE) {
        display.clear();
        zeigeZ16C(64, 10, "Werksreset");
        zeigeZ16C(64, 26, "NVS wird");
        zeigeZ16C(64, 42, "gelöscht ...");
        display.display();
        xSemaphoreGive(displayMutex);
      }
      vTaskDelay(pdMS_TO_TICKS(2000));                                                   // Meldung lesbar halten – außerhalb Mutex
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
//  Auto-Rückkehr zu UI_CLOCK nach AUTO_RETURN_MS (20 s) ohne Touch-Eingabe.
//  UI_INFO ist ausgenommen – nur S3 verlässt die Info-Seite.
// =============================================================
static void displayTask(void *pvParam) {
  esp_task_wdt_add(NULL);          // Hardware-TWDT: diesen Task anmelden
  while (true) {
    esp_task_wdt_reset();          // Hardware-TWDT zurücksetzen – alle ~300 ms
    wdg_displayTask = millis();                                                            // Alive-Signal: vor Mutex – gültig auch wenn Mutex-Timeout auftritt

    if (xSemaphoreTake(displayMutex, pdMS_TO_TICKS(100)) == pdTRUE) {

      showTime();                                                                          // datum[], zeit[], t_* unter Mutex → konsistent mit menu()

      // NTP-Sync-Daten sicher übertragen (Callback schrieb in tmp-Puffer)
      if (ntpSyncPending) {
        ntpSyncPending = false;
        memcpy(datum_sync, datum_sync_tmp, sizeof(datum_sync));
        memcpy(zeit_sync,  zeit_sync_tmp,  sizeof(zeit_sync));
      }

      // WiFi-Verbindungsdaten sicher übertragen (wifiTask schrieb in tmp-Puffer auf Core 0)
      if (wifiSyncPending) {
        wifiSyncPending = false;
        memcpy(datum_WiFi, datum_WiFi_tmp, sizeof(datum_WiFi)); // atomarer Puffer-Tausch unter displayMutex
        memcpy(zeit_WiFi,  zeit_WiFi_tmp,  sizeof(zeit_WiFi));  // atomarer Puffer-Tausch
      }

      if (uiState == UI_CLOCK) {
        if (t_hour == 0 && t_min == 0 && t_sec < 2) {
          uiTransition(UI_CLOCK); // Mitternacht: menu() zeichnet Seite komplett + ruft display.display() intern
        } else if (t_sec != t_sec_alt) {
          t_sec_alt = t_sec;
          cleanTXT(20, 0, 120, 16);
          zeigeZ16C(64, 0, zeit);
          display.display();                                                             // Uhrzeitzeile übertragen
        }
      }

      // Auto-Rückkehr: nur wenn nicht UI_CLOCK und nicht UI_INFO,
      // und letzter Touch-Event mindestens AUTO_RETURN_MS (20 s) zurückliegt.
      if (uiState != UI_CLOCK && uiState != UI_INFO &&
          (millis() - lastTouchMs >= AUTO_RETURN_MS)) {
        uiTransition(UI_CLOCK);  // Auto-Rückkehr: menu() übernimmt display.display()
      }

      xSemaphoreGive(displayMutex);
    }

    vTaskDelay(pdMS_TO_TICKS(DISPLAY_UPDATE_MS));
  }
}



// =============================================================
//  Task 4 – alarmTask  (Core 1, Pri 2)
//
//  Führt Alarm- und Kuckuck-State-Machine aus.
// =============================================================
static void alarmTask(void *pvParam) {
  esp_task_wdt_add(NULL);          // Hardware-TWDT: diesen Task anmelden
  while (true) {
    esp_task_wdt_reset();          // Hardware-TWDT zurücksetzen – alle 500 ms
    // Atomarer Zeitschnappschuss – verhindert Race Condition mit displayTask:
    // displayTask (Pri 1) könnte mitten in showTime() von alarmTask (Pri 2) verdrängt
    // werden und t_sec/t_min/t_hour inkonsistent hinterlassen.
    // localtime_r() ist re-entrant und schreibt nur in den lokalen Puffer.
    time_t    now_alarm;
    struct tm tm_alarm;
    time(&now_alarm);
    localtime_r(&now_alarm, &tm_alarm);
    const uint8_t a_sec  = (uint8_t)tm_alarm.tm_sec;
    const uint8_t a_min  = (uint8_t)tm_alarm.tm_min;
    const uint8_t a_hour = (uint8_t)tm_alarm.tm_hour;
    runAlarmMachine(a_sec, a_min, a_hour);
    runCuckooMachine(a_sec, a_min, a_hour);
    wdg_alarmTask = millis();                                                          // Alive-Signal: alarmTask aktiv
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
    if (WiFi.status() != WL_CONNECTED && delayFunction(t_start7, WIFI_RECONNECT_MS)) {
      // Lokaler Snapshot via localtime_r() – thread-safe, kein Mutex nötig.
      // Verhindert Torn-Read der globalen tm-Struct (wird auf Core 1 von showTime() beschrieben).
      time_t    now_local;
      struct tm tm_local;
      time(&now_local);
      localtime_r(&now_local, &tm_local);
      snprintf(datum_WiFi_tmp, sizeof(datum_WiFi_tmp), "%04u%02u%02u", tm_local.tm_year + 1900, tm_local.tm_mon + 1, tm_local.tm_mday);
      snprintf(zeit_WiFi_tmp,  sizeof(zeit_WiFi_tmp),  "%02u:%02u:%02u", tm_local.tm_hour, tm_local.tm_min, tm_local.tm_sec);
      wifiSyncPending = true;  // displayTask überträgt unter displayMutex → kein torn read
      WiFi.begin(sta_ssid, sta_psk);                                                     // Laufzeit-Credentials aus NVR
      t_start7 = millis();
    }
    vTaskDelay(pdMS_TO_TICKS(WIFI_RECONNECT_MS));
  }
}



// =============================================================
//  Task 6 – nvrTask  (Core 0, Pri 1)
// =============================================================
static void nvrTask(void *pvParam) {
  while (true) {
    xSemaphoreTake(nvrSemaphore, portMAX_DELAY);
    data.begin("varSafe", ReadWrite);
    writeNVR();
    data.end();
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
    vTaskDelay(pdMS_TO_TICKS(STACK_MON_INTERVAL_MS));
    updateSnapStack();                                                                     // Snapshot aktualisieren (kein Ring-Puffer-Eintrag)
  }
}



// =============================================================
//  Task 8 – watchdogTask  (Core 0, Pri 1)
//
//  Anwendungs-Watchdog: überwacht ob inputTask, displayTask und alarmTask
//  noch regelmäßig laufen. Jeder Task setzt seinen wdg_*-Timestamp in
//  jedem Zyklus. Der Watchdog prüft alle WDG_CHECK_MS ob der Timestamp
//  jünger als WDG_TIMEOUT_MS (30 s) ist.
//
//  Schützt gegen echte Deadlocks (Task hängt komplett).
//  Begrenzt NICHT die Alarm-Dauer: alarmTask läuft in ALARM_RUNNING
//  weiterhin alle 500 ms und setzt seinen Timestamp.
// =============================================================
static void watchdogTask(void *pvParam) {
  // Kurze Startpause: alle Tasks sollen sich einmal initialisiert haben
  vTaskDelay(pdMS_TO_TICKS(WDG_TIMEOUT_MS));

  while (true) {
    vTaskDelay(pdMS_TO_TICKS(WDG_CHECK_MS)); // alle 5 s prüfen
    uint32_t now = millis();

    bool inputOk   = (now - wdg_inputTask)   < WDG_TIMEOUT_MS;
    bool displayOk = (now - wdg_displayTask) < WDG_TIMEOUT_MS;
    bool alarmOk   = (now - wdg_alarmTask)   < WDG_TIMEOUT_MS;

    if (!inputOk || !displayOk || !alarmOk) {
      webLog("[WATCHDOG] Task-Freeze erkannt!");
      if (!inputOk)   webLogf("  inputTask   : %lu ms ohne Lebenszeichen", now - wdg_inputTask);
      if (!displayOk) webLogf("  displayTask : %lu ms ohne Lebenszeichen", now - wdg_displayTask);
      if (!alarmOk)   webLogf("  alarmTask   : %lu ms ohne Lebenszeichen", now - wdg_alarmTask);
      // Display-Meldung nur wenn displayTask noch läuft (sonst I2C-Zugriff riskant)
      if (displayOk) {
        if (xSemaphoreTake(displayMutex, pdMS_TO_TICKS(500)) == pdTRUE) {
          display.clear();
          zeigeZ10C(64,  8, "WATCHDOG");
          zeigeZ10C(64, 24, "Task Freeze!");
          zeigeZ10C(64, 40, "Neustart...");
          display.display();
          xSemaphoreGive(displayMutex);
        }
      }
      vTaskDelay(pdMS_TO_TICKS(2000));  // Meldung kurz lesbar halten
      ESP.restart();
    }
  }
}



// =============================================================
//  Task 9 – webLogTask  (Core 0, Pri 1)
//
//  Startet HTTP-Server auf Port WEBLOG_PORT sobald WiFi bereit.
//  GET /     → HTML-Seite mit Auto-Refresh (alle 3 s)
//  GET /log  → Ring-Puffer als plain text (neueste Zeilen zuerst)
//  Der Task blockiert bis WiFi verbunden ist (prüft alle 2 s).
//  Aktiv sobald sich ein Client verbindet.
// =============================================================
static void webLogTask(void *pvParam) {
  // Warten bis WiFi steht
  while (WiFi.status() != WL_CONNECTED) {
    vTaskDelay(pdMS_TO_TICKS(2000));
  }

  WebServer logServer(WEBLOG_PORT);

  // GET / → HTML-Seite
  logServer.on("/", HTTP_GET, [&logServer]() {
    String ip = WiFi.localIP().toString();
    String html =
      "<!DOCTYPE html><html><head><meta charset='UTF-8'>"
      "<meta http-equiv='refresh' content='10'>"
      "<title>bTn Wecker Log</title>"
      "<style>"
      "body{font-family:monospace;background:#1a1a2e;color:#e0e0e0;margin:0;padding:16px}"
      "h2{color:#BDD7EE;margin-bottom:8px}"
      "h3{color:#888;font-weight:normal;font-size:13px;margin:0 0 12px}"
      ".snap-wrap{margin-bottom:16px}"
      ".snap-title{font-size:12px;color:#78909c;margin-bottom:4px}"
      ".snap-title .ts{color:#4A9EFF;font-weight:bold}"
      ".snap-box{background:#0d1a0d;border:1px solid #2a4a2a;border-radius:6px;padding:10px;"
      "white-space:pre;font-size:13px;color:#b0d0b0}"
      "#log{background:#0d0d1a;border:1px solid #333;border-radius:6px;padding:12px;"
      "white-space:pre-wrap;word-break:break-all;max-height:60vh;overflow-y:auto;font-size:13px}"
      ".ok{color:#6BCB77}.err{color:#FF6B6B}.warn{color:#FFD93D}"
      ".sec-title{font-size:12px;color:#78909c;margin:16px 0 4px}"
      "</style></head><body>"
      "<h2>&#x1F553; bTn Wecker 9v3 &ndash; Web-Log</h2>"
      "<h3>IP: " + ip + ":" + String(WEBLOG_PORT) + " &nbsp;|&nbsp; Auto-Refresh: 10 s"
      " &nbsp;|&nbsp; Aktualisiert: <span id='upd'></span></h3>";

    // ── Snapshot: Touch Baseline ─────────────────────────────
    if (webLogMutex && xSemaphoreTake(webLogMutex, pdMS_TO_TICKS(200)) == pdTRUE) {
      String touchTime = String(snapTouchTime);
      String touchContent = String(snapTouchBuf);
      String stackTime = String(snapStackTime);
      String stackContent = String(snapStackBuf);
      xSemaphoreGive(webLogMutex);

      touchContent.replace("<", "&lt;"); touchContent.replace(">", "&gt;");
      stackContent.replace("<", "&lt;"); stackContent.replace(">", "&gt;");

      html += "<div class='snap-wrap'>"
              "<div class='snap-title'>Touch Baseline – letzte Kalibrierung: "
              "<span class='ts'>" + touchTime + "</span></div>"
              "<div class='snap-box'>" + touchContent + "</div></div>";

      html += "<div class='snap-wrap'>"
              "<div class='snap-title'>Stack High-Water Marks – letzte Messung: "
              "<span class='ts'>" + stackTime + "</span></div>"
              "<div class='snap-box'>" + stackContent + "</div></div>";
    }

    // ── Ring-Puffer ──────────────────────────────────────────
    html += "<div class='sec-title'>Allgemeines Log</div>"
            "<div id='log'>";
    if (webLogMutex && xSemaphoreTake(webLogMutex, pdMS_TO_TICKS(200)) == pdTRUE) {
      uint16_t start = (webLogCount < WEBLOG_LINES)
                     ? 0
                     : webLogHead;                         // ältester Eintrag
      for (uint16_t i = 0; i < webLogCount; i++) {
        uint16_t idx = (start + i) % WEBLOG_LINES;
        String line = String(webLogBuf[idx]);
        if (line.indexOf("[WATCHDOG]") >= 0 || line.indexOf("[PANIC]") >= 0 || line.indexOf("failed") >= 0)
          html += "<span class='err'>";
        else if (line.indexOf("OK") >= 0 || line.indexOf("ready") >= 0 || line.indexOf("connected") >= 0)
          html += "<span class='ok'>";
        else if (line.indexOf("Timeout") >= 0 || line.indexOf("Warnung") >= 0)
          html += "<span class='warn'>";
        else
          html += "<span>";
        line.replace("<", "&lt;"); line.replace(">", "&gt;");
        html += line + "</span>\n";
      }
      xSemaphoreGive(webLogMutex);
    }
    html += "</div><script>document.getElementById('upd').textContent=new Date().toLocaleTimeString();</script></body></html>";
    logServer.send(200, "text/html; charset=UTF-8", html);
  });

  // GET /log → plain text für curl / wget
  logServer.on("/log", HTTP_GET, [&logServer]() {
    String out = "";
    if (webLogMutex && xSemaphoreTake(webLogMutex, pdMS_TO_TICKS(200)) == pdTRUE) {
      uint16_t start = (webLogCount < WEBLOG_LINES) ? 0 : webLogHead;
      for (uint16_t i = 0; i < webLogCount; i++) {
        out += String(webLogBuf[(start + i) % WEBLOG_LINES]) + "\n";
      }
      xSemaphoreGive(webLogMutex);
    }
    logServer.send(200, "text/plain; charset=UTF-8", out);
  });

  logServer.begin();
  webLogReady = true;
  webLogf("[Reset] Anzahl: %lu", (unsigned long)resetCount);

  while (true) {
    logServer.handleClient();                              // eingehende Requests verarbeiten
    vTaskDelay(pdMS_TO_TICKS(10));                        // 10 ms Pause – verhindert WDT-Reset
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
  Serial.print("\n[PANIC] FreeRTOS-Fehler: ");
  Serial.println(what);
  display.clear();
  zeigeZ10C(64,  8, "RTOS FEHLER");
  zeigeZ10C(64, 24, what);
  zeigeZ10C(64, 40, "Neustart in 3s");
  display.display();
  delay(3000);
  ESP.restart();
}

// =============================================================
//  setup()
// =============================================================
void setup() {
  Serial.begin(115200);
  Serial2.begin(9600, SERIAL_8N1, RXD2, TXD2);
  delay(2000);
  bTn_info();

  // ── NVR laden ────────────────────────────────────────────
  data.begin("varSafe", ReadWrite);
  bool varState = data.getBool("state", false);
  if (varState) {
    Serial.println("gespeicherte Variable aus dem NVR lesen");
    readNVR();
  } else {
    data.putBool("state", true); // Erststart-Flag dauerhaft setzen
    writeNVR();
  }
  data.end();

  // ── Display init (wird auch von runWifiConfigServer genutzt) ─
  display.init();
  display.flipScreenVertically();
  display.clear();
  zeigeZ10C(64, 16, PGMInfo);
  display.display();                                                                   // Versionsstring anzeigen

  // ── WiFi-Credentials aus NVR laden ───────────────────────
  // Erster Start oder NVR-Flag gelöscht (z.B. via T0 auf Info):
  // → WiFi-Konfigurator starten (blockiert bis Neustart).
  if (!loadWifiCredentials()) {
    Serial.println("[WiFi-Config] Keine Zugangsdaten – starte Konfigurator");
    runWifiConfigServer();   // kehrt nicht zurück (ESP.restart am Ende)
  }
  webLogf("[WiFi] Credentials geladen: SSID=%s", sta_ssid);

  // ── NTP ──────────────────────────────────────────────────
  sntp_set_time_sync_notification_cb(timeavailable);
  configTime(0, 0, MY_NTP_SERVER);
  setenv("TZ", MY_TZ, 1);
  tzset();

  // ── WiFi verbinden ───────────────────────────────────────
  display.clear();
  zeigeZ10C(64, 8, PGMInfo);
  zeigeZ16C(64, 32, "warte auf");
  zeigeZ16C(64, 49, "WLAN ...");
  display.display();                                                                   // WiFi-Wartebildschirm anzeigen
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.begin(sta_ssid, sta_psk);                                                        // Laufzeit-Credentials aus NVR
  Serial.println("\nwarte auf WiFi");
  bool wifiConnected = false;                                                            // ersetzt goto wifi_skip
  {
    uint32_t t0 = millis();
    while (WiFi.status() != WL_CONNECTED) {
      if (millis() - t0 >= SETUP_WIFI_TIMEOUT_MS) {
        Serial.println("\nWiFi Timeout – starte ohne WLAN");
        cleanTXT(0, 32, 128, 32);
        zeigeZ16C(64, 32, "kein WLAN");
        zeigeZ10C(64, 49, "weiter ohne NTP");
        display.display();                                                             // Timeout-Meldung anzeigen
        delay(2000);
        break;                                                                           // wifiConnected bleibt false → NTP-Block wird übersprungen
      }
      delay(500);
      Serial.print(".");
    }
    wifiConnected = (WiFi.status() == WL_CONNECTED);
  }
  if (wifiConnected) {
    Serial.printf("\nWiFi connected – IP: %s  Log: http://%s:%u\n",
                  WiFi.localIP().toString().c_str(),
                  WiFi.localIP().toString().c_str(),
                  (unsigned)WEBLOG_PORT);                  // letzte Serial-Ausgabe nach WiFi
    webLog("[WiFi] connected");
    webLogf("[WiFi] IP: %s", WiFi.localIP().toString().c_str());

    // ── NTP warten ─────────────────────────────────────────
    cleanTXT(0, 49, 128, 15);
    zeigeZ16C(64, 49, "NTP ...");
    display.display();                                                                 // NTP-Wartemeldung anzeigen
    webLog("[NTP] warte auf Synchronisation ...");
    {
      uint32_t t0 = millis();
      while (timeinfo.tm_year < 71) {
        if (millis() - t0 >= SETUP_NTP_TIMEOUT_MS) {
          webLog("[NTP] Timeout – Uhr nicht gestellt");
          cleanTXT(0, 49, 128, 15);
          zeigeZ10C(64, 49, "NTP Timeout");
          display.display();                                                           // NTP-Timeout-Meldung anzeigen
          delay(2000);
          break;
        }
        showTime();
        delay(500);
        // kein Web-Log für Fortschrittspunkte
      }
    }
    webLog("[NTP] Synchronisation OK");
  }

  // ── GPIO ─────────────────────────────────────────────────
  pinMode(S1, INPUT_PULLUP);
  pinMode(S2, INPUT_PULLUP);
  pinMode(S3, INPUT_PULLUP);
  pinMode(E1, OUTPUT);
  pinMode(E2, OUTPUT);
  pinMode(E3, OUTPUT);

  // ── FreeRTOS Objekte ─────────────────────────────────────
  webLogMutex  = xSemaphoreCreateMutex();               // Ring-Puffer-Schutz für webLog()
  inputQueue   = xQueueCreate(32, sizeof(uint8_t));
  displayMutex = xSemaphoreCreateMutex();
  playerMutex  = xSemaphoreCreateMutex();
  nvrSemaphore = xSemaphoreCreateBinary();
  if (!webLogMutex)  rtosPanic("webLogMutex");
  if (!inputQueue)   rtosPanic("inputQueue");
  if (!displayMutex) rtosPanic("displayMutex");
  if (!playerMutex)  rtosPanic("playerMutex");
  if (!nvrSemaphore) rtosPanic("nvrSemaphore");

  // ── Taster-Interrupts ────────────────────────────────────
  attachInterrupt(S1, isrS1, FALLING);
  attachInterrupt(S2, isrS2, FALLING);
  attachInterrupt(S3, isrS3, FALLING);

  // ── DFPlayer ─────────────────────────────────────────────
  cleanTXT(0, 49, 128, 15);
  zeigeZ16C(64, 49, "Sound ...");
  display.display();                                                                   // DFPlayer-Initialisierung anzeigen
  if (player.begin(Serial2, true, true)) {
    delay(3000);
    webLog("[DFPlayer] Serial2 OK");
    player.volume(vol);
    player.EQ(DFPLAYER_EQ_BASS);
    player.playFolder(2, 1);
    delay(1000);
    playerStatus = 1;
  } else {
    webLog("[DFPlayer] Verbindung fehlgeschlagen!");
  }

  // ── Startseite ───────────────────────────────────────────
  snprintf(datum_WiFi,  sizeof(datum_WiFi), "%04u%02u%02u", timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday);
  snprintf(zeit_WiFi,   sizeof(zeit_WiFi),  "%02u:%02u:%02u", timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
  showTime();
  snprintf(str_a1,      sizeof(str_a1),      "%02u:%02u", a1_hour, a1_min);
  snprintf(str_a2,      sizeof(str_a2),      "%02u:%02u", a2_hour, a2_min);
  snprintf(str_s1,      sizeof(str_s1),      "%03u",      sound1_selected);
  snprintf(str_s2,      sizeof(str_s2),      "%03u",      sound2_selected);
  snprintf(str_s1_play, sizeof(str_s1_play), "%03u",      sound1_assigned);
  snprintf(str_s2_play, sizeof(str_s2_play), "%03u",      sound2_assigned);
  snprintf(str_vol,     sizeof(str_vol),     "%02u",      vol);
  snprintf(str_cot,     sizeof(str_cot),     "%02u",      cuckoo_onTime);
  snprintf(str_coff,    sizeof(str_coff),    "%02u",      cuckoo_offTime);
  uiTransition(UI_CLOCK);                                                               // initialer Zustand + Bildschirm

  // ── MP3-Anzahl ───────────────────────────────────────────
  {
    uint32_t t0 = millis();
    while (mp3Count < 1) {
      if (millis() - t0 >= SETUP_MP3_TIMEOUT_MS) {
        webLog("[DFPlayer] Timeout – mp3Count unbekannt");
        mp3Count = 99;                                                                   // Fallback: MP3-Auswahl bis Datei 99 erlauben
        break;
      }
      int16_t c = player.readFileCounts();
      if (c > 0) mp3Count = c - 1;                                                       // c==0 → mp3Count bleibt 0, kein uint8_t-Unterlauf auf 255
    }
  }
  snprintf(str_mp3, sizeof(str_mp3), "%03u", mp3Count);
  snprintf(str_reset, sizeof(str_reset), "%04lu", (unsigned long)resetCount);            // rechtsbündig 4-stellig
  webLogf("[DFPlayer] mp3Count: %d", mp3Count);

  // ── FreeRTOS Tasks starten ───────────────────────────────
  if (xTaskCreatePinnedToCore(touchTask,    "touchTask",    STACK_TOUCH, nullptr, 2, &hTouchTask,   0) != pdPASS) rtosPanic("touchTask");   // Core 0, Prio 2
  if (xTaskCreatePinnedToCore(alarmTask,    "alarmTask",    STACK_ALARM, nullptr, 2, &hAlarmTask,   0) != pdPASS) rtosPanic("alarmTask");   // Core 0, Prio 2 – getrennt von inputTask
  if (xTaskCreatePinnedToCore(wifiTask,     "wifiTask",     STACK_WIFI, nullptr, 1, &hWifiTask,    0) != pdPASS) rtosPanic("wifiTask");    // Core 0, Prio 1
  if (xTaskCreatePinnedToCore(nvrTask,      "nvrTask",      STACK_NVR, nullptr, 1, &hNvrTask,     0) != pdPASS) rtosPanic("nvrTask");     // Core 0, Prio 1
  if (xTaskCreatePinnedToCore(stackMonTask, "stackMonTask", STACK_STACKMON, nullptr, 1, nullptr,          0) != pdPASS) rtosPanic("stackMonTask"); // Core 0, Prio 1
  if (xTaskCreatePinnedToCore(watchdogTask, "watchdogTask", STACK_WATCHDOG, nullptr, 1, &hWatchdogTask,   0) != pdPASS) rtosPanic("watchdogTask"); // Core 0, Prio 1
  if (xTaskCreatePinnedToCore(inputTask,    "inputTask",    STACK_INPUT, nullptr, 2, &hInputTask,   1) != pdPASS) rtosPanic("inputTask");   // Core 1, Prio 2
  if (xTaskCreatePinnedToCore(displayTask,  "displayTask",  STACK_DISPLAY, nullptr, 1, &hDisplayTask, 1) != pdPASS) rtosPanic("displayTask"); // Core 1, Prio 1
  if (xTaskCreatePinnedToCore(webLogTask,   "webLogTask",   STACK_WEBLOG,  nullptr, 1, nullptr,       0) != pdPASS) rtosPanic("webLogTask");  // Core 0, Prio 1

  // ── Hardware Task Watchdog Timer (TWDT) ──────────────
  // Initialisierung NACH Task-Start: Tasks müssen bereits laufen bevor
  // sie sich anmelden können (esp_task_wdt_add in Task-Funktion selbst).
  // trigger_panic=true: TWDT-Ablauf erzeugt Backtrace + Reset statt stiller Neustart.
  // Timeout WDT_HARDWARE_MS kürzer als Software-Watchdog WDG_TIMEOUT_MS:
  // Hardware greift bei echtem CPU-Lock, Software bei logischem Freeze.
  const esp_task_wdt_config_t twdt_cfg = {
    .timeout_ms    = WDT_HARDWARE_MS,  // aus SysConf_9v3.h
    .idle_core_mask = 0,               // Idle-Tasks nicht überwachen
    .trigger_panic  = true,            // Backtrace + Reset bei Ablauf
  };
  esp_task_wdt_init(&twdt_cfg);        // TWDT initialisieren
  webLogf("[TWDT] Hardware Watchdog aktiv (%u ms)", (unsigned)WDT_HARDWARE_MS);
}



// =============================================================
//  loop() – leer
// =============================================================
void loop() {
  vTaskDelete(nullptr);
}
