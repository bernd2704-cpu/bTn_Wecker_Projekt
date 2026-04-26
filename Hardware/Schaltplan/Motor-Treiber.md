# Motortreiber

```
3V3 ─[Ferrit]──┬─────────┬────── Motor+ ──────┬────────────┐
               │         │         │          │    Kathode │
            [100µF]   [100nF]   [Motor]   [10Ω+10nF]   [1N4448]
               │         │         │          │      Anode │
              GND       GND      Motor- ──────┴────────────┘
                                   │
                                   │
                           Drain ──┘
GPIO ──── [330Ω] ────┬──── Gate             IRLML6344
                     │     Source ─┐
                   [10kΩ]          │
                     │             │
                    GND           GND
```
