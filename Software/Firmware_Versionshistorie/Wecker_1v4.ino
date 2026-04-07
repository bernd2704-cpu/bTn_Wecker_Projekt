// bTn Wecker mit OLED-Anzeige und MP3-Player
// Basis: bTn_Alarm_1v4 – FreeRTOS + State Machine
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
//  S3 (beliebig) ─────────────────────────▶ UI_INFO
//  S3 (von INFO) ──────────────────────────▶ UI_CLOCK
//
//  Alarm-State-Machine  (alarmTask)
//  ALARM_IDLE ──── Alarmzeit erreicht ──▶ ALARM_RUNNING
//  ALARM_RUNNING ── MP3 beendet (delay6) ─▶ ALARM_IDLE
//
//  Kuckuck-State-Machine  (alarmTask)
//  CUCKOO_IDLE ──── t_min==0, cuckoo_on ──▶ CUCKOO_RUNNING
//  CUCKOO_RUNNING ── delay4 abgelaufen ────▶ CUCKOO_IDLE
//
// ─── Task-Architektur (unverändert gegenüber v1) ─────────────
//  touchTask   Core 0  Pri 2  ESP-IDF touch_pad_* → inputQueue
//  inputTask   Core 1  Pri 2  Dispatch → UI-State-Machine
//  displayTask Core 1  Pri 1  Zeitanzeige, Auto-Rückkehr
//  alarmTask   Core 1  Pri 2  Alarm- + Kuckuck-State-Machine
//  wifiTask    Core 0  Pri 1  WiFi-Reconnect
//  nvrTask     Core 0  Pri 1  Flash-Sicherung bei Änderung
// ─────────────────────────────────────────────────────────────

String PGMInfo = "bTn_Alarm_1v4.ino";

// ── Bibliotheken ─────────────────────────────────────────────
#include <WiFi.h>
#include <time.h>
#include <esp_sntp.h>
#include <SSD1306Wire.h>
#include <DFRobotDFPlayerMini.h>
#include <Preferences.h>
#include <driver/touch_pad.h>
#include <freertos/semphr.h>

// ── Konfiguration ────────────────────────────────────────────
#include "SystemConfig.h"

// ── Touch-Task ───────────────────────────────────────────────

static const touch_pad_t TOUCH_PADS[4] = {
  TOUCH_PAD_NUM0,   // T0 – GPIO4
  TOUCH_PAD_NUM2,   // T2 – GPIO2
  TOUCH_PAD_NUM3,   // T3 – GPIO15
  TOUCH_PAD_NUM4    // T4 – GPIO13
};

// ── State-Machine-Typen ──────────────────────────────────────
// Enum-Werte 0–6 entsprechen direkt den menu()-Seitennummern.
enum UiState : uint8_t {
  UI_CLOCK  = 0,   // Seite 0: Zeitanzeige
  UI_ALARM1 = 1,   // Seite 1: Alarm 1 einstellen
  UI_ALARM2 = 2,   // Seite 2: Alarm 2 einstellen
  UI_SOUND1 = 3,   // Seite 3: Sound 1 wählen
  UI_SOUND2 = 4,   // Seite 4: Sound 2 wählen
  UI_FUNCS  = 5,   // Seite 5: Funktionen wählen
  UI_INFO   = 6    // Seite 6: Info (nur via S3 erreichbar)
};

enum AlarmState  : uint8_t { ALARM_IDLE,  ALARM_RUNNING  };
enum CuckooState : uint8_t { CUCKOO_IDLE, CUCKOO_RUNNING };

UiState     uiState     = UI_CLOCK;
AlarmState  alarmState  = ALARM_IDLE;
CuckooState cuckooState = CUCKOO_IDLE;

// ── FreeRTOS Objekte ─────────────────────────────────────────
static QueueHandle_t     inputQueue   = nullptr;
static SemaphoreHandle_t displayMutex = nullptr;
static SemaphoreHandle_t nvrSemaphore = nullptr;

// ── Hardware ─────────────────────────────────────────────────
SSD1306Wire         display(0x3C, SDA, SCL, GEOMETRY_128_64);
DFRobotDFPlayerMini player;
Preferences         data;

// ── Zeit / Datum ─────────────────────────────────────────────
time_t  now;
tm      tm;
char    datum[11];
char    zeit[9];
char    datum_sync[9];
char    zeit_sync[9];
char    datum_WiFi[9];
char    zeit_WiFi[9];
uint8_t t_hour;
uint8_t t_min;
uint8_t t_sec;
uint8_t t_sec_alt;

// ── Alarm ────────────────────────────────────────────────────
bool    a1_on   = true;
uint8_t a1_hour = 6;
uint8_t a1_min  = 0;
char    str_a1[6];
bool    a2_on   = true;
uint8_t a2_hour = 6;
uint8_t a2_min  = 0;
char    str_a2[6];
bool    ax_live = false;

uint32_t t_start4 = 0;
uint32_t t_start5 = 0;
uint32_t t_start6 = 0;
uint32_t t_start7 = 0;

// ── Sound ────────────────────────────────────────────────────
bool    sound1_on   = false;
uint8_t sound1      = 1;
char    str_s1[4];
uint8_t sound1_play = 1;
char    str_s1_play[4];
bool    sound2_on   = false;
uint8_t sound2      = 1;
char    str_s2[4];
uint8_t sound2_play = 1;
char    str_s2_play[4];
uint8_t vol         = 9;
uint8_t max_vol     = 25;
char    str_vol[3];
int16_t playerStatus;
int16_t mp3Count    = 0;
char    str_mp3[4];

bool       varState;
volatile bool safeChange = false;

// ── Taster Toggle-Status ─────────────────────────────────────
bool          S2_SW = false;                                                             // Toggle-Status Zugschalter

// ── Funktion-Vorwahl ─────────────────────────────────────────
bool    cuckoo_on  = false;
bool    light_on   = true;
bool    wheel_on   = false;

// pageselect: spiegelt (uint8_t)uiState – wird von checkboxAlarm/Sound genutzt
uint8_t pageselect = 0;

// ── Taster-Debounce ──────────────────────────────────────────
static uint32_t lastBtnMs[3] = {};                                                       // S1, S2, S3



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
  localtime_r(&now, &tm);
  sprintf(datum, "%02u.%02u.%04u", tm.tm_mday, tm.tm_mon + 1, tm.tm_year + 1900);
  sprintf(zeit,  "%02u:%02u:%02u",  tm.tm_hour, tm.tm_min,    tm.tm_sec);
  t_sec  = tm.tm_sec;
  t_min  = tm.tm_min;
  t_hour = tm.tm_hour;
}

void timeavailable(struct timeval *t) {
  time(&now);
  localtime_r(&now, &tm);
  sprintf(datum_sync, "%04u%02u%02u", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
  sprintf(zeit_sync,  "%02u:%02u:%02u", tm.tm_hour, tm.tm_min, tm.tm_sec);
}



// =============================================================
//  Display-Hilfsfunktionen
//  Unverändert. Alle Aufrufe nur unter displayMutex (außer setup()).
// =============================================================

void cleanTXT(int xPos, int yPos, int dx, int dy) {
  display.setColor(BLACK);
  display.fillRect(xPos, yPos, dx, dy);
  display.setColor(WHITE);
}

void zeigeZ10C(int xPos, int yPos, String TXT) {
  display.setTextAlignment(TEXT_ALIGN_CENTER);
  display.setFont(ArialMT_Plain_10);
  display.drawString(xPos, yPos, TXT);
  display.display();
}

void zeigeZ10L(int xPos, int yPos, String TXT) {
  display.setTextAlignment(TEXT_ALIGN_LEFT);
  display.setFont(ArialMT_Plain_10);
  display.drawString(xPos, yPos, TXT);
  display.display();
}

void zeigeZ16C(int xPos, int yPos, String TXT) {
  display.setTextAlignment(TEXT_ALIGN_CENTER);
  display.setFont(ArialMT_Plain_16);
  display.drawString(xPos, yPos, TXT);
  display.display();
}

void zeigeZ16L(int xPos, int yPos, String TXT) {
  display.setTextAlignment(TEXT_ALIGN_LEFT);
  display.setFont(ArialMT_Plain_16);
  display.drawString(xPos, yPos, TXT);
  display.display();
}

// Alarm-Checkboxen (nutzt pageselect – wird von uiTransition synchron gesetzt)
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
        player.playFolder(1, sound1_play);
      } else {
        display.drawRect(68, 38, 7, 7); display.display();
        player.stop();
      }
      break;
    case 4:
      if (sound2_on) {
        display.fillRect(68, 55, 7, 7); display.display();
        sound2_play = sound2;
        player.playFolder(1, sound2_play);
      } else {
        display.drawRect(68, 55, 7, 7); display.display();
        player.stop();
      }
      break;
  }
}

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
      player.stop();
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
      zeigeZ10C(64, 16, PGMInfo);
      zeigeZ10L(1,  30, "WiFi");
      zeigeZ10L(28, 30, datum_WiFi);
      zeigeZ10L(82, 30, zeit_WiFi);
      zeigeZ10L(1,  42, "NTP");
      zeigeZ10L(28, 42, datum_sync);
      zeigeZ10L(82, 42, zeit_sync);
      zeigeZ10L(1,  54, "MP3");
      zeigeZ10L(28, 54, str_mp3);
      break;
  }
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
  data.putInt ("sound1_play", sound1_play);
  data.putInt ("sound2_play", sound2_play);
  data.putBool("cuckoo_on",   cuckoo_on);
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
  sound1_play = data.getInt ("sound1_play",  sound1_play);
  sound2_play = data.getInt ("sound2_play",  sound2_play);
  cuckoo_on   = data.getBool("cuckoo_on",    cuckoo_on);
  light_on    = data.getBool("light_on",     light_on);
  wheel_on    = data.getBool("wheel_on",     wheel_on);
  vol         = data.getInt ("vol",          vol);
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
  t_start5   = millis();                                                                 // Auto-Rückkehr-Timer zurücksetzen
  menu(pageselect);                                                                      // Bildschirm zeichnen (Entry-Aktion)
}

// ── Per-State-Handler: T2/T3/T4 ─────────────────────────────
// Jeder Handler gibt den Folge-Zustand zurück.
// Bleibt der Zustand gleich, wird uiTransition NICHT aufgerufen
// (kein unnötiges Neuzeichnen; nur partielle Display-Updates).

// — UI_CLOCK: Lautstärke mit T3/T4 ──────────────────────────
static UiState onClock(uint8_t evt) {
  switch (evt) {
    case EVT_T3:                                                                         // Lautstärke –
      if (vol > 0) {
        vol--;
        player.volume(vol);
        sprintf(str_vol, "%02u", vol);
        cleanTXT(115, 17, 15, 10);
        zeigeZ10L(115, 17, str_vol);
        safeChange = true;
      }
      break;
    case EVT_T4:                                                                         // Lautstärke +
      if (vol < max_vol) {
        vol++;
        player.volume(vol);
        sprintf(str_vol, "%02u", vol);
        cleanTXT(115, 17, 15, 10);
        zeigeZ10L(115, 17, str_vol);
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
      sprintf(str_a1, "%02u:%02u", a1_hour, a1_min);
      cleanTXT(82, 34, 46, 13);
      zeigeZ16C(105, 32, str_a1);
      safeChange = true;
      break;
    case EVT_T4:                                                                         // Minute +
      if (a1_min < 59) { a1_min++; } else { a1_min = 0; }
      sprintf(str_a1, "%02u:%02u", a1_hour, a1_min);
      cleanTXT(82, 34, 46, 13);
      zeigeZ16C(105, 32, str_a1);
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
      sprintf(str_a2, "%02u:%02u", a2_hour, a2_min);
      cleanTXT(82, 34, 46, 13);
      zeigeZ16C(105, 49, str_a2);
      safeChange = true;
      break;
    case EVT_T4:                                                                         // Minute +
      if (a2_min < 59) { a2_min++; } else { a2_min = 0; }
      sprintf(str_a2, "%02u:%02u", a2_hour, a2_min);
      cleanTXT(82, 51, 46, 13);
      zeigeZ16C(105, 49, str_a2);
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
        sprintf(str_s1_play, "%03u", sound1);
        cleanTXT(42, 17, 20, 10);
        zeigeZ10L(42, 17, str_s1_play);
      }
      break;
    case EVT_T3:                                                                         // Sound 1 +
      if (sound1 < mp3Count) { sound1++; } else { sound1 = 1; }
      sprintf(str_s1, "%03u", sound1);
      cleanTXT(82, 34, 46, 13);
      zeigeZ16C(105, 32, str_s1);
      sound1_on = false;
      checkboxSound();
      safeChange = true;
      break;
    case EVT_T4:                                                                         // Sound 1 –
      if (sound1 > 1) { sound1--; } else { sound1 = mp3Count; }
      sprintf(str_s1, "%03u", sound1);
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
        sprintf(str_s2_play, "%03u", sound2);
        cleanTXT(108, 17, 20, 10);
        zeigeZ10L(108, 17, str_s2_play);
      }
      break;
    case EVT_T3:                                                                         // Sound 2 +
      if (sound2 < mp3Count) { sound2++; } else { sound2 = 1; }
      sprintf(str_s2, "%03u", sound2);
      cleanTXT(82, 51, 46, 13);
      zeigeZ16C(105, 32, str_s2);
      sound2_on = false;
      checkboxSound();
      safeChange = true;
      break;
    case EVT_T4:                                                                         // Sound 2 –
      if (sound2 > 1) { sound2--; } else { sound2 = mp3Count; }
      sprintf(str_s2, "%03u", sound2);
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

// — UI_INFO: keine Touch-Aktionen ────────────────────────────
static UiState onInfo(uint8_t evt) {
  (void)evt;
  return UI_INFO;                                                                        // Nur S3 verlässt Info (global behandelt)
}

// ── Haupt-Dispatcher ─────────────────────────────────────────
// Behandelt globale Events, dann per-State-Handler.
// Gibt den Folge-Zustand zurück; Aufrufer ruft uiTransition wenn nötig.
static UiState uiDispatch(UiState s, uint8_t evt) {

  // ── T0: Seitenwechsel (globaler Zyklus, State-unabhängig) ──
  if (evt == EVT_T0) {
    static const UiState cycle[] = {
      UI_ALARM1,   // von UI_CLOCK
      UI_ALARM2,   // von UI_ALARM1
      UI_SOUND1,   // von UI_ALARM2
      UI_SOUND2,   // von UI_SOUND1
      UI_FUNCS,    // von UI_SOUND2
      UI_CLOCK,    // von UI_FUNCS
      UI_CLOCK     // von UI_INFO → zurück zu Uhr
    };
    return cycle[(uint8_t)s];
  }

  // ── S3: Info-Seite ein/aus (global, mit Taster-Entprellung) ─
  if (evt == EVT_S3) {
    uint32_t t_now = millis();
    if (t_now - lastBtnMs[2] >= delay3) {
      lastBtnMs[2] = t_now;
      return (s == UI_INFO) ? UI_CLOCK : UI_INFO;                                       // Toggle Info
    }
    return s;
  }

  // ── S1: Alarm/Sound stoppen oder Kuckuck einmalig ───────────
  if (evt == EVT_S1) {
    uint32_t t_now = millis();
    if (t_now - lastBtnMs[0] >= delay3) {
      lastBtnMs[0] = t_now;
      playerStatus = player.readState();
      while (playerStatus == -1) {
        playerStatus = player.readState();
        vTaskDelay(pdMS_TO_TICKS(1));
      }
      if (playerStatus > 0) {
        player.stop();
        digitalWrite(E2, LOW);
        digitalWrite(E3, LOW);
        ax_live     = false;
        alarmState  = ALARM_IDLE;                                                        // Alarm-State zurücksetzen
      } else {
        digitalWrite(E1, HIGH);
        t_start4    = millis();
        cuckooState = CUCKOO_RUNNING;                                                    // Kuckuck-State setzen
      }
    }
    return s;                                                                            // kein UI-Zustandswechsel
  }

  // ── S2: Zugschalter – Licht + Mühlrad EIN/AUS ───────────────
  if (evt == EVT_S2) {
    uint32_t t_now = millis();
    if (t_now - lastBtnMs[1] >= delay3) {
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
    return s;
  }

  // ── T2/T3/T4: State-spezifisch ──────────────────────────────
  switch (s) {
    case UI_CLOCK:  return onClock (evt);
    case UI_ALARM1: return onAlarm1(evt);
    case UI_ALARM2: return onAlarm2(evt);
    case UI_SOUND1: return onSound1(evt);
    case UI_SOUND2: return onSound2(evt);
    case UI_FUNCS:  return onFuncs (evt);
    case UI_INFO:   return onInfo  (evt);
    default:        return s;
  }
}



// =============================================================
//  System-State-Machines  (werden von alarmTask aufgerufen)
// =============================================================

// ── Alarm-State-Machine ──────────────────────────────────────
static void runAlarmMachine() {
  switch (alarmState) {

    case ALARM_IDLE:
      // Alarm 1 prüfen
      if (a1_on && t_sec == 0 && t_min == a1_min && t_hour == a1_hour) {
        player.playFolder(1, sound1_play);
        if (wheel_on) { digitalWrite(E2, HIGH); }
        if (light_on) { digitalWrite(E3, HIGH); }
        if (wheel_on || light_on) {
          t_start6   = millis();
          ax_live    = true;
          alarmState = ALARM_RUNNING;                                                    // → ALARM_RUNNING
        }
      }
      // Alarm 2 prüfen
      if (a2_on && t_sec == 0 && t_min == a2_min && t_hour == a2_hour) {
        player.playFolder(1, sound2_play);
        if (wheel_on) { digitalWrite(E2, HIGH); }
        if (light_on) { digitalWrite(E3, HIGH); }
        if (wheel_on || light_on) {
          t_start6   = millis();
          ax_live    = true;
          alarmState = ALARM_RUNNING;                                                    // → ALARM_RUNNING
        }
      }
      break;

    case ALARM_RUNNING:
      if (delayFunction(t_start6, delay6)) {
        playerStatus = player.readState();
        playerStatus = player.readState();                                               // zweite Abfrage → korrekter Status
        t_start6 = millis();
        if (playerStatus < 1) {                                                          // MP3 beendet
          if (wheel_on) { digitalWrite(E2, LOW); }
          if (light_on) { digitalWrite(E3, LOW); }
          ax_live    = false;
          alarmState = ALARM_IDLE;                                                       // → ALARM_IDLE
        }
      }
      break;
  }
}

// ── Kuckuck-State-Machine ────────────────────────────────────
static void runCuckooMachine() {
  switch (cuckooState) {

    case CUCKOO_IDLE:
      if (t_min == 0 && t_sec == 0 && cuckoo_on) {
        digitalWrite(E1, HIGH);
        t_start4    = millis();
        cuckooState = CUCKOO_RUNNING;                                                    // → CUCKOO_RUNNING
      }
      break;

    case CUCKOO_RUNNING:
      if (delayFunction(t_start4, delay4)) {
        digitalWrite(E1, LOW);
        cuckooState = CUCKOO_IDLE;                                                       // → CUCKOO_IDLE
      }
      break;
  }
}



// =============================================================
//  Task 1 – touchTask  (Core 0, Pri 2)
//  ESP-IDF touch_pad_* Polling → inputQueue
// =============================================================
static void touchTask(void *pvParam) {
  touch_pad_init();
  touch_pad_set_voltage(TOUCH_HVOLT_2V7, TOUCH_LVOLT_0V5, TOUCH_HVOLT_ATTEN_1V);
  for (int i = 0; i < 4; i++) { touch_pad_config(TOUCH_PADS[i], 0); }

  vTaskDelay(pdMS_TO_TICKS(300));

  uint16_t baseline[4];
  for (int i = 0; i < 4; i++) {
    touch_pad_read(TOUCH_PADS[i], &baseline[i]);
    Serial.printf("Touch Pad %d  Baseline: %u  Threshold: %u\n",
                  i, baseline[i],
                  baseline[i] > TOUCH_DROP ? baseline[i] - TOUCH_DROP : 0);
  }

  static const uint8_t EVT_ID[4] = { EVT_T0, EVT_T2, EVT_T3, EVT_T4 };
  bool       wasPressed[4]   = {};
  TickType_t lastFireTick[4] = {};
  const TickType_t debounce  = pdMS_TO_TICKS(delay1);

  while (true) {
    uint16_t   val;
    TickType_t now = xTaskGetTickCount();
    for (int i = 0; i < 4; i++) {
      touch_pad_read(TOUCH_PADS[i], &val);
      uint16_t thr     = baseline[i] > TOUCH_DROP ? baseline[i] - TOUCH_DROP : 0;
      bool     pressed = (val < thr);
      if (pressed && !wasPressed[i] && (now - lastFireTick[i]) >= debounce) {
        lastFireTick[i] = now;
        uint8_t evt = EVT_ID[i];
        xQueueSend(inputQueue, &evt, 0);
      }
      wasPressed[i] = pressed;
    }
    vTaskDelay(pdMS_TO_TICKS(TOUCH_POLL_MS));
  }
}



// =============================================================
//  Taster-ISRs  (S1, S2, S3)
// =============================================================
void IRAM_ATTR isrS1() { uint8_t e=EVT_S1; BaseType_t hp=pdFALSE; xQueueSendFromISR(inputQueue,&e,&hp); portYIELD_FROM_ISR(hp); }
void IRAM_ATTR isrS2() { uint8_t e=EVT_S2; BaseType_t hp=pdFALSE; xQueueSendFromISR(inputQueue,&e,&hp); portYIELD_FROM_ISR(hp); }
void IRAM_ATTR isrS3() { uint8_t e=EVT_S3; BaseType_t hp=pdFALSE; xQueueSendFromISR(inputQueue,&e,&hp); portYIELD_FROM_ISR(hp); }



// =============================================================
//  Task 2 – inputTask  (Core 1, Pri 2)
//
//  Reiner Dispatch-Loop:
//    1. Event aus inputQueue lesen
//    2. uiDispatch() → Folge-Zustand
//    3. Bei Zustandswechsel: uiTransition() (zeichnet Bildschirm)
//    4. safeChange → nvrSemaphore
// =============================================================
static void inputTask(void *pvParam) {
  uint8_t evt;

  while (true) {
    if (xQueueReceive(inputQueue, &evt, pdMS_TO_TICKS(50)) != pdTRUE) {
      // kein Event – nur NVR prüfen
      if (safeChange) { safeChange = false; xSemaphoreGive(nvrSemaphore); }
      continue;
    }

    if (xSemaphoreTake(displayMutex, pdMS_TO_TICKS(100)) != pdTRUE) {
      continue;                                                                          // Display belegt – Event verwerfen
    }

    UiState next = uiDispatch(uiState, evt);                                            // State-Machine auswerten
    if (next != uiState) {
      uiTransition(next);                                                               // Zustand wechseln + Bildschirm zeichnen
    }

    xSemaphoreGive(displayMutex);

    if (safeChange) { safeChange = false; xSemaphoreGive(nvrSemaphore); }
  }
}



// =============================================================
//  Task 3 – displayTask  (Core 1, Pri 1)
//
//  Uhrzeit auf Seite 0 aktualisieren (delay2 = 300 ms).
//  Mitternacht: Datum neu zeichnen.
//  Auto-Rückkehr zu UI_CLOCK nach delay5 (30 s).
// =============================================================
static void displayTask(void *pvParam) {
  while (true) {
    showTime();

    if (xSemaphoreTake(displayMutex, pdMS_TO_TICKS(100)) == pdTRUE) {

      if (uiState == UI_CLOCK) {
        if (t_hour == 0 && t_min == 0 && t_sec < 2) {
          uiTransition(UI_CLOCK);                                                        // Mitternacht: Seite 0 komplett neu
        } else if (t_sec != t_sec_alt) {
          t_sec_alt = t_sec;
          cleanTXT(20, 0, 120, 16);
          zeigeZ16C(64, 0, zeit);                                                        // nur Uhrzeit partiell aktualisieren
        }
      }

      if (uiState != UI_CLOCK && delayFunction(t_start5, delay5)) {
        uiTransition(UI_CLOCK);                                                          // Auto-Rückkehr
      }

      xSemaphoreGive(displayMutex);
    }

    vTaskDelay(pdMS_TO_TICKS(delay2));
  }
}



// =============================================================
//  Task 4 – alarmTask  (Core 1, Pri 2)
//
//  Führt Alarm- und Kuckuck-State-Machine aus.
// =============================================================
static void alarmTask(void *pvParam) {
  while (true) {
    runAlarmMachine();
    runCuckooMachine();
    vTaskDelay(pdMS_TO_TICKS(500));
  }
}



// =============================================================
//  Task 5 – wifiTask  (Core 0, Pri 1)
// =============================================================
static void wifiTask(void *pvParam) {
  while (true) {
    if (WiFi.status() != WL_CONNECTED && delayFunction(t_start7, delay7)) {
      sprintf(datum_WiFi, "%04u%02u%02u", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
      sprintf(zeit_WiFi,  "%02u:%02u:%02u", tm.tm_hour, tm.tm_min, tm.tm_sec);
      WiFi.disconnect();
      WiFi.reconnect();
      t_start7 = millis();
    }
    vTaskDelay(pdMS_TO_TICKS(delay7));
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
//  setup()
// =============================================================
void setup() {
  Serial.begin(115200);
  Serial2.begin(9600, SERIAL_8N1, RXD2, TXD2);
  delay(2000);
  bTn_info();

  // ── NVR laden ────────────────────────────────────────────
  data.begin("varSafe", ReadWrite);
  varState = data.getBool("state", false);
  if (varState) {
    Serial.println("gespeicherte Variable aus dem NVR lesen");
    readNVR();
  } else {
    varState = true;
    data.putBool("state", varState);
    writeNVR();
  }
  data.end();

  // ── NTP ──────────────────────────────────────────────────
  sntp_set_time_sync_notification_cb(timeavailable);
  configTime(0, 0, MY_NTP_SERVER);
  setenv("TZ", MY_TZ, 1);
  tzset();

  // ── Display ──────────────────────────────────────────────
  display.init();
  display.flipScreenVertically();
  display.clear();
  zeigeZ10C(64, 16, PGMInfo);
  zeigeZ16C(64, 32, "warte auf");
  zeigeZ16C(64, 49, "WLAN ...");

  // ── WiFi ─────────────────────────────────────────────────
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.begin(STA_SSID, STA_PSK);
  Serial.println("\nwarte auf WiFi");
  while (WiFi.status() != WL_CONNECTED) { delay(500); Serial.print("."); }
  Serial.println("\nWiFi connected");
  Serial.println(WiFi.localIP());

  // ── NTP warten ───────────────────────────────────────────
  cleanTXT(0, 49, 128, 15);
  zeigeZ16C(64, 49, "NTP ...");
  Serial.println("\nwarte auf NTP");
  while (tm.tm_year < 71) { showTime(); delay(500); Serial.print("."); }
  Serial.println("\nNTP connected");

  // ── GPIO ─────────────────────────────────────────────────
  pinMode(S1, INPUT_PULLUP);
  pinMode(S2, INPUT_PULLUP);
  pinMode(S3, INPUT_PULLUP);
  pinMode(E1, OUTPUT);
  pinMode(E2, OUTPUT);
  pinMode(E3, OUTPUT);

  // ── FreeRTOS Objekte ─────────────────────────────────────
  inputQueue   = xQueueCreate(32, sizeof(uint8_t));
  displayMutex = xSemaphoreCreateMutex();
  nvrSemaphore = xSemaphoreCreateBinary();

  // ── Taster-Interrupts ────────────────────────────────────
  attachInterrupt(S1, isrS1, FALLING);
  attachInterrupt(S2, isrS2, FALLING);
  attachInterrupt(S3, isrS3, FALLING);

  // ── DFPlayer ─────────────────────────────────────────────
  cleanTXT(0, 49, 128, 15);
  zeigeZ16C(64, 49, "Sound ...");
  if (player.begin(Serial2, true, true)) {
    delay(3000);
    Serial.println("\nDFPlayer Serial2 OK");
    player.volume(vol);
    player.EQ(DFPLAYER_EQ_BASS);
    player.playFolder(2, 1);
    delay(1000);
    playerStatus = 1;
  } else {
    Serial.println("\nConnecting to DFPlayer Mini failed!");
  }

  // ── Startseite ───────────────────────────────────────────
  sprintf(datum_WiFi,  "%04u%02u%02u", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
  sprintf(zeit_WiFi,   "%02u:%02u:%02u", tm.tm_hour, tm.tm_min, tm.tm_sec);
  showTime();
  sprintf(str_a1,      "%02u:%02u", a1_hour, a1_min);
  sprintf(str_a2,      "%02u:%02u", a2_hour, a2_min);
  sprintf(str_s1,      "%03u",      sound1);
  sprintf(str_s2,      "%03u",      sound2);
  sprintf(str_s1_play, "%03u",      sound1_play);
  sprintf(str_s2_play, "%03u",      sound2_play);
  sprintf(str_vol,     "%02u",      vol);
  uiTransition(UI_CLOCK);                                                               // initialer Zustand + Bildschirm

  // ── MP3-Anzahl ───────────────────────────────────────────
  while (mp3Count < 1) { mp3Count = player.readFileCounts() - 1; }
  sprintf(str_mp3, "%03u", mp3Count);
  Serial.printf("Anzahl mp3-Files: %d\n", mp3Count);

  // ── FreeRTOS Tasks starten ───────────────────────────────
  xTaskCreatePinnedToCore(touchTask,   "touchTask",   4096, nullptr, 2, nullptr, 0);
  xTaskCreatePinnedToCore(wifiTask,    "wifiTask",    3072, nullptr, 1, nullptr, 0);
  xTaskCreatePinnedToCore(nvrTask,     "nvrTask",     3072, nullptr, 1, nullptr, 0);
  xTaskCreatePinnedToCore(inputTask,   "inputTask",   6144, nullptr, 2, nullptr, 1);
  xTaskCreatePinnedToCore(displayTask, "displayTask", 4096, nullptr, 1, nullptr, 1);
  xTaskCreatePinnedToCore(alarmTask,   "alarmTask",   3072, nullptr, 2, nullptr, 1);
}



// =============================================================
//  loop() – leer
// =============================================================
void loop() {
  vTaskDelete(nullptr);
}
