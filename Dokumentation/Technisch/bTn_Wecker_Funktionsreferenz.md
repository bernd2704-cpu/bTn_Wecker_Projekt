# bTn Wecker – Funktions- und Task-Referenz

*Firmware 11v00 · ESP32 / FreeRTOS*

## 1. FreeRTOS Tasks

9 Tasks auf Core 0 und Core 1. Priorität 2 > Priorität 1.

| Funktion / Task | Kategorie | Aufgabe |
|---|---|---|
| touchTask<br>Core 0 · Pri 2<br>Stack 3072 | Task | Kapazitive Touch-Pads T0/T2/T3/T4 via ESP-IDF touch_pad_read(). State Machine TS_IDLE→TS_PRESSED→TS_REPEAT. Hold-Erkennung 750ms, Repeat 250ms. Exklusivität: nur ein Pad aktiv. Baseline-Rekalibrierung alle 10min im Idle. |
| alarmTask<br>Core 0 · Pri 2<br>Stack 2048 | Task | Führt alle 500ms runAlarmMachine() + runCuckooMachine() aus. Atomarer localtime_r()-Snapshot verhindert Race Condition mit displayTask. Core 0 trennt Task physisch von inputTask. runAlarmMachine() ruft bei Alarmstart wakeDisplay() auf (10v03). Setzt wdg_alarmTask + esp_task_wdt_reset(). |
| wifiTask<br>Core 0 · Pri 1<br>Stack 2560 | Task | Überwacht WiFi.status(), reconnect nach WIFI_RECONNECT_MS. Schreibt Verbindungszeitpunkt in Double-Buffer (datum_WiFi_tmp/zeit_WiFi_tmp) nur wenn wifiSyncPending==false (Torn-Read-Schutz, 11v00), setzt wifiSyncPending=true. |
| nvrTask<br>Core 0 · Pri 1<br>Stack 2560 | Task | Wartet auf nvrSemaphore. Öffnet NVS-Namespace "varSafe", ruft writeNVR() auf. inputTask gibt das Semaphore erst nach NVR_COMMIT_DELAY_MS (2 s) Ruhezeit frei (Flash-Wear-Schutz gegen Touch-REPEAT-Hammer, 11v00). |
| stackMonTask<br>Core 0 · Pri 1<br>Stack 3072 | Task | Aktualisiert alle STACK_MON_INTERVAL_MS (60 s) den Stack-HWM-Snapshot (snapStackBuf/snapStackTime) via updateSnapStack(): alle 9 Tasks + freier Heap. Wird auf der Web-Log-Seite als dedizierte Sektion angezeigt. |
| watchdogTask<br>Core 0 · Pri 1<br>Stack 1536 | Task | Software-Watchdog: prüft alle WDG_CHECK_MS (5s) ob wdg_inputTask/displayTask/alarmTask innerhalb WDG_TIMEOUT_MS (30s) aktualisiert wurden. Bei Freeze: webLog + OLED-Meldung + ESP.restart(). |
| webLogTask<br>Core 0 · Pri 1<br>Stack 4096 | Task | HTTP-Log-Server auf Port WEBLOG_PORT (8080). Wartet auf WiFi, dann: GET / → HTML-Seite mit Auto-Refresh alle 20 s und farbiger Darstellung (grün/rot/gelb). GET /log → plain text. Ring-Puffer WEBLOG_LINES × WEBLOG_LINE_LEN (Allg. Log, Titel zeigt letzten NTP-Sync, 9v8) + Snapshot-Sektionen Touch-Baseline und Stack-HWM. |
| inputTask<br>Core 1 · Pri 2<br>Stack 2560 | Task | Konsumiert inputQueue (50 ms-Timeout). S1/S2 ohne displayMutex. Alle anderen Events: displayMutex → uiDispatch() → uiTransition(). safeChange + safeChangeMs: nvrSemaphore erst nach NVR_COMMIT_DELAY_MS (2 s) Ruhezeit ohne weiteres Event freigeben (11v00). Touch-Wake wenn displayBlanked. Setzt wdg_inputTask + esp_task_wdt_reset(). |
| displayTask<br>Core 1 · Pri 1<br>Stack 2560 | Task | Aktualisiert OLED alle DISPLAY_UPDATE_MS (300 ms) unter displayMutex. Überträgt NTP/WiFi-Double-Buffer. Auto-Rückkehr zu UI_CLOCK nach AUTO_RETURN_MS (20 s). Schaltet OLED ab nach DISPLAY_TIMEOUT_MS (5 min) ohne Touch (10v00/10v02). Setzt wdg_displayTask + esp_task_wdt_reset(). |

## 2. Interrupt Service Routinen

Zwei-Stufen-Debouncing: ISR-Ebene BTN_DEBOUNCE_MS=30ms, Task-Ebene BTN_LOCKOUT_MS=1000ms.

| Funktion / Task | Kategorie | Aufgabe |
|---|---|---|
| isrS1() | ISR | FALLING-ISR GPIO32 (IRAM). Prüft millis()-isrBtnMs[0] ≥ BTN_DEBOUNCE_MS, sendet EVT_S1 via xQueueSendFromISR. |
| isrS2() | ISR | FALLING-ISR GPIO33. Analog S1, sendet EVT_S2. |
| isrS3() | ISR | FALLING-ISR GPIO0. Analog S1, sendet EVT_S3. |

## 3. State Machines

| Funktion / Task | Kategorie | Aufgabe |
|---|---|---|
| runAlarmMachine()<br>sec,min,hour | State Machine | ALARM_IDLE/ALARM_RUNNING. IDLE: prüft Alarm 1+2 (Minuten-Sperre, Alarm 1 Vorrang). RUNNING: pollt DFPlayer alle ALARM_POLL_MS (5s). Bei playerStatus==0 (MP3 beendet): Ausgänge aus, ALARM_IDLE. playerStatus==-1 (UART-Timeout) hält den Alarm aktiv – siehe 9v4-Bugfix. |
| runCuckooMachine()<br>sec,min,hour | State Machine | CUCKOO_IDLE/CUCKOO_RUNNING. IDLE: min==0, sec==0, cuckoo_on, Zeitfenster OK, !alarmThisHour, min≠lastCuckooMin → E1 HIGH. RUNNING: nach CUCKOO_DURATION_MS (7,5s) → E1 LOW, CUCKOO_IDLE. |
| uiDispatch()<br>s, evt | State Machine | UI-Haupt-Dispatcher. Behandelt T0 (Seitenzyklus) und S3 (Info-Toggle) global, delegiert Rest an State-Handler. |
| uiTransition()<br>next | State Machine | Führt Zustandswechsel durch: uiState=next, pageselect=next, menu(next). Muss unter displayMutex aufgerufen werden. |
| wakeDisplay() | Helper | Schaltet OLED wieder ein, falls es nach DISPLAY_TIMEOUT_MS abgeschaltet wurde (10v03 als Alarm-Wake). Prüft displayBlanked und setzt lastTouchMs atomar unter displayMutex – behebt TOCTOU/Race (10v06). |
| markSafeChange() | Helper | Setzt safeChangeMs=millis() und safeChange=true. Jeder Aufruf startet den NVR_COMMIT_DELAY_MS-Ruhezeit-Zähler neu; inputTask gibt das nvrSemaphore erst nach Ablauf dieser Ruhezeit frei (11v00, Flash-Wear-Schutz). |

## 4. UI State-Handler

| Funktion / Task | Kategorie | Aufgabe |
|---|---|---|
| onClock(evt) | UI-Handler | Seite 0: T3=Vol+, T4=Vol–. Sofortiger Player-Aufruf unter playerMutex. |
| onAlarm1(evt) | UI-Handler | Seite 1: T2=Ein/Aus, T3=Stunde+, T4=Minute+ (mit Überlauf). Partielles Display-Update. |
| onAlarm2(evt) | UI-Handler | Seite 2: analog onAlarm1 für Alarm 2. |
| onSound1(evt) | UI-Handler | Seite 3: T2=Vorschau toggeln, T3=Datei+, T4=Datei–. sound1_assigned bei Vorschau-Aktivierung. |
| onSound2(evt) | UI-Handler | Seite 4: analog onSound1 für Alarm 2. |
| onFuncs(evt) | UI-Handler | Seite 5: T2=Kuckuck, T3=Licht, T4=Mühlrad – je toggle. |
| onCuckooTime(evt) | UI-Handler | Seite 6: T3=cuckoo_onTime+, T4=cuckoo_offTime+ (0–23 mit Überlauf). |
| onInfo(evt) | UI-Handler | Info (S3): T0=WiFi-Konfigurator, T4=Werksreset. Zeile 54: IP:Port des Web-Log-Servers. |

## 5. Web-Logger

| Funktion / Task | Kategorie | Aufgabe |
|---|---|---|
| snapTimeStr() | Web-Logger | Hilfsfunktion: formatiert die aktuelle Zeit als Timestamp-String (dd.mm.yyyy hh:mm:ss). Gibt Uptime-Sekunden zurück wenn NTP noch nicht synchronisiert ist. |
| updateSnapTouch()<br>baseline | Web-Logger | Aktualisiert den Touch-Baseline-Snapshot-Puffer (snapTouchBuf) thread-safe unter webLogMutex. Schreibt Baseline und Threshold aller 4 Pads sowie Timestamp (snapTouchTime). Wird von touchTask bei Rekalibrierung aufgerufen. |
| updateSnapStack() | Web-Logger | Aktualisiert den Stack-HWM-Snapshot-Puffer (snapStackBuf) thread-safe unter webLogMutex. Erfasst alle 9 Tasks (inkl. stackMonTask via hStackMonTask und webLogTask via hWebLogTask) + freien Heap sowie Timestamp. Wird von stackMonTask aufgerufen. |
| webLog(msg) | Web-Logger | Schreibt msg in den Ring-Puffer (WEBLOG_LINES×WEBLOG_LINE_LEN). Thread-sicher via webLogMutex. Null-Guard: if(!webLogMutex) return. Älteste Zeilen werden überschrieben. |
| webLogf(fmt,...) | Web-Logger | printf-Variante von webLog(). Formatiert in lokalen Puffer, ruft webLog() auf. Ersetzt Serial.*-Ausgaben nach WiFi-Connect. |

## 6. Display-Funktionen

| Funktion / Task | Kategorie | Aufgabe |
|---|---|---|
| menu(page) | Display | Zeichnet komplette Menüseite 0–7, ruft display.display() am Ende auf. |
| cleanTXT(x,y,dx,dy) | Display | Löscht Rechteck im Framebuffer (fillRect schwarz). |
| zeigeZ10C/L/R(x,y,txt) | Display | 10pt-Text zentriert / linksbündig / rechtsbündig. |
| zeigeZ16C/L(x,y,txt) | Display | 16pt-Text zentriert / linksbündig. |
| checkboxAlarm() | Display | Alarm-Checkboxen zeichnen. |
| checkboxSound() | Display | Sound-Vorschau-Checkboxen zeichnen. |
| checkboxFunction() | Display | Funktions-Checkboxen (Kuckuck/Licht/Mühlrad) zeichnen. |
| showTime() | Display | Zeit via localtime_r() lesen, datum[]/zeit[]/t_* befüllen. Unter displayMutex aufgerufen. |
| timeavailable() | Display | SNTP-Callback: schreibt datum_sync_tmp/zeit_sync_tmp, setzt ntpSyncPending. |

## 7. NVS-, WiFi- und Systemfunktionen

| Funktion / Task | Kategorie | Aufgabe |
|---|---|---|
| writeNVR() | NVS | Schreibt alle Einstellungen in NVS-Namespace "varSafe". |
| readNVR() | NVS | Liest NVS, klemmt alle Werte auf gültige Bereiche. Hat ab 9v14 keine Seiteneffekte mehr (resetCount nicht mehr hier inkrementiert). |
| bumpResetCount() | NVS | Liest resetCount aus NVS, inkrementiert und schreibt zurück. Aus readNVR() ausgelagert (9v14) – klare Trennung: readNVR() liest nur, bumpResetCount() schreibt nur den Zähler. |
| loadWifiCredentials() | WiFi | Liest SSID/PSK aus "wifiCfg", gibt true bei gültigem Eintrag zurück. |
| clearWifiCredentials() | WiFi | Setzt valid-Flag in "wifiCfg" auf false → Konfigurator beim nächsten Start. |
| runWifiConfigServer() | WiFi | Blockierend (vor Task-Start). AP "bTn-Wecker", HTTP Port 80, GET/POST, ESP.restart() nach Speichern. |
| validateWifiInput() | WiFi | Prüft SSID (1–32) und PSK (leer oder 8–63 Zeichen). |
| wifiErrorRedirect()<br>msg | WiFi | Erzeugt eine HTTP-Redirect-Seite (in WEB.h) zur Konfigurationsseite mit URL-kodierter Fehlermeldung als ?err=... Parameter. Browser zeigt Fehler per JavaScript an. |
| setup() | System | Initialisierung: Serial, NVS, Display, WiFi, NTP, GPIO, FreeRTOS-Objekte, Interrupts, DFPlayer, Tasks, TWDT. |
| loop() | System | Löscht Arduino-Loop-Task sofort via vTaskDelete(nullptr). |
| rtosPanic(what) | System | Fehlerbehandlung: Serial + OLED, 3s warten, ESP.restart(). |
| delayFunction(t,d) | System | true wenn millis()-t >= d. Nicht-blockierende Zeitprüfung für State Machines. |
| bTn_info() | System | Gibt Trennzeile und PGMInfo-Banner (Projekt-/Versionskennung) auf Serial aus. Einmaliger Aufruf am Ende von setup(). |

---

*bTn Wecker · Funktionsreferenz · Firmware 11v00*
