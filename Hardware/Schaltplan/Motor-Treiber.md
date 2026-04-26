# Motortreiber

```
5V ─ [AMS1117 3v3] ─┬─────────┬────── Motor+ ──────┬────────────┐
                    │         │         │          │    Kathode │
                 [100µF]   [100nF]   [Motor]   [10Ω+10nF]    [1N4448]
                    │         │         │          │      Anode │
                   GND       GND      Motor- ──────┴────────────┘
                                        │
                                        │
                                Drain ──┘
GPIO26 ─────── [330Ω] ────┬──── Gate             IRLML6344
                          │     Source ─┐
                        [10kΩ]          │
                          │             │
                         GND           GND
```
