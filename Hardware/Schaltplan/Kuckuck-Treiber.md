# Kuckuck-Treiber

```
5V ────────┬─────────┬────────── Modul+ 
           │         │             │    
        [100µF]   [100nF]   [Kuckuck-Modul] 
           │         │             │    
          GND       GND          Modul- 
                                   │
                                   │
                           Drain ──┘
GPIO ──── [330Ω] ────┬──── Gate             IRLML6344
                     │     Source ─┐
                   [10kΩ]          │
                     │             │
                    GND           GND
```
