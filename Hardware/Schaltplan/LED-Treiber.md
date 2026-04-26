# LED-Treiber

```
5V ──────────── [47Ω / 0,25W]──── LED+
                                   │
                           LED-Streifen (2,8V / 48mA)
                                   │
                                  LED−
                                   │
                           Drain ──┘
GPIO ──── [330Ω] ────┬──── Gate             IRLML6344
                     │     Source ─┐
                   [10kΩ]          │
                     │             │
                    GND           GND

```
