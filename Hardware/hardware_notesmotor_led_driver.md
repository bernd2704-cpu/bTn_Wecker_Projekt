# bTn Wecker – Hardware-Erweiterung: LED-Streifen & DC-Motor

## Versorgung
- Betriebsspannung Lastseite: 5V
- ESP32 GPIO-Pegel: 3,3V

---

## MOSFET: IRLML6344TRPBF (für beide Lasten)
- Logic-Level N-Channel, Vgs(th) = 0,4–1,0V → 3,3V GPIO tauglich
- Vds(max) = 20V, Id(max) = 5A
- Rds(on) bei Vgs=2,5V: ~30mΩ → Verlustleistung vernachlässigbar
- Bauform: SOT-23 (SMD)

### Beschaltung (gilt für beide Kanäle)
- Gate:  100Ω Reihenwiderstand zum GPIO
- Gate:  10kΩ Pull-Down nach GND (verhindert floating beim Boot/Reset)

---

## Kanal 1: LED-Streifen – GPIO27 (E3, Licht)
- Versorgung: 5V, Vf = 2,8V, If = 48mA
- Vorwiderstand: 47Ω / 0,25W in Serie (High-Side, zwischen 5V und LED+)
- Spannungsaufteilung: 2,2V am Widerstand, 2,8V an LED
- Freilaufdiode: nicht erforderlich (ohmsche Last)


---

## Kanal 2: DC-Motor – GPIO26 (E2, Wasserrad)
- Versorgung: 5V, Vnenn = 3V, Istall = 200mA
- Vorwiderstand: keiner (Strom ist lastabhängig → PWM statt Widerstand)
- Freilaufdiode: 1N4148, Kathode zu 5V, Anode zu Drain (schnelle Diode, <4ns)
- Entstörkondensator: 100nF Keramik direkt an den Motoranschlüssen

### PWM
```cpp
#define E2  26
ledcAttach(E2, 20000, 8);   // 20kHz, 8-Bit (über Hörschwelle → kein Surren)
ledcWrite(E2, 153);          // 60% Duty → ~3V Mittelwert
```

---

## Hinweise zur Integration
- GPIOs E2 (26) und E3 (27) sind bereits in SysConf_10v05.h definiert
- Stack-Größen und Task-Zuordnung gemäß bestehender SysConf-Konventionen
- Kein vTaskDelay() unter gehaltenem Mutex
- PWM-Initialisierung in setup() vor Task-Start