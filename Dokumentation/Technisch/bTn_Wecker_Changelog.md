**bTn Wecker**

Änderungshistorie

Basis 4v1  →  11v01

## Kategorien

| Bugfix | Stabilität | Funktion | Refactoring | Qualität |
|---|---|---|---|---|

## Versionen 4v1 – 5v4  (Arduino-Basis)

| Version | Kategorie | Änderung |
|---|---|---|
| 4v2 | Bugfix | Touch T3/T4 Lautstärke vertauscht – korrigiert |
| 4v2 | Bugfix | Maximale Lautstärke auf 20 begrenzt (max_vol) |
| 4v2 | Bugfix | NVS-Lautstärke-Clamp beim Laden auf max_vol |
| 4v3 | Stabilität | Touch-Schwellwert: Fallback auf 80% der Baseline wenn Baseline < TOUCH_DROP |
| 4v4 | Bugfix | Alarm 2 auf else-if: Alarm 1 hat Vorrang bei gleicher Weckzeit |
| 4v5 | Bugfix | readFileCounts() Unterlauf-Fix: c==0 verhindert uint8_t-Überlauf auf 255 |
| 5v0 | Stabilität | readState()-Schleife: Timeout 200 ms verhindert Hänger bei DFPlayer-Ausfall |
| 5v1 | Qualität | Alle sprintf() durch snprintf() ersetzt |
| 5v2 | Funktion | Reset-Zähler im NVS; Werksreset via T4 auf Info-Seite |
| 5v2 | Funktion | Neue Funktion zeigeZ10R() für rechtsbündige OLED-Ausgabe |
| 5v3 | Qualität | Layout-Anpassungen Info-Seite |
| 5v4 | Funktion | Kuckuck unterdrückt wenn Alarm auf gleiche volle Stunde eingestellt |

## Versionen 6v0 – 7v2

| Version | Kategorie | Änderung |
|---|---|---|
| 6v0 | Funktion | Neue Menüseite 6: Kuckuck-Aktivzeit (Von/Bis-Stunde), NVS-gespeichert |
| 6v2 | Bugfix | Kuckuck-Zeitfenster: Mitternacht-Überlauf korrekt behandelt |
| 6v3 | Bugfix | lastCuckooMin nie zurückgesetzt – Kuckuck löste nur einmal aus |
| 6v4 | Stabilität | WiFi-Konfig- und Werksreset-Meldung unter displayMutex |
| 6v5 | Stabilität | wifiTask: lokaler tm-Snapshot via localtime_r() – kein Torn-Read |
| 6v6 | Qualität | zeigeZ-Parameter von String auf const char* – kein Heap-Overhead |
| 6v8 | Qualität | display.display() aus zeigeZ-Funktionen entfernt – zentraler Flush |
| 7v0 | Qualität | max_vol als constexpr MAX_VOL; varState lokal in setup() |
| 7v2 | Stabilität | alarmTask/runAlarmMachine/runCuckooMachine: eigener localtime_r()-Snapshot – Race Condition mit displayTask beseitigt |

## Versionen 7v3 – 7v9

| Version | Kategorie | Änderung |
|---|---|---|
| 7v3 | Stabilität | datum_WiFi / zeit_WiFi Double-Buffer – wifiTask schreibt in tmp-Puffer |
| 7v4 | Stabilität | readNVR(): alle Werte nach Laden geclampt |
| 7v5 | Qualität | goto wifi_skip durch bool wifiConnected ersetzt |
| 7v6 | Qualität | delay1–delay7 durch sprechende Konstantennamen ersetzt |
| 7v7 | Qualität | sound1/sound2 → sound1_selected/sound2_selected; sound1_play/sound2_play → sound1_assigned/sound2_assigned |
| 7v8 | Bugfix | setup(): display.display() an allen Stellen ergänzt – OLED blieb beim Start dunkel |
| 7v9 | Stabilität | Zwei-Stufen-Debouncing: ISR BTN_DEBOUNCE_MS=30ms, inputTask BTN_LOCKOUT_MS=1000ms |

## Versionen 8v0 – 8v2

| Version | Kategorie | Änderung |
|---|---|---|
| 8v0 | Bugfix | vTaskDelay(1ms) unter gehaltenem playerMutex → Freeze bei Alarm+Kuckuck 10:00 – Mutex vor Delay freigeben |
| 8v1 | Stabilität | alarmTask Core 1 → Core 0: physische Trennung von inputTask eliminiert CPU-Scheduling-Konflikte |
| 8v2 | Stabilität | Anwendungs-Watchdog (watchdogTask): überwacht inputTask/displayTask/alarmTask via Alive-Timestamps (30s/5s) |
| 8v2 | Bugfix | wdg_displayTask fehlte in displayTask – Timestamp vor Mutex-Versuch gesetzt |

## Versionen 9v0 – 9v2

| Version | Kategorie | Änderung |
|---|---|---|
| 9v0 | Stabilität | Hardware-TWDT (esp_task_wdt): inputTask, displayTask, alarmTask angemeldet; Timeout 15 s, trigger_panic=true |
| 9v0 | Qualität | Stack-Größen als STACK_*-Konstanten in SysConf; SystemConfig.h → SysConf_9v0.h mit Versionshistorie |
| 9v0 | Qualität | Stack-Größen angepasst: touchTask 3072, alarmTask 2048, inputTask 3072, displayTask 3072 |
| 9v1/9v2 | Funktion | Web-Logger: webLog()/webLogf() mit Thread-sicherem Ring-Puffer (Mutex-geschützt) |
| 9v1/9v2 | Funktion | webLogTask (Core 0, Pri 1): HTTP-Server Port 8080, GET / (HTML+Auto-Refresh), GET /log (plain text) |
| 9v1/9v2 | Funktion | Alle Serial.*-Ausgaben nach WiFi-Connect → webLog/webLogf (stackMon, watchdog, touch, wifi, setup) |
| 9v1/9v2 | Funktion | Letzte Serial-Ausgabe: IP-Adresse + Log-URL nach WiFi-Connect |
| 9v1/9v2 | Funktion | UI_INFO (Seite 7): Zeile 54 zeigt jetzt IP:Port des Web-Log-Servers statt SSID |
| 9v2 | Bugfix | webLogMutex fehlte in setup() → assert NULL pxQueue bei erstem Client-Zugriff – Mutex jetzt korrekt initialisiert |
| 9v2 | Bugfix | Lambda-Handler ohne Null-Prüfung auf webLogMutex – if(webLogMutex && ...) ergänzt |
| 9v2 | Qualität | WEBLOG_LINES 80 → 40 (Heap-Ersparnis ~5 KB) |
| 9v2 | Qualität | Auto-Refresh HTML 20 s → 10 s |
| 9v3 | Funktion | Web-Log-Seite: Touch-Baseline und Stack-HWM als dedizierte Snapshot-Sektionen – nur jeweils letzter Wert + Timestamp, kein Spam im Ring-Puffer |
| 9v4 | Bugfix | playerStatus == 0 statt < 1 in runAlarmMachine(): readState() gibt bei UART-Timeout -1 zurück – mit < 1 wurde fälschlich Alarm-Abbruch ausgelöst während webLogTask auf Serial2 zugreift |
| 9v4 | Qualität | SysConf bleibt bei 9v3-Stand – keine Konstanten-Änderungen in 9v4 |
| 9v4 | Bugfix | KRITISCH: vTaskDelay(1ms) unter playerMutex im S1-Handler (inputTask) – Mutex sofort freigeben, dann vTaskDelay(), dann neu nehmen (analog Fix 8v0 in alarmTask) |
| 9v4 | Bugfix | webLogMutex-Initialisierung vor ersten webLogf()-Aufrufen in setup() verschoben – setup()-Meldungen gingen bisher verloren |
| 9v4 | Qualität | webLogReady-Flag entfernt (toter Code – Deklaration und Zuweisung) |
| 9v4 | Qualität | WEB.h Footer-Versionsstring 4v0 → 9v4 korrigiert |
| 9v4 | Qualität | Web-Log-Seitentitel 9v3 → 9v4, TWDT-Kommentar 9v3 → 9v4 korrigiert |
| 9v5 | Bugfix | esp_task_wdt_init() → esp_task_wdt_reconfigure(): TWDT ist beim Arduino-Start bereits initialisiert – Doppel-Init vermieden, alle drei Parameter (timeout_ms, idle_core_mask=0, trigger_panic=true) werden korrekt übernommen |
| 9v5 | Qualität | Alle internen 9v4-Referenzen in .ino und SysConf ersetzt; WEB.h Footer-Version aktualisiert |
| 9v6 | Funktion | FW_VERSION-Makro eingeführt – Versionsnummer zentral in SysConf_9v6.h definiert, nicht mehr verteilt im Code |
| 9v6 | Funktion | Auto-Rückkehr zu UI_CLOCK jetzt auch von UI_INFO (Seite 7) – bisher war UI_INFO ausgenommen |
| 9v6 | Bugfix | Web-Log Versionsstring korrigiert; mp3Count-Clamp auf gültigen Bereich ergänzt |
| 9v6 | Qualität | PGMInfo und WEB.h Footer auf bTn_Alarm_9v6 aktualisiert; Kommentare korrigiert |
| 9v7 | Bugfix | EVT_S3 aktualisiert lastTouchMs – ohne Fix wurde nach S3 (Info-Toggle) die Auto-Rückkehr-Zeit nicht zurückgesetzt, Seite kehrte zu früh zurück |
| 9v8 | Qualität | bTn_Alarm_ → bTn_Wecker_ in PGMInfo, Kommentaren und WEB.h-Footer – konsistente Projektbezeichnung |
| 9v8 | Funktion | Web-Log: Reihenfolge der Sektionen – Allg. Log zuerst, dann Touch-Baseline, zuletzt Stack-HWM |
| 9v8 | Funktion | Web-Log: Allg. Log-Titel zeigt NTP-Sync-Zeitstempel nach Reset |
| 9v8 | Qualität | Web-Log: Schriftgröße 13 → 19 px; Zeilen nicht umbrechen (overflow-x: auto) |
| 9v8 | Qualität | Web-Log: Auto-Refresh 10 → 20 s |
| 9v9 | Qualität | Web-Log: Schriftgröße der Überschriften vergrößert (h2 → 1.6 rem, h3/.sec-title/.snap-title → 1 rem) |
| 9v10 | Bugfix | DFPlayer Start-Sound: readFileCounts() wird vor playFolder() aufgerufen – verhindert Abspielversuch bevor Dateianzahl bekannt ist |
| 9v11 | Qualität | Stack-Vorgaben reduziert: wifiTask/nvrTask/inputTask/displayTask 2560, watchdogTask 1536 – basierend auf gemessenen High-Water Marks |
| 9v12 | Bugfix | Bugfixes aus Code-Review (kritische Issues) |
| 9v13 | Bugfix | Mittlere Issues aus Code-Review behoben |
| 9v14 | Qualität | Wartungsqualität: Kosmetik aus Code-Review (Kommentare, Formatierung, Namensgebung) |
| 9v14 | Qualität | Versionsangaben in allen Dateien auf 9v14 synchronisiert |

## Versionen 9v15 – 10v06

| Version | Kategorie | Änderung |
|---|---|---|
| 9v15 | Qualität | UI: Checkboxen von 7×7 auf 8×8 vergrößert; Checked-Darstellung als Rahmen (drawRect 8×8) plus innere Füllung (fillRect 6×6) |
| 9v16 | Stabilität | DFPlayer-Kaltstart robuster: player.begin() in Retry-Schleife mit DFP_INIT_TIMEOUT_MS / DFP_INIT_RETRY_MS; SETUP_MP3_TIMEOUT_MS 5000 → 10000 ms |
| 9v17 | Qualität | UI: Checkbox-Rahmen von 1 px auf 2 px verdickt (zwei verschachtelte drawRect); alle Checkboxen um 1 px nach oben verschoben |
| 9v18 | Qualität | UI: Checkboxen auf 10×10 vergrößert (2 px Rahmen + 1 px Abstand + 4×4 Füllung); Checkboxen auf Seite Funktion um 2 px nach links verschoben |
| 10v00 | Funktion | Display-Abschaltung nach 10 min ohne Touch (DISPLAY_TIMEOUT_MS); Berührung eines beliebigen Touchpads weckt Display wieder für 10 min, das auslösende Touch-Event wird verworfen; Checkbox-Rahmen vereinfacht (nur äußerer 10×10 drawRect) |
| 10v01 | Qualität | DFPlayer-Init: 9v16-Retry-Logik zurückgenommen (player.begin() wieder einmaliger Aufruf); DFP_INIT_TIMEOUT_MS / DFP_INIT_RETRY_MS entfernt; SETUP_MP3_TIMEOUT_MS 10000 → 5000 ms |
| 10v02 | Qualität | Display-Ein-Zeit DISPLAY_TIMEOUT_MS von 10 min auf 5 min reduziert |
| 10v03 | Funktion | Display wird bei Alarm-Start automatisch eingeschaltet (analog Touch-Wake); Helper wakeDisplay() in alarmTask |
| 10v04 | Qualität | Web-Log: Zeile „[Reset] Anzahl: N" → „[RESET] resetCount: N" (einheitlicher Stil) |
| 10v05 | Stabilität | DFPlayer TX-Pin (GPIO17) vor Serial2.begin() 3 s LOW halten – verhindert Fehlinterpretation der ersten UART-Bytes beim Kaltstart |
| 10v06 | Bugfix | wakeDisplay(): TOCTOU und Race auf lastTouchMs behoben (displayBlanked-Check und lastTouchMs=millis() atomar unter displayMutex); lastTouchMs als volatile deklariert (Cross-Core-Sichtbarkeit) |

## Version 11v00

| Version | Kategorie | Änderung |
|---|---|---|
| 11v00 | Stabilität | NVR-Flash-Wear-Schutz: nvrSemaphore wird erst nach NVR_COMMIT_DELAY_MS (2 s) Ruhezeit ohne weiteres Event freigegeben; verhindert Flash-Writes bei gehaltener Einstelltaste im Touch-REPEAT-Modus |
| 11v00 | Bugfix | wifiTask Double-Buffer Race: snprintf in datum_WiFi_tmp/zeit_WiFi_tmp nur wenn kein altes Paar pending (wifiSyncPending-Guard); schließt Torn-Read-Fenster gegen displayTask |
| 11v00 | Qualität | Log-Regel: Serial.printf mit Web-Log-URL bleibt bewusst nach WiFi-Connect im Serial-Monitor – die URL steht sonst nur im Web-Log selbst und wäre unerreichbar |

## Version 11v01

| Version | Kategorie | Änderung |
|---|---|---|
| 11v01 | Funktion | Web-Log: neue Rubrik „Status – Letzter Start" zeigt die Zeilen WiFi und NTP analog zur Info-Seite (datum_WiFi/zeit_WiFi, datum_sync/zeit_sync); platziert oberhalb der Touch-Baseline-Sektion |

## Version 11v03

| Version | Kategorie | Änderung |
|---|---|---|
| 11v03 | Bugfix | resetCount zeigte nach Werksreset 2 statt 1: `bumpResetCount()` in `setup()` wurde vor `loadWifiCredentials()` aufgerufen – Konfigurator-Boot nach NVS-Erase zählte ebenfalls mit. Aufruf jetzt hinter `loadWifiCredentials()`; `bumpResetCount()` öffnet NVR-Namespace selbst (begin/end). |

bTn Wecker  ·  Änderungshistorie  ·  Stand 11v03
