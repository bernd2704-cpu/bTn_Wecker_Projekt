// bTn Wecker mit OLED-Anzeige und MP3-Player
// Basis: bTn_Alarm_3v05 – vollständig auf FreeRTOS umgestellt
// Boardverwalter: esp32 3.3.7 von Espressif Systems
//
// ─── Task-Architektur ────────────────────────────────────────
//  touchTask   Core 0  Pri 2  ESP-IDF touch_pad_* Polling
//                             → schickt Events in inputQueue
//  inputTask   Core 1  Pri 2  liest inputQueue + Taster-Flags
//                             → führt alle Eingabe-Aktionen aus
//                             → hält displayMutex bei Display-Zugriffen
//                             → gibt nvrSemaphore bei Einstellungsänderung
//  displayTask Core 1  Pri 1  Uhrzeit-Aktualisierung Seite 0
//                             → Auto-Rückkehr nach delay5
//  alarmTask   Core 1  Pri 2  alarmTrigger / cuckooTrigger / Nachlauf
//  wifiTask    Core 0  Pri 1  WiFi-Reconnect alle delay7 ms
//  nvrTask     Core 0  Pri 1  wartet auf nvrSemaphore → writeNVR()
// ─────────────────────────────────────────────────────────────

String PGMInfo = "bTn_Alarm_3v05.ino";                                                   // PGM-Name und Version

// ── Bibliotheken ─────────────────────────────────────────────
#include <WiFi.h>
#include <time.h>
#include <esp_sntp.h>
#include <SSD1306Wire.h>
#include <DFRobotDFPlayerMini.h>
#include <Preferences.h>
#include <driver/touch_pad.h>                                                            // ESP-IDF Touch-Pad API (Core-unabhängig)
#include <freertos/semphr.h>

// ── WiFi ─────────────────────────────────────────────────────
#define STA_SSID  "my_ssid"
#define STA_PSK   "my_passwrd"

// ── NTP ──────────────────────────────────────────────────────
#define MY_NTP_SERVER "pool.ntp.org"
#define MY_TZ         "CET-1CEST,M3.5.0/02,M10.5.0/03"

// ── DFPlayer ─────────────────────────────────────────────────
#define RXD2 16
#define TXD2 17

// ── Touch-Task Konfiguration ─────────────────────────────────
// touch_pad_read() liefert Entladewert (uint16_t).
// Ein Touch senkt den Wert. Auslösung wenn: wert < (baseline - TOUCH_DROP)
// Beim Start werden die Baseline-Werte per Serial ausgegeben → kalibrieren.
#define TOUCH_DROP    80                                                                  // Empfindlichkeit (Original: threshold = 35 Einheiten anderer Skala)
#define TOUCH_POLL_MS 10                                                                  // Abtastrate des Touch-Tasks in ms

// Mapping: logischer Index → ESP-IDF Pad
//   0 = T0 GPIO4  = TOUCH_PAD_NUM0
//   1 = T2 GPIO2  = TOUCH_PAD_NUM2
//   2 = T3 GPIO15 = TOUCH_PAD_NUM3
//   3 = T4 GPIO13 = TOUCH_PAD_NUM4
static const touch_pad_t TOUCH_PADS[4] = {
  TOUCH_PAD_NUM0, TOUCH_PAD_NUM2, TOUCH_PAD_NUM3, TOUCH_PAD_NUM4
};

// ── Eingabe-Event-IDs (inputQueue) ────────────────────────────
// Touch
#define EVT_T0  0
#define EVT_T2  1
#define EVT_T3  2
#define EVT_T4  3
// Taster
#define EVT_S1  4
#define EVT_S2  5
#define EVT_S3  6

// ── FreeRTOS Objekte ─────────────────────────────────────────
static QueueHandle_t     inputQueue   = nullptr;                                         // uint8_t Event-IDs von Touch + Tastern
static SemaphoreHandle_t displayMutex = nullptr;                                         // Mutex: exklusiver Display-Zugriff
static SemaphoreHandle_t nvrSemaphore = nullptr;                                         // Binärer Semaphor: löst NVR-Sicherung aus

// ── Hardware-Instanzen ────────────────────────────────────────
SSD1306Wire          display(0x3C, SDA, SCL, GEOMETRY_128_64);
DFRobotDFPlayerMini  player;
Preferences          data;

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

// ── Alarm ─────────────────────────────────────────────────────
bool    a1_on   = true;
uint8_t a1_hour = 6;
uint8_t a1_min  = 0;
char    str_a1[6];
bool    a2_on   = true;
uint8_t a2_hour = 6;
uint8_t a2_min  = 0;
char    str_a2[6];
bool    ax_live = false;

// ── Verzögerungskonstanten (ms) ───────────────────────────────
const uint32_t delay1 = 250;                                                             // Touch-Tasten Wiederholrate
const uint32_t delay2 = 300;                                                             // Zeitanzeige Seite 0
const uint32_t delay3 = 1000;                                                            // Taster-Entprellung / Wiederholrate
const uint32_t delay4 = 7500;                                                            // Kuckuck-Laufzeit
const uint32_t delay5 = 30000;                                                           // Auto-Rückkehr zu Seite 0
const uint32_t delay6 = 5000;                                                            // Alarm-Nachlauf Prüfintervall
const uint32_t delay7 = 3000;                                                            // WiFi-Reconnect Wiederholrate

// Laufzeit-Zeitstempel (nur von alarmTask / displayTask / wifiTask geschrieben)
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

// ── NVR / Preferences ────────────────────────────────────────
const bool ReadWrite = false;
const bool ReadOnly  = true;
bool       varState;
volatile bool safeChange = false;                                                        // von inputTask gesetzt, von nvrTask gelesen

// ── Taster ───────────────────────────────────────────────────
const uint8_t S1 = 32;
const uint8_t S2 = 33;
const uint8_t S3 = 0;
bool          S2_SW = false;
bool          S3_SW = false;

// ── Ausgänge ─────────────────────────────────────────────────
const uint8_t E1 = 25;
const uint8_t E2 = 26;
const uint8_t E3 = 27;

// ── Funktion-Vorwahl ─────────────────────────────────────────
bool    cuckoo_on  = false;
bool    light_on   = true;
bool    wheel_on   = false;
uint8_t pageselect = 0;



// =============================================================
//  Hilfsfunktionen
// =============================================================

void bTn_info() {
  Serial.println("\n----------------------------------------");
  Serial.println(PGMInfo);
}

// nicht-blockierende Verzögerungsprüfung
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
//  Alle Aufrufe nur unter displayMutex (außer setup())!
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
        display.fillRect(68, 38, 7, 7);
        display.display();
        sound1_play = sound1;
        player.playFolder(1, sound1_play);
      } else {
        display.drawRect(68, 38, 7, 7);
        display.display();
        player.stop();
      }
      break;
    case 4:
      if (sound2_on) {
        display.fillRect(68, 55, 7, 7);
        display.display();
        sound2_play = sound2;
        player.playFolder(1, sound2_play);
      } else {
        display.drawRect(68, 55, 7, 7);
        display.display();
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
//  NVR (Flash-Persistenz)
// =============================================================

void writeNVR() {
  data.putBool("a1_on",      a1_on);
  data.putInt ("a1_hour",    a1_hour);
  data.putInt ("a1_min",     a1_min);
  data.putBool("a2_on",      a2_on);
  data.putInt ("a2_hour",    a2_hour);
  data.putInt ("a2_min",     a2_min);
  data.putInt ("sound1_play",sound1_play);
  data.putInt ("sound2_play",sound2_play);
  data.putBool("cuckoo_on",  cuckoo_on);
  data.putBool("light_on",   light_on);
  data.putBool("wheel_on",   wheel_on);
  data.putInt ("vol",        vol);
}

void readNVR() {
  a1_on      = data.getBool("a1_on",       a1_on);
  a1_hour    = data.getInt ("a1_hour",      a1_hour);
  a1_min     = data.getInt ("a1_min",       a1_min);
  a2_on      = data.getBool("a2_on",        a2_on);
  a2_hour    = data.getInt ("a2_hour",      a2_hour);
  a2_min     = data.getInt ("a2_min",       a2_min);
  sound1_play= data.getInt ("sound1_play",  sound1_play);
  sound2_play= data.getInt ("sound2_play",  sound2_play);
  cuckoo_on  = data.getBool("cuckoo_on",    cuckoo_on);
  light_on   = data.getBool("light_on",     light_on);
  wheel_on   = data.getBool("wheel_on",     wheel_on);
  vol        = data.getInt ("vol",          vol);
}



// =============================================================
//  Alarm / Kuckuck
// =============================================================

void alarmTrigger() {
  if (a1_on && t_sec == 0 && t_min == a1_min && t_hour == a1_hour) {
    player.playFolder(1, sound1_play);
    if (wheel_on) { digitalWrite(E2, HIGH); }
    if (light_on) { digitalWrite(E3, HIGH); }
    if (wheel_on || light_on) { t_start6 = millis(); ax_live = true; }
  }
  if (a2_on && t_sec == 0 && t_min == a2_min && t_hour == a2_hour) {
    player.playFolder(1, sound2_play);
    if (wheel_on) { digitalWrite(E2, HIGH); }
    if (light_on) { digitalWrite(E3, HIGH); }
    if (wheel_on || light_on) { t_start6 = millis(); ax_live = true; }
  }
}

void cuckooTrigger() {
  if (t_min == 0 && t_sec == 0 && cuckoo_on) {
    digitalWrite(E1, HIGH);
    t_start4 = millis();
  }
  if (delayFunction(t_start4, delay4)) {
    digitalWrite(E1, LOW);
  }
}



// =============================================================
//  Task 1 – touchTask  (Core 0, Pri 2)
//
//  ESP-IDF touch_pad_* Polling. Misst Baseline beim Start,
//  erkennt Touch-ON-Flanken, schickt EVT_T0..T4 in inputQueue.
//  Debounce per xTaskGetTickCount() mit delay1 (250 ms).
// =============================================================
static void touchTask(void *pvParam) {
  touch_pad_init();
  touch_pad_set_voltage(TOUCH_HVOLT_2V7, TOUCH_LVOLT_0V5, TOUCH_HVOLT_ATTEN_1V);
  for (int i = 0; i < 4; i++) {
    touch_pad_config(TOUCH_PADS[i], 0);
  }

  vTaskDelay(pdMS_TO_TICKS(300));                                                        // Einschwingenzeit

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
  const TickType_t debounce  = pdMS_TO_TICKS(delay1);                                   // 250 ms

  while (true) {
    uint16_t   val;
    TickType_t now = xTaskGetTickCount();

    for (int i = 0; i < 4; i++) {
      touch_pad_read(TOUCH_PADS[i], &val);
      uint16_t thr  = (baseline[i] > TOUCH_DROP) ? baseline[i] - TOUCH_DROP : 0;
      bool     pressed = (val < thr);

      if (pressed && !wasPressed[i]) {                                                   // Touch-ON-Flanke
        if ((now - lastFireTick[i]) >= debounce) {
          lastFireTick[i] = now;
          uint8_t evt = EVT_ID[i];
          xQueueSend(inputQueue, &evt, 0);                                               // nie blockieren
        }
      }
      wasPressed[i] = pressed;
    }
    vTaskDelay(pdMS_TO_TICKS(TOUCH_POLL_MS));
  }
}



// =============================================================
//  Taster-ISRs  (S1, S2, S3)
//
//  Kurze ISRs: schicken Event direkt in die Queue.
//  Debounce erfolgt in inputTask per lastBtnMs[].
// =============================================================
void IRAM_ATTR isrS1() {
  uint8_t evt = EVT_S1;
  BaseType_t hp = pdFALSE;
  xQueueSendFromISR(inputQueue, &evt, &hp);
  portYIELD_FROM_ISR(hp);
}
void IRAM_ATTR isrS2() {
  uint8_t evt = EVT_S2;
  BaseType_t hp = pdFALSE;
  xQueueSendFromISR(inputQueue, &evt, &hp);
  portYIELD_FROM_ISR(hp);
}
void IRAM_ATTR isrS3() {
  uint8_t evt = EVT_S3;
  BaseType_t hp = pdFALSE;
  xQueueSendFromISR(inputQueue, &evt, &hp);
  portYIELD_FROM_ISR(hp);
}



// =============================================================
//  Task 2 – inputTask  (Core 1, Pri 2)
//
//  Liest alle Events aus inputQueue (Touch + Taster).
//  Führt die komplette Bedienlogik (Touched-Äquivalent) aus.
//  Alle Display-Aufrufe laufen unter displayMutex.
//  Setzt safeChange → gibt nvrSemaphore an nvrTask.
// =============================================================
static uint32_t lastBtnMs[3] = {};                                                       // Debounce-Zeitstempel S1, S2, S3

static void inputTask(void *pvParam) {
  uint8_t evt;

  while (true) {
    // Blockiert bis ein Event ankommt (max. 50 ms, damit safeChange nicht verzögert)
    if (xQueueReceive(inputQueue, &evt, pdMS_TO_TICKS(50)) != pdTRUE) {
      goto check_nvr;                                                                    // kein Event – nur NVR-Check
    }

    // ── Display-Mutex holen ───────────────────────────────────
    if (xSemaphoreTake(displayMutex, pdMS_TO_TICKS(100)) != pdTRUE) {
      goto check_nvr;                                                                    // Display belegt – Event verwerfen
    }

    // ─────────────────────────────────────────────────────────
    //  T0: Seite weiterschalten (0 → 1 → 2 → ... → 5 → 0)
    // ─────────────────────────────────────────────────────────
    if (evt == EVT_T0) {
      t_start5 = millis();
      if (pageselect < 5) { pageselect++; } else { pageselect = 0; }
      menu(pageselect);
    }

    // ─────────────────────────────────────────────────────────
    //  T2: Kontext-Aktion je nach Seite
    // ─────────────────────────────────────────────────────────
    else if (evt == EVT_T2) {
      t_start5 = millis();
      switch (pageselect) {
        case 1:
          a1_on = !a1_on;
          checkboxAlarm();
          safeChange = true;
          break;
        case 2:
          a2_on = !a2_on;
          checkboxAlarm();
          safeChange = true;
          break;
        case 3:
          sound1_on = !sound1_on;
          checkboxSound();
          if (sound1_on) {
            sprintf(str_s1_play, "%03u", sound1);
            cleanTXT(42, 17, 20, 10);
            zeigeZ10L(42, 17, str_s1_play);
          }
          break;
        case 4:
          sound2_on = !sound2_on;
          checkboxSound();
          if (sound2_on) {
            sprintf(str_s2_play, "%03u", sound2);
            cleanTXT(108, 17, 20, 10);
            zeigeZ10L(108, 17, str_s2_play);
          }
          break;
        case 5:
          cuckoo_on = !cuckoo_on;
          checkboxFunction();
          safeChange = true;
          break;
      }
    }

    // ─────────────────────────────────────────────────────────
    //  T3: Kontext-Aktion je nach Seite
    // ─────────────────────────────────────────────────────────
    else if (evt == EVT_T3) {
      t_start5 = millis();
      switch (pageselect) {
        case 0:                                                                           // Lautstärke -
          if (vol > 0) {
            vol--;
            player.volume(vol);
            sprintf(str_vol, "%02u", vol);
            cleanTXT(115, 17, 15, 10);
            zeigeZ10L(115, 17, str_vol);
          }
          safeChange = true;
          break;
        case 1:                                                                           // Alarm1 Stunde +
          if (a1_hour < 23) { a1_hour++; } else { a1_hour = 0; }
          sprintf(str_a1, "%02u:%02u", a1_hour, a1_min);
          cleanTXT(82, 34, 46, 13);
          zeigeZ16C(105, 32, str_a1);
          safeChange = true;
          break;
        case 2:                                                                           // Alarm2 Stunde +
          if (a2_hour < 23) { a2_hour++; } else { a2_hour = 0; }
          sprintf(str_a2, "%02u:%02u", a2_hour, a2_min);
          cleanTXT(82, 34, 46, 13);
          zeigeZ16C(105, 49, str_a2);
          safeChange = true;
          break;
        case 3:                                                                           // Sound1 +
          if (sound1 < mp3Count) { sound1++; } else { sound1 = 1; }
          sprintf(str_s1, "%03u", sound1);
          cleanTXT(82, 34, 46, 13);
          zeigeZ16C(105, 32, str_s1);
          sound1_on = false;
          checkboxSound();
          safeChange = true;
          break;
        case 4:                                                                           // Sound2 +
          if (sound2 < mp3Count) { sound2++; } else { sound2 = 1; }
          sprintf(str_s2, "%03u", sound2);
          cleanTXT(82, 51, 46, 13);
          zeigeZ16C(105, 32, str_s2);
          sound2_on = false;
          checkboxSound();
          safeChange = true;
          break;
        case 5:                                                                           // Licht toggle
          light_on = !light_on;
          checkboxFunction();
          safeChange = true;
          break;
      }
    }

    // ─────────────────────────────────────────────────────────
    //  T4: Kontext-Aktion je nach Seite
    // ─────────────────────────────────────────────────────────
    else if (evt == EVT_T4) {
      t_start5 = millis();
      switch (pageselect) {
        case 0:                                                                           // Lautstärke +
          if (vol < max_vol) {
            vol++;
            player.volume(vol);
            sprintf(str_vol, "%02u", vol);
            cleanTXT(115, 17, 15, 10);
            zeigeZ10L(115, 17, str_vol);
          }
          safeChange = true;
          break;
        case 1:                                                                           // Alarm1 Minute +
          if (a1_min < 59) { a1_min++; } else { a1_min = 0; }
          sprintf(str_a1, "%02u:%02u", a1_hour, a1_min);
          cleanTXT(82, 34, 46, 13);
          zeigeZ16C(105, 32, str_a1);
          safeChange = true;
          break;
        case 2:                                                                           // Alarm2 Minute +
          if (a2_min < 59) { a2_min++; } else { a2_min = 0; }
          sprintf(str_a2, "%02u:%02u", a2_hour, a2_min);
          cleanTXT(82, 51, 46, 13);
          zeigeZ16C(105, 49, str_a2);
          safeChange = true;
          break;
        case 3:                                                                           // Sound1 -
          if (sound1 > 1) { sound1--; } else { sound1 = mp3Count; }
          sprintf(str_s1, "%03u", sound1);
          cleanTXT(82, 34, 46, 13);
          zeigeZ16C(105, 32, str_s1);
          sound1_on = false;
          checkboxSound();
          safeChange = true;
          break;
        case 4:                                                                           // Sound2 -
          if (sound2 > 1) { sound2--; } else { sound2 = mp3Count; }
          sprintf(str_s2, "%03u", sound2);
          cleanTXT(82, 51, 46, 13);
          zeigeZ16C(105, 49, str_s2);
          sound2_on = false;
          checkboxSound();
          safeChange = true;
          break;
        case 5:                                                                           // Mühlrad toggle
          wheel_on = !wheel_on;
          checkboxFunction();
          safeChange = true;
          break;
      }
    }

    // ─────────────────────────────────────────────────────────
    //  S1: Alarm/Sound stoppen  oder  Kuckuck einmalig
    // ─────────────────────────────────────────────────────────
    else if (evt == EVT_S1) {
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
        } else {
          digitalWrite(E1, HIGH);
          t_start4 = millis();
        }
      }
    }

    // ─────────────────────────────────────────────────────────
    //  S2: Zugschalter – Licht + Mühlrad EIN/AUS
    // ─────────────────────────────────────────────────────────
    else if (evt == EVT_S2) {
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
    }

    // ─────────────────────────────────────────────────────────
    //  S3: INFO-Seite ein/aus
    // ─────────────────────────────────────────────────────────
    else if (evt == EVT_S3) {
      uint32_t t_now = millis();
      if (t_now - lastBtnMs[2] >= delay3) {
        lastBtnMs[2] = t_now;
        S3_SW = !S3_SW;
        if (S3_SW) { menu(6); } else { menu(0); }
      }
    }

    xSemaphoreGive(displayMutex);

    check_nvr:
    // NVR-Sicherung auslösen wenn eine Einstellung geändert wurde
    if (safeChange) {
      safeChange = false;
      xSemaphoreGive(nvrSemaphore);
    }
  }
}



// =============================================================
//  Task 3 – displayTask  (Core 1, Pri 1)
//
//  Aktualisiert die Uhrzeit auf Seite 0 (delay2 = 300 ms).
//  Behandelt Mitternacht (Datum neu zeichnen).
//  Auto-Rückkehr zu Seite 0 nach delay5 (30 s).
// =============================================================
static void displayTask(void *pvParam) {
  while (true) {
    showTime();

    if (xSemaphoreTake(displayMutex, pdMS_TO_TICKS(100)) == pdTRUE) {

      if (pageselect == 0) {
        if (t_hour == 0 && t_min == 0 && t_sec < 2) {
          menu(0);                                                                        // Mitternacht: Datum neu
        } else if (t_sec != t_sec_alt) {
          t_sec_alt = t_sec;
          cleanTXT(20, 0, 120, 16);
          zeigeZ16C(64, 0, zeit);
        }
      }

      if (pageselect != 0 && delayFunction(t_start5, delay5)) {
        pageselect = 0;
        menu(0);
      }

      xSemaphoreGive(displayMutex);
    }

    vTaskDelay(pdMS_TO_TICKS(delay2));                                                   // 300 ms
  }
}



// =============================================================
//  Task 4 – alarmTask  (Core 1, Pri 2)
//
//  Prüft Alarmzeiten und Kuckuck (500 ms Zyklus).
//  Schaltet Ausgänge nach MP3-Ende ab (Nachlauf delay6).
// =============================================================
static void alarmTask(void *pvParam) {
  while (true) {
    alarmTrigger();
    cuckooTrigger();

    if (ax_live && delayFunction(t_start6, delay6)) {
      playerStatus = player.readState();
      playerStatus = player.readState();                                                  // zweite Abfrage → korrekter Status
      t_start6 = millis();
      if (playerStatus < 1) {
        if (wheel_on) { digitalWrite(E2, LOW); }
        if (light_on) { digitalWrite(E3, LOW); }
        ax_live = false;
      }
    }

    vTaskDelay(pdMS_TO_TICKS(500));
  }
}



// =============================================================
//  Task 5 – wifiTask  (Core 0, Pri 1)
//
//  Überwacht WiFi-Verbindung, reconnect alle delay7 (3 s).
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
//
//  Wartet blockierend auf nvrSemaphore (von inputTask gegeben).
//  Schreibt dann die geänderten Einstellungen in den NVR.
// =============================================================
static void nvrTask(void *pvParam) {
  while (true) {
    xSemaphoreTake(nvrSemaphore, portMAX_DELAY);                                         // blockiert bis Einstellung geändert
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
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nWiFi connected");
  Serial.println(WiFi.localIP());

  // ── NTP warten ───────────────────────────────────────────
  cleanTXT(0, 49, 128, 15);
  zeigeZ16C(64, 49, "NTP ...");
  Serial.println("\nwarte auf NTP");
  while (tm.tm_year < 71) {
    showTime();
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nNTP connected");

  // ── GPIO ─────────────────────────────────────────────────
  pinMode(S1, INPUT_PULLUP);
  pinMode(S2, INPUT_PULLUP);
  pinMode(S3, INPUT_PULLUP);
  pinMode(E1, OUTPUT);
  pinMode(E2, OUTPUT);
  pinMode(E3, OUTPUT);

  // ── FreeRTOS Objekte ─────────────────────────────────────
  inputQueue   = xQueueCreate(32, sizeof(uint8_t));                                      // 32 Events puffern
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

  // ── WiFi-Zeitstempel ─────────────────────────────────────
  sprintf(datum_WiFi, "%04u%02u%02u", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
  sprintf(zeit_WiFi,  "%02u:%02u:%02u", tm.tm_hour, tm.tm_min, tm.tm_sec);

  // ── Startseite ───────────────────────────────────────────
  showTime();
  sprintf(str_a1,      "%02u:%02u", a1_hour, a1_min);
  sprintf(str_a2,      "%02u:%02u", a2_hour, a2_min);
  sprintf(str_s1,      "%03u",      sound1);
  sprintf(str_s2,      "%03u",      sound2);
  sprintf(str_s1_play, "%03u",      sound1_play);
  sprintf(str_s2_play, "%03u",      sound2_play);
  sprintf(str_vol,     "%02u",      vol);
  menu(pageselect);

  // ── MP3-Anzahl ───────────────────────────────────────────
  while (mp3Count < 1) {
    mp3Count = player.readFileCounts() - 1;
  }
  sprintf(str_mp3, "%03u", mp3Count);
  Serial.printf("Anzahl mp3-Files: %d\n", mp3Count);

  // ── FreeRTOS Tasks starten ───────────────────────────────
  //                              Funktion     Name          Stack  Param  Pri  Handle  Core
  xTaskCreatePinnedToCore(touchTask,   "touchTask",   4096, nullptr, 2, nullptr, 0);
  xTaskCreatePinnedToCore(wifiTask,    "wifiTask",    3072, nullptr, 1, nullptr, 0);
  xTaskCreatePinnedToCore(nvrTask,     "nvrTask",     3072, nullptr, 1, nullptr, 0);
  xTaskCreatePinnedToCore(inputTask,   "inputTask",   6144, nullptr, 2, nullptr, 1);
  xTaskCreatePinnedToCore(displayTask, "displayTask", 4096, nullptr, 1, nullptr, 1);
  xTaskCreatePinnedToCore(alarmTask,   "alarmTask",   3072, nullptr, 2, nullptr, 1);
}



// =============================================================
//  loop() – leer
//  Alle Funktionen laufen in FreeRTOS-Tasks.
// =============================================================
void loop() {
  vTaskDelete(nullptr);                                                                  // Arduino-loop-Task entfernen
}
