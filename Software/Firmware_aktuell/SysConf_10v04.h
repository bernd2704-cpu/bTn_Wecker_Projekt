#pragma once
// SysConf_10v04.h – Konfigurationskonstanten für bTn Wecker
// Firmware-Version : 10v04
// Datei-Version    : 10v04
// Boardverwalter   : esp32 3.3.8 von Espressif Systems
//
// Änderungshistorie:
//   10v04–Web-Log: Zeile "[Reset] Anzahl: N" → "[RESET] resetCount: N"
//   10v03–Display wird bei Alarm-Start automatisch eingeschaltet (analog
//          Touch-Wake); Helper wakeDisplay() in alarmTask
//   10v02–Display-Ein-Zeit DISPLAY_TIMEOUT_MS 10 min → 5 min
//   10v01–DFPlayer-Init: 9v16-Retry-Logik zurückgenommen (player.begin()
//          wieder als einmaliger Aufruf ohne Schleife); DFP_INIT_TIMEOUT_MS
//          und DFP_INIT_RETRY_MS entfernt; SETUP_MP3_TIMEOUT_MS
//          10000 → 5000 ms
//   10v00–Display-Abschaltung nach 10 min ohne Touch (DISPLAY_TIMEOUT_MS);
//          Berührung eines beliebigen Touchpads weckt Display erneut für
//          10 min, das auslösende Touch-Event wird verworfen (andere
//          Touch-Funktionen nur bei eingeschaltetem Display aktiv);
//          Checkbox-Rahmen vereinfacht (nur äußerer 10x10 drawRect, keine
//          inneren 8x8 drawRects mehr – schlankere Darstellung)
//   9v18– UI: Checkboxen auf 10x10 vergrößert (2px Rahmen + 1px Abstand
//          + 4x4 Check-Füllung); Checkboxen auf Seite Funktion um 2px
//          nach links verschoben (x 34 → 32)
//   9v17– UI: Checkbox-Rahmen von 1px auf 2px verdickt (zwei verschachtelte
//          drawRect 8x8 und 6x6); alle Checkboxen um 1px nach oben verschoben
//   9v16– DFPlayer-Kaltstart robuster: player.begin() in Retry-Schleife mit
//          DFP_INIT_TIMEOUT_MS/DFP_INIT_RETRY_MS; SETUP_MP3_TIMEOUT_MS
//          5000 → 10000 ms (SD-Indizierung nach Power-On dauert länger als
//          nach Reset-Taste)
//   9v15– UI: Checkboxen von 7x7 auf 8x8 vergrößert; Checked-Darstellung als
//          Rahmen (drawRect 8x8) plus innerer Füllung (fillRect 6x6) gemäß
//          neuer Icon-Vorlage
//   9v14– Wartungsqualität (Kosmetik aus Code-Review):
//          (1) resetCount++ aus readNVR() in bumpResetCount() ausgelagert –
//              readNVR() hat keine Seiteneffekte mehr
//          (2) t_sec_alt von file-scope in displayTask als static verschoben –
//              signalisiert korrekt, dass nur displayTask zugreift
//          (3) Task-Handles für stackMonTask und webLogTask ergänzt,
//              updateSnapStack() zeigt jetzt auch deren Stack-HWM
//   9v13– Mittlere Issues aus Code-Review behoben:
//          (1) Race auf t_start4/lastCuckooMin zwischen inputTask und
//              alarmTask dokumentiert – 32-bit-Writes auf Xtensa atomar,
//              daher logisch harmlos; Kommentar im Code ergänzt
//          (2) webLogTask "/"-Handler: html.reserve(8192) – verhindert
//              inkrementelle String-Reallokationen → Heap-Fragmentierung
//          (3) Mitternachts-Flackern: One-Shot-Flag midnightDrawn –
//              nur ein einziger Full-Redraw um 00:00:00 statt ~6-7×
//          (4) sound{1,2}_selected = 0 wenn mp3Count == 0 verhindert:
//              Fallback auf 1 statt ungültige Dateinummer an DFPlayer
//          (5) Alarm-Start ohne playerMutex: State nur wechseln wenn
//              playFolder wirklich lief – kein stiller Alarm bei Mutex-
//              Timeout, nächster alarmTask-Tick versucht es erneut
//   9v12– Bugfixes aus Code-Review:
//          (1) Race Condition auf globaler timeinfo/now behoben –
//              timeavailable() nutzt lokale tm-Struktur (analog wifiTask)
//          (2) datum_WiFi/zeit_WiFi ohne NTP-Sync: showTime() vor snprintf
//              und nur füllen wenn wifiConnected – verhindert "19000101"
//          (3) Dead Code ax_live entfernt (nie gelesen)
//          (4) Kommentar-Referenz SysConf_9v6.h → SysConf_9v12.h korrigiert
//   9v11– Stack-Vorgaben reduziert: wifiTask/nvrTask/inputTask/displayTask
//          2560, watchdogTask 1536 (basierend auf High-Water-Mark-Messung)
//   9v10– DFPlayer Start-Sound: readFileCounts() vor playFolder verschoben –
//          Sound wird erst abgespielt wenn SD-Index aufgebaut ist; behebt
//          Abbruch des Start-Sounds nach Power-On/Flash
//   9v9 – Web-Log: Schriftgröße der Überschriften (h2, h3, .sec-title,
//          .snap-title) auf 1rem / 1.6rem vergrößert
//   9v8 – Web-Log Auto-Refresh 10 → 20 s; Reihenfolge: Allg. Log,
//          Touch Baseline, Stack HWM; Allg. Log-Titel zeigt NTP-Sync-
//          Zeitstempel des ersten Resets
//   9v7 – Bugfix: EVT_S3 aktualisiert nun lastTouchMs – Auto-Rückkehr von
//          UI_INFO startete sofort statt nach 20 s (S3 hat EVT-ID 6 > EVT_T4=3)
//   9v6 – Feature: Auto-Rückkehr zu Seite 0 nach 20 s jetzt auch für
//          Seite 7 (UI_INFO); UI_INFO-Ausnahme aus displayTask entfernt
//   9v5 – Bugfix: esp_task_wdt_reconfigure() statt esp_task_wdt_init()
//          verhindert "TWDT already initialized"-Fehler (Arduino Core 3.x)
//   9v4 – Bugfix: playerStatus == 0 statt < 1 verhindert fälschlichen
//          Alarm-Abbruch bei UART-Timeout während WebLog-Zugriff
//   9v3 – Web-Log-Seite: Touch-Baseline und Stack-HWM als dedizierte
//          Snapshot-Sektionen (nur jeweils letzter Wert + Timestamp)
//   9v2 – WEBLOG_LINES 80 → 40, Web-Log-Seite Auto-Refresh 20 → 10 s
//   9v1 – Datei von SystemConfig.h in SysConf_9v1.h umbenannt,
//          Versionierung eingeführt, WDT_HARDWARE_MS ergänzt,
//          Stack-Größen als Kommentar dokumentiert

// ── Firmware-Version ─────────────────────────────────────────
#define FW_VERSION "10v04"                                                     // Versionsnummer (als String in PGMInfo, Web-Log, WEB.h)

// ── WiFi ─────────────────────────────────────────────────────
// STA_SSID / STA_PSK werden nicht mehr direkt genutzt.
// WLAN-Zugangsdaten werden per WebKonfigurator eingerichtet und
// im NVR-Namespace "wifiCfg" gespeichert (ab Version 4v0).
#define STA_SSID  "my_ssid"                                                    // nur als Referenz – nicht mehr in WiFi.begin() genutzt
#define STA_PSK   "my_passwrd"                                                 // nur als Referenz – nicht mehr in WiFi.begin() genutzt

// Access-Point-Konfiguration für den WiFi-Konfigurator
#define WIFI_AP_SSID    "bTn-Wecker"                                           // SSID des Konfigurations-Access-Points
#define WIFI_AP_CHANNEL 1                                                      // WiFi-Kanal des Access-Points

// ── NTP ──────────────────────────────────────────────────────
#define MY_NTP_SERVER "pool.ntp.org"                                           // NTP-Serveradresse
#define MY_TZ         "CET-1CEST,M3.5.0/02,M10.5.0/03"                         // Zeitzone (POSIX-Format)

// ── DFPlayer Serial-Pins ─────────────────────────────────────
#define RXD2 16                                                                // ESP32 GPIO16 → DFPlayer TX
#define TXD2 17                                                                // ESP32 GPIO17 → DFPlayer RX

// ── Touch-Sensor ─────────────────────────────────────────────
#define TOUCH_DROP      150                                                    // Mindest-Absenkung zur Touch-Erkennung (kalibrieren, ca. 50 % des Differenzwerts)
#define TOUCH_POLL_MS    50                                                    // Abtastrate des touchTask in ms
#define TOUCH_HOLD_MS   750                                                    // Haltezeit bis HOLD-Zustand und erster Wiederholungs-Event
#define TOUCH_REPEAT_MS 250                                                    // Wiederholrate im REPEAT-Zustand
#define TOUCH_RECAL_MS  600000UL                                               // Baseline-Rekalibrierung Intervall (10 min, nur wenn TS_IDLE)

// ── Eingabe-Event-IDs (inputQueue) ────────────────────────────
#define EVT_T0  0                                                              // Touch T0 – GPIO4
#define EVT_T2  1                                                              // Touch T2 – GPIO2
#define EVT_T3  2                                                              // Touch T3 – GPIO15
#define EVT_T4  3                                                              // Touch T4 – GPIO13
#define EVT_S1  4                                                              // Taster S1 – GPIO32
#define EVT_S2  5                                                              // Taster S2 – GPIO33
#define EVT_S3  6                                                              // Taster S3 – GPIO0

// ── Setup-Timeouts (ms) ──────────────────────────────────────
#define SETUP_WIFI_TIMEOUT_MS 30000                                            // max. Wartezeit auf WiFi-Verbindung
#define SETUP_NTP_TIMEOUT_MS  30000                                            // max. Wartezeit auf erste NTP-Synchronisation
#define SETUP_MP3_TIMEOUT_MS   5000                                            // max. Wartezeit auf DFPlayer Dateianzahl

// ── Diagnose ─────────────────────────────────────────────────
#define STACK_MON_INTERVAL_MS 60000UL                                          // Ausgabe-Intervall Stack-Überwachung (60 s)
#define WDG_TIMEOUT_MS        30000UL                                          // Watchdog: maximale Zeit ohne Lebenszeichen (30 s)
#define WDG_CHECK_MS           5000UL                                          // Watchdog: Prüfintervall (5 s)
#define WDT_HARDWARE_MS       15000UL                                          // Hardware-TWDT: Timeout (15 s) – kürzer als WDG_TIMEOUT_MS

// ── Verzögerungskonstanten (ms) ───────────────────────────────
const uint32_t TOUCH_REPEAT_RATE_MS =  250;                                    // Touch-Wiederholrate
const uint32_t DISPLAY_UPDATE_MS    =  300;                                    // Zeitanzeige Seite 0
const uint32_t BTN_DEBOUNCE_MS      =   30;                                    // ISR-Entprellung: filtert Hardware-Prellen (typ. 5–50 ms)
const uint32_t BTN_LOCKOUT_MS       = 1000;                                    // Aktionssperre in inputTask: verhindert bewusste Doppeldrücke
const uint32_t CUCKOO_DURATION_MS   = 7500;                                    // Kuckuck-Laufzeit
const uint32_t AUTO_RETURN_MS       = 20000;                                   // Auto-Rückkehr zu Seite 0
const uint32_t DISPLAY_TIMEOUT_MS   = 300000UL;                                // OLED aus nach 5 min ohne Touch-Event
const uint32_t ALARM_POLL_MS        = 5000;                                    // Alarm-Nachlauf Prüfintervall
const uint32_t WIFI_RECONNECT_MS    = 3000;                                    // WiFi-Reconnect Wiederholrate

// ── NVR-Zugriffsmodus ────────────────────────────────────────
const bool ReadWrite = false;                                                  // Preferences: Lesen + Schreiben
const bool ReadOnly  = true;                                                   // Preferences: nur Lesen

// ── Taster-Pins ──────────────────────────────────────────────
const uint8_t S1 = 32;                                                         // GPIO32 – Alarm aus / Kuckuck einmalig
const uint8_t S2 = 33;                                                         // GPIO33 – Zugschalter Licht + Mühlrad
const uint8_t S3 = 0;                                                          // GPIO0  – Info-Seite ein/aus

// ── Ausgangs-Pins ────────────────────────────────────────────
const uint8_t E1 = 25;                                                         // GPIO25 – Kuckuck
const uint8_t E2 = 26;                                                         // GPIO26 – Mühlrad / Motor
const uint8_t E3 = 27;                                                         // GPIO27 – Licht

// ── Stack-Größen (Bytes) ──────────────────────────────────────
// Angepasst auf Basis der Stack High-Water Marks aus stackMonTask.
// setup() verwendet diese Konstanten direkt – Änderungen hier wirken sofort.
#define STACK_TOUCH     3072                                                   // touchTask
#define STACK_ALARM     2048                                                   // alarmTask
#define STACK_WIFI      2560                                                   // wifiTask
#define STACK_NVR       2560                                                   // nvrTask
#define STACK_STACKMON  3072                                                   // stackMonTask
#define STACK_WATCHDOG  1536                                                   // watchdogTask
#define STACK_INPUT     2560                                                   // inputTask
#define STACK_DISPLAY   2560                                                   // displayTask
#define STACK_WEBLOG    4096                                                   // webLogTask (HTTP-Server benötigt mehr Stack)

// ── Web-Logger ────────────────────────────────────────────────
#define WEBLOG_PORT      8080                                                  // HTTP-Port des Log-Servers (8080 ≠ 80 des WiFi-Konfigurators)
#define WEBLOG_LINES       40                                                  // Anzahl Zeilen auf der Seite
#define WEBLOG_LINE_LEN   128                                                  // maximale Zeichenanzahl je Zeile
