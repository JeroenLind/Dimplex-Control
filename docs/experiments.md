# Experiment log: Dimplex Opti-myst fireplace (FI7539, firmware EP4120)

This file describes **how** the registers were deciphered: which dumps, which comparisons and which measurements. The result (what each register does) is in `register-map.md`.

Status: 20 October 2026, based on register dumps 01 to 16 (14 and 15 Oct), the sniffs of the remote (13 and 17 Oct) and the bridge's status log (17 Oct). Dump file names are in Dutch as originally created (`Referentie` = reference, `HaardLichtAan` = fireplace light on, `HeaterAan` = heater on, `HoogVermogen` = high power).

## 1. Sources and caveats

| Source | Used for |
|---|---|
| `01_Referentie.txt`, `01b_Referentie.txt`, `01c_Referentie.txt`, `02_HaardLichtAan.txt`, `03_HeaterAan.txt`, `04_TemperatuurSetpoint_20Graden.txt` | the dumps of the first day (handle, UUID, value) |
| `05_` to `16_` (15 Oct): setpoint 21 and 19, HI/LO power, heater off, flame levels F2/F6/F3/F1, fireplace off. `07_HoogVermogen.txt` is empty and nothing was changed at 07 and 08 according to Jeroen (so 08 is an extra noise measurement with the heater on) | the dumps of the second day |
| `Experiment_overzicht.md` | what was changed with the remote between the dumps |
| Manual Pro Range (ENG56-400 / ENG68-400 / ENG56-600), p. 3-8 | functions: HI/LO/OFF, setpoint, F1-F6, weekly timer, frost protection, limits |
| Sniff trace of the remote (`parse.txt`, 13 Oct) and the sniff of 17 Oct | write and read order of the remote |
| Status log of the bridge (17 Oct, `/csv`) | temperature curve, `ffff` in 0x003c, remembered power setting |
| Lammerink, `docs/PROTOCOL.md` | base registers 0x0040 / 0x0042 / 0x0076 / clock, same firmware |

Caveats about the data:

- **Dump 01 is incomplete, standby itself is not.** In dump 01, 0x0051 to 0x0069 are empty and all handles from 0x006c are missing. The standby dumps 01b and 01c (same tool, fireplace off) are complete, so standby does not hide registers. The clock in dump 01 (2019-01-01) most likely comes from the old `sendInit()` (see section 6). The `ffff` in 0x003c is no sign of "taken too early": `ffff` also occurs occasionally in ordinary standby and during operation. Why dump 01 is incomplete remains unknown. **Use 01b and 01c as the standby reference, not 01.**
- **Experiment 03:** the setpoint was 21 °C (`0x15`), as confirmed by Jeroen; an earlier description saying 20 °C was wrong.
- **The dumps are not entirely "clean":** every time the remote connects it writes the clock and walks through the 7 weekdays of the timer. As a result the clock and the day register of the timer (0x0016, 0x0028-0x002e) change between dumps even when no setting was changed.

## 2. First day: dumps 01 to 04 and standby

Only the registers whose value really differs, without the clock.

### 01b -> 01c: two standby dumps without any change (noise measurement)

Between 20:19:34 and 20:21:38 nothing was changed on the fireplace. Only this differs:

| Handle | UUID | 01b | 01c | Note |
|---|---|---|---|---|
| 0x0010/0x0012 | | `...1413...22` | `...1415...26` | clock |
| 0x0051 | 0x0004 | `0100` | `0101` | byte 2 changes by itself: counter or measurement |
| 0x0055 | 0x0008 | `011407` (20.7) | `011403` (20.3) | temperature reading drops 0.4 °C |

These are the registers that move on their own. In experiments, look only at other differences.

### 01b -> 02: flame effect (lights and evaporator) on, heater off

(Compared against the complete standby dump 01b; the old dump 01 is incomplete.)

| Handle | UUID | Standby (01b) | Effect on (02) |
|---|---|---|---|
| 0x0040 | 0x4008 | `0000` | `0601` |
| 0x004d | 0x0002 | `010000` | `010100` |
| 0x003c | 0x4003 | `010000` | `01c703` (967) |
| 0x005d | 0x4000 | `0100` | `0164` |
| 0x005f | 0x4001 | `0100` | `0101` |
| 0x0065 | 0x4005 | `0100` | `0101` |
| 0x0063 | 0x4004 | `0000` | `003a` |
| 0x0053 | 0x0007 | `03` | `00` |
| 0x0055 | 0x0008 | `011407` (20.7) | `011207` (18.7) |

Unchanged: 0x0014 (`06..`), 0x0042 (`01`), 0x0076 (`03`), 0x0061 (`0100`).

### 01 -> 02 (old dump, for reference)

| Handle | UUID | Before | After | Note |
|---|---|---|---|---|
| 0x0040 | 0x4008 | `0000` | `0601` | byte 1: `00` -> `06`; byte 2: `00` -> `01` |
| 0x004d | 0x0002 | `010000` | `010100` | byte 2: `00` -> `01` |
| 0x003c | 0x4003 | `01ffff` | `01c703` | `ffff` (no measurement) -> 967 |
| 0x0051-0x0069 | various | empty | filled | dump 01 is incomplete |
| 0x006c-0x009b | various | missing | present | same (incl. 0x0076 volume, 0x0087 handshake, firmware, serial number) |
| 0x0016, 0x0028-0x002e | 0x1002, 0x1012-0x1015 | day 0 | day 6 | **not a setting**: remote sync |

### 02 -> 03: heater on (setpoint 21 °C)

| Handle | UUID | Before | After | Note |
|---|---|---|---|---|
| 0x0040 | 0x4008 | `0601` | `0101` | byte 1: `06` -> `01`; byte 2 stays `01` |
| 0x0014 | 0x1001 | `060000000000` | `020000000000` | byte 1: `06` -> `02` |
| 0x0018 | 0x1003 | `0000` | `1500` | LE16 21 |
| 0x0034 | 0x1023 | `0000` | `1500` | LE16 21 |
| 0x003c | 0x4003 | `01c703` | `01e303` | 967 -> 995 (+28) |
| 0x0055 | 0x0008 | `011207` | `011303` | 18.7 -> 19.3 |

Unchanged: 0x0042 (flame level `01`), 0x0076 (volume `03`), 0x005d (`0164`) and all other registers.

### 03 -> 04: setpoint 21 -> 20 °C

| Handle | UUID | Before | After | Note |
|---|---|---|---|---|
| 0x0018 | 0x1003 | `1500` (21) | `1400` (20) | **setpoint** |
| 0x0034 | 0x1023 | `1500` (21) | `1400` (20) | follows the setpoint |
| 0x003c | 0x4003 | `01e303` (995) | `01e503` (997) | +2, keeps rising |
| 0x0055 | 0x0008 | `011303` | `011502` | 19.3 -> 21.2 in about 2 min |
| 0x0010/0x0012 | | `...132f...13` | `...1331...22` | clock only (19:47:19 -> 19:49:34) |

0x0040 (`0101`) and 0x0014 (`02..`) stayed the same: so the setpoint is **not** in those two registers.

### 04 -> 01b: fireplace fully off (standby)

| Handle | UUID | 04 | 01b | Note |
|---|---|---|---|---|
| 0x0040 | 0x4008 | `0101` | `0000` | both bytes back to 0 |
| 0x0014 | 0x1001 | `02..` | `06..` | heater status back to "off" |
| 0x0018 | 0x1003 | `1400` (20) | `0000` | **setpoint becomes 0 as soon as the heater is off** |
| 0x0034 | 0x1023 | `1400` | `0000` | same |
| 0x003c | 0x4003 | `01e503` | `010000` | 0 in standby |
| 0x004d | 0x0002 | `010100` | `010000` | effect flag off |
| 0x0053 | 0x0007 | `00` | `03` | see `register-map.md` |
| 0x0055 | 0x0008 | `011502` (21.2) | `011407` (20.7) | drops after switching off |
| 0x005d | 0x4000 | `0164` | `0100` | |
| 0x005f / 0x0065 | 0x4001 / 0x4005 | `0101` | `0100` | |
| 0x0063 | 0x4004 | `003a` | `0000` | |

So the setpoint is an **active value**: it is only present in 0x0018/0x0034 while the heater is on. The default of 21 at every start comes from the fireplace (see section 4).

## 3. Second measurement day, 15 Oct (dumps 05 to 16)

All dumps were taken one after another (18:44 to 19:01).

| Dump | What was changed | 0x0014 | 0x0018 / 0x0034 | 0x0040 | 0x0042 | 0x0048 | 0x004f | 0x0063 | 0x0055 | 0x003c |
|---|---|---|---|---|---|---|---|---|---|---|
| 05 | effect + heater on, setpoint 21 | `02` | 21 | `0101` | 1 | `01` | `0100` | `003a` | 19.6 | 986 |
| 06 | setpoint 19 | `02` | **19** | `0101` | 1 | `01` | `0100` | `003a` | 21.6 | 985 |
| 07 | (empty file, no change) | | | | | | | | | |
| 08 | no change (low power) | `02` | 19 | `0101` | 1 | `01` | `0100` | `003a` | 23.6 | 998 |
| 09 | **high power** (power meter confirmed) | `02` | 19 | `0101` | 1 | **`03`** | **`0101`** | `003a` | 24.3 | 1015 |
| 10 | **low power** (power meter confirmed) | `02` | 19 | `0101` | 1 | **`01`** | **`0100`** | `003a` | 23.8 | 1010 |
| 11 | heater off, effect stays on | **`06`** | **0** | `0601` | 1 | `01` | `0100` | `003a` | 24.3 | 1007 |
| 12 | flame F2 | `06` | 0 | `0601` | **2** | `01` | `0100` | **`0042`** | 24.7 | 1033 |
| 13 | flame F6 | `06` | 0 | `0601` | **6** | `01` | `0100` | **`0064`** | 25.0 | 1012 |
| 14 | flame F3 | `06` | 0 | `0601` | **3** | `01` | `0100` | **`004b`** | 25.0 | 1035 |
| 15 | flame F1 | `06` | 0 | `0601` | **1** | `01` | `0100` | **`003a`** | 25.2 | 1045 |
| 16 | fireplace off | `06` | 0 | **`0001`** | 1 | `01` | `0100` | `0000` | 25.2 | 0 |

Conclusions:

- **Heater power:** 0x0048 is `01` for low and `03` for high power; 0x004f byte 2 follows (`0100` low, `0101` high). Only in dump 09 do these two differ. All earlier dumps with the heater on (03, 04, 05, 06, 08) had `01`: the fireplace was set to **low**. Also in standby and with the heater off, 0x0048 stays at `01`: it is a stored setting that does not disappear.
- **Setpoint:** 0x0018 and 0x0034 are equal in all dumps (21, 19, then 0 with the heater off).
- **Flame level:** 0x0042 is 1 to 6 (F1, F2, F3, F6 seen). 0x0063 follows the flame level: F1 = 58, F2 = 66, F3 = 75, F6 = 100; about 8.4 per step, so F4 about 83 and F5 about 92 (not measured).
- **0x003c** is not related to flame level or power (F2 gives 1033, F6 1012, F3 1035); it more or less follows time and temperature.
- **0x0055** rises from 19.6 to 24.3 while heating and keeps rising to 25.2 after the heater is off (for six minutes): fits a sensor inside the appliance. In dump 06 the setpoint is 19 while the reading is 21.6, yet 0x0014 and 0x0040 stay at "heating on": those registers show the **switched-on** mode, not whether the element is drawing power.
- **Fireplace off (16):** 0x0040 is `0001`; byte 2 is still 1, one minute after the effect. In 01b and 01c (half an hour after switching off) byte 2 was 0 again: a run-on time or "recently on" flag.

## 4. Sniff of the remote, 17 Oct

Sniffed: fireplace on, heater on, setpoint 20, setpoint 19, heater off, fireplace off. The write commands are in `register-map.md`, section 6.

- **The default of 21 belongs to the fireplace:** after heater on, 0x0034 reads `1500` (21) although the remote did not write a setpoint.
- **Heater on** consists of two writes (0x0040 = `01`, 0.7 s later 0x0014 = `0200`). Whether the second is needed has not been tested.
- **HI/LO** was not sniffed, but tested with the bridge: writing `03` or `01` to 0x0048 (heater on) changes the power, verified with a power meter.
- Earlier sniff of 13 October: the remote wrote the date 8 May 2026 to the fireplace (the remote's clock was wrong at the time); in dump 02 it had been corrected. Day programs from that trace (day 5 missing):

| Day index | P1 | P2 | P3 |
|---|---|---|---|
| 0 | 06:30-07:45, 19 °C | 16:50-18:00, 19 °C | empty |
| 1 | 06:30-07:40, 18 °C | 16:45-18:00, 19 °C | empty |
| 2 | 06:30-07:50, 19 °C | 16:45-18:00, 19 °C | empty |
| 3 | 06:30-07:30, 19 °C | empty (19 °C) | 16:55-18:00, 19 °C |
| 4 | 06:30-07:40, 19 °C | 16:45-18:00, 19 °C | 12:30-13:10, 19 °C |
| 6 | 08:30-09:30, 19 °C | empty | empty |

## 5. Temperature reading 0x0055: is it the room temperature?

Conclusion: no, it is the fireplace's internal sensor. Evidence:

- Standby dumps 01b/01c: 20.7 and 20.3 (20:19 and 20:21); effect on 18.7 (19:45, the atomised water cools); heater on 19.3 and 21.2 (19:47 and 19:49). Dumps 02-04 were taken earlier in the evening than 01b/01c.
- **Status log 17 Oct (15:10-15:14, bridge, about 5 s per row):** standby 18.5-18.7 (n=7, mean 18.59, sd 0.08, so noise ±0.1); effect on changes nothing (mean 18.53). Heater on (15:12:12): +0.3 °C after about 22 s, +0.5 after about 46 s, +1.0 after about 60 s; slope about 1.0 °C/min over 82 s (1.1 °C/min at low power). Heater off for 14 s (15:13:35-49): the value does not drop (19.8 -> 20.3), it keeps rising.
- A room of this size does not warm up that fast. Confirmed by the owner: the fireplace has no external sensor and the room temperature barely rose during the test. No reference thermometer was used.
- Consequence for Home Assistant: do not use 0x0055 as `current_temperature` of a climate entity without a caveat; use a separate room sensor.

**0x003c and `ffff`:** in the status log of 17 Oct, `ffff` appeared in 4 of 37 rows (15:10:54, 15:11:34, 15:12:42, 15:13:59), with no clear link to a state change. Valid values 966-1009 (mean 985) with the effect on.

**0x0048 is remembered:** in the status log of 17 Oct the power was already on high right after the bridge started, so it stays after switching off and after restarting the ESP32.

## 6. Clock reset in the own firmware (fixed)

The old `sendInit()` in `Dimplex_MQTT.ino` wrote `14 13 01 01 00 0a 01 07` to 0x0012 on every connection. With the clock structure that is 2019-01-01 00:10:07, so the bridge reset the fireplace clock to 2019 on every connection. That most likely explains dump 01 (2019-01-01 22:00).

Now: the firmware gets the time via NTP and writes the real local time in the same format as the remote (`141a0a0e132d0104` = 2026-10-14 19:45:04). If there is no valid time yet, the clock is not written and retried later. The flag byte is always `01`, as the remote writes it.

## 7. Suggested follow-up experiments

Use Lammerink's method: dump A, disconnect the ESP32, change **one** setting on the remote, reconnect the ESP32, dump B, compare.

1. ~~Noise dump~~ **done** (01b -> 01c and 06 -> 08): only the clock, 0x0051, 0x0055 and 0x003c move on their own.
2. ~~Setpoint~~ **done** (21, 20, 19, also sniffed): 0x0018 is writable, 0x0034 mirrors it. The default 21 comes from the fireplace.
3. ~~HI/LO~~ **done** (read and write via 0x0048). Remembering is confirmed; mains power reset still to be tested. Still to do: F4 and F5 for 0x0063.
4. **Mode:** manual, timer and frost, one dump per mode. The mode-selection register has not been found yet.
5. **Clock-related registers** (volume is not relevant: the fireplace has no sound).
6. **°C -> °F and lowering the maximum setpoint**, for 0x001c and 0x001e.
7. **DST on/off** and moving the clock by an hour, for the flag in 0x0010 (DST ends on 25 October 2026 in Europe).
8. **Water and run-on time:** dumps every minute after switching off for 0x0040 byte 2. ~~Effect on with an empty tank~~ **done** (section 9): 0x003c drops to 3. Still to do: slowly lowering the water level to find the value at which the fireplace beeps.
9. **Other unknown registers:** 0x0051 (flips on its own), 0x0053, 0x0026. First only observe, then interpret. Since 11 Nov the bridge watches 0x0051, 0x0026, 0x0036 and 0x0024 (raw MQTT sensor, Debug page, CSV). Section 10: 0x0051 flips in standby too, so it is not the heating element; still to do: a run with the heater on and the power meter to see if it follows anything, and note any change of the other three. 0x003c is explained in section 9.
10. ~~Temperature source 0x0055~~ **settled:** internal fireplace sensor (section 5).
11. **Thermostat:** set the setpoint below the room temperature (e.g. 16 with a room at 18) with a power meter and see whether the heating element switches off, and set it a few degrees above to check it keeps heating. Section 8 shows the element does not follow 0x0055 reaching the setpoint.
12. **History:** the bridge keeps about 120 rows (one row per change or per minute). For longer logs: lower the logging frequency or enlarge the buffer.
13. Only when everything above is done: a write test of one register at a time, with your hand on the power switch, because it is a heater.

## 9. Empty water tank, 10 Nov (Home Assistant history of the raw registers)

Setup: effect on, heater on (low power, setpoint 18), flame level F1. At about 16:30 local time (15:30 UTC; the Home Assistant export is in UTC, which explains the one-hour difference in the logging) the water shortage was simulated by lifting the ultrasonic mist maker (a 24 V piezo disc, of the kind sold as a pond fogger) out of the water and putting it back. The bridge published the registers every 5 s; a power meter measured the fireplace. Two exports: 14:56 to 15:00 UTC (effect on, heater off) and 15:00 to 15:32 UTC (heater on).

| UTC | 0x003c | 0x0063 | 0x0040 / 0x0014 | Power |
|---|---|---|---|---|
| until 15:30:00 | 1050-1090 | `003a` | `0101` / `0200` | about 995 W |
| 15:30:03 to 15:30:29 | **3** (`010300`) | `003a`, then `0000` from 15:30:19 | unchanged | 845 W at 15:30:16 |
| 15:30:33 and :39 | 1053, 1042 | `0000` | unchanged | |
| 15:30:43 | 3 | `0100` (once) | unchanged | 991 W |
| 15:30:50 | 3 | `003a` | unchanged | |
| from 15:30:54 | 1056-1098 | `003a` | unchanged | about 1000 W |

- **0x003c is a measurement of the mist maker being in water (or not).** It is about 1000 to 1100 with the disc under water and **3** with the disc out of the water; 7 samples of 3 in 47 s, exactly during the empty tank, while the water was being put back. Earlier `ffff` values (section 4) are not 3, so they are still an invalid reading.
- **Probably it measures the load on the disc, not a level.** Such mist makers have no level sensor of their own; a disc that is out of the water is unloaded, so its drive current or feedback collapses. That fits the small drift with temperature (the disc load depends on the water temperature), the 0 in standby (not driven) and the 3 within one poll of lifting it. Consequences: it detects "disc dry", which an empty tank also causes, but a bubble or lime scale on the disc could give the same reading, so a Home Assistant sensor should need several readings in a row. This is an interpretation, not measured.
- **0x003c reacts first, the effect follows later.** At 15:30:33 and :39 the value is back at ~1050 (disc in the water again) while 0x0063 stays `0000` until 15:30:50: the fireplace restarts the effect about 15 s after the disc is wet again. The 3 at :43 and :50 shows a second dry reading while the effect was already restarting (the disc was probably lifted again or still wet-dry; the exact handling during the test was not logged).
- **The effect switches off by itself** (0x0063 `0000`, power down about 150 W, which is the effect's share: with the heater off and the effect on the meter shows about 164 W). The heater keeps running, as observed.
- **0x0040 and 0x0014 do not change.** Byte 2 of 0x0040 is therefore not a low water indicator, and the "flame effect on" bit stays set while the lamps are off. A bridge that wants to detect low water has to read 0x003c (and 0x0063).
- **Normal range.** Over 469 readings with water: 1002 to 1098, mean 1029 with the heater off and 1059 with the heater on. In the heater-on window the value correlates a little with the internal temperature (r = 0.41); with the heater off there is no correlation (r = 0.03). The change is small against the gap to 3, so a threshold anywhere between 100 and 900 separates the cases. The value at which the fireplace itself beeps is not known (only empty and full were tried).
- **Not yet known:** the transition values (a tank that is nearly empty), whether the fireplace beeps with `0x003c` = 3 and what happens to 0x003c when the effect is off for a long time (standby gives `010000`).

## 10. Four days of history, 10 to 14 Nov (a real dry run on 13 Nov)

Source: Home Assistant history of the raw registers (older format `40 14 18 48 55 3c 63`, about 54,000 rows, one every 5 s) with the power meter of the fireplace. The export is in UTC.

**Real run-out on 13 Nov, 11:14:42 UTC.** The effect had been on for a long time and ran out of water (no test this time).

| UTC | 0x003c | 0x0063 | 0x0040 | Power |
|---|---|---|---|---|
| until 11:14:37 | 1110-1136 | `003a` | `0601` | 170 W |
| 11:14:42 | **3** in one step (no decline before it) | `003a` | `0601` | |
| 11:14:52 | 3 | **`0000`** (effect off, about 10 s later) | `0601` | 0-159 W |
| 11:15:53 | 3 | `0000` | **`0600`** (byte 2 falls to 0 about 70 s after the step) | 0 W |
| 16:10:54 | back to **974** (water added) | `0000` | `0100` | |
| 16:11:45 | 1010 | `0100`, 4 s later `003a` | `0101` | 977 W |

- **The effect is off for 5 hours, the heater is not.** The fireplace kept heating at about 820 W (low power, with the effect off) for hours, in several heating periods, while the mist maker was dry. The fireplace has no dry-run protection for the heater; only the mist and the flames stop. With the effect running the same heater draws about 975 W, so the effect itself is about 155 W.
- **0x003c does not announce the empty tank.** Over the hours before, it was stable at 1095 to 1165. The value goes from about 1120 to 3 between two readings.
- **After refilling, 0x003c reacts first, the effect follows much later.** The value was back at 974 within one reading of the water being added (16:10:54). The effect started again 51 s later (`0100` at 16:11:45, `003a` at 16:11:49); in the test of 10 Nov it was about 17 s.
- **Byte 2 of 0x0040 does follow, but late.** It is `01` while the mist is really running and falls to `00` about 70 s after the water has gone (in the 47 s test of 10 Nov it had not fallen yet, which fits). It also is `00` for about 50 s right after the effect has been switched on, until the mist starts. After switching off it stays `01` for a while (`0001`, 56 rows). So byte 2 is a "mist maker is running" flag that lags.

**Start-up of the effect (about 50 s).** After `0x0040` becomes `0600`, 0x003c alternates between `0000` and `ffff` for about 50 s while 0x0063 is still 0. Then 0x0063 becomes `0100` (once) and `003a`, byte 2 of 0x0040 becomes `01`, and 0x003c reads about 950 to 1000 (12 Nov 16:18:49 to 16:19:42; 13 Nov 05:32:34 to 05:33:24). The same alternation of `0000` and `ffff` is what 0x003c shows in standby (11,023 runs of 0 and 11,020 runs of `ffff`). So **0 and `ffff` both mean "nothing is measured"**, not "invalid".

**Consequence for the Low water sensor.** Version 1 counted 0 as a dry reading, and the 20 s start-up wait was shorter than the real 50 s. Replaying the four days showed two false alarms (12 Nov 16:19:21 for 36 s and 13 Nov 05:33:14 for 25 s). Fixed: 0 and `ffff` are now ignored. The replay of the corrected logic gives only the two real events: the test of 10 Nov (on 15:30:19, off 15:31:09) and the run-out of 13 Nov (on 11:14:52, 10 s after the step; off 16:11:09, 15 s after the water was back).

**0x003c warms up.** In 7 sessions (effect running, 80 to 340 minutes) the value starts at 984 to 1024 in the first 5 minutes, rises about 1.5 % per 30 min and flattens at about 1100 to 1130 after 2 to 3 hours (per 30 min: 1014, 1062, 1090, 1108, 1122, 1127, 1130). Every session starts at about 990 again, also when the previous one ended an hour before. This looks like the temperature of the water or the disc, not the water level. It does not touch the wet/dry limit (about 950 or more against 3).

**New registers (about 6 minutes, 29 rows, all in standby).** `0x0051` flips between `0100` and `0101` all the time **in standby, with the heater off**: it is therefore not the state of the heating element. `0x0026`, `0x0036` and `0x0024` stayed `00`, `0000` and `00`. A run with the heater on has not been logged yet.

## 11. Run-out on 18 Nov with register 0x0026 (Home Assistant history, 18 to 19 Nov)

Source: Home Assistant history export in UTC (about 35 hours, 14,410 rows of the raw registers `3c= 63= 51= 26= 36= 24=`, power meter, heater element, low water, room sensor, bridge diagnostics). Flame level F1 and heater power low did not change in the whole export (one row each at the start), so everything below is **F1, low power**. The tank was not measured; the filling times are known only from the log.

| UTC | Event | 0x003c | 0x0063 | 0x0026 | Power |
|---|---|---|---|---|---|
| 18 Nov 11:43:11 | fireplace on (effect and heater) | `ffff`/`0000` | `0000` | `00` | 999 W |
| 11:43:44 / 11:43:49 | effect starts (33 s and 38 s after the power rose) | about 980 | `0100`, then `003a` | `00` | about 1010 W |
| 15:03:40 | **run-out** | **3** in one step, no decline before it (1091 to 1149 in the minutes before) | `003a` | `00` | 165 W to 155 W |
| 15:03:50 | | 3 | **`0000`** | **`33`** | 0 W at 15:03:54 |
| 15:03:50 to 05:48:23 | empty, 14 h 45 min | 3, sometimes 2 (89 readings) | `0000` | `33` (about 4,000 readings) | 0 W; the heater element still cycles at about 850 W |
| 19 Nov 05:48:13 | water added | | | | 13 W |
| 05:48:18 | | **back to 976** | `0000` | `33` | |
| 05:48:23 | | about 1000 | `0000` | **`00`** | |
| 05:48:24 | | | `0000` | `00` | **167 W** (the effect runs) |
| 05:49:08 / 05:49:14 | | about 1000 | `0100`, then `003a` | `00` | 167 W |
| 07:25:01 | fireplace off by hand | | `0000` | `00` | |

- **The tank lasted at least 3 h 20 min at F1** (effect on from 11:43:44, dry at 15:03:40). It is a lower bound: the tank may not have been full at 11:43 (it was not logged when it was last filled).
- **0x0026 is a dry flag.** It was `00` in every reading except in the single dry period (about 4,000 readings `33`, about 10,000 readings `00` otherwise), so no false positive in 35 hours. It switched at most 10 s after 0x003c fell to 3 (and in the same reading as 0x0063 going to 0) and went back to `00` 5 s after 0x003c returned to normal, so it follows the water and is not latched until a restart. It does not need the effect to be running: it stayed `33` for 14 hours with the effect off. **The meaning of the value `0x33` is not known**: it may be a single flag that happens to read `33`, or a bit field. It was `35` once before (register-map.md), so it is not a fixed constant either.
- **Detection by the fireplace itself.** There is no level sensor in the tank, only the mist maker (confirmed by the owner). The fireplace tests the disc and sees "dry" as soon as it is not loaded; 0x0026 and the effect switch-off follow within seconds. Nothing in the log announces an empty tank beforehand: neither 0x003c (no trend) nor the power (flat at 165 W until the step).
- **The heater keeps running with an empty tank** (once the thermostat calls for heat: 9 heating periods of about 11 minutes each during the dry night, starting every 34 to 55 minutes, at about 850 W). Only the mist and flames stop.
- **The fireplace tests again by itself.** While it is dry 0x003c keeps reading 3 (sometimes 2), so the unit keeps probing the disc. The effect restarts on its own after refilling; about 10 s pass between pouring the water (13 W at 05:48:13) and the effect power (167 W at 05:48:24), which matches what the owner sees (a few seconds before the light effect comes back).
- **0x0063 and the power meter disagree at the start by 30 to 45 s.** The power rises when the effect starts, 0x0063 follows `0100` about 33 s (11:43) and 44 s (05:49) later; at switch-off 0x0063 follows within one reading. For "is the effect really running" the power meter is faster than 0x0063.
- **Low water sensor flap at 21:25.** `binary_sensor.low_water` went off at 21:25:02 and on again at 21:25:43 while 0x0026 stayed `33` and 0x003c stayed 3. Most likely this is the designed reset on a Bluetooth disconnect (design.md section 5.6; the export has no Bluetooth connection counter to confirm it): after the reconnect the 20 s start-up grace plus the 10 s dry hold plus the reconnect time add up to the 41 s seen. The flap would not have happened if the state were kept over a link loss, or if 0x0026 were used as a second signal.
- **Register 0x0051** keeps flipping between `0100` and `0101` with the effect on, off and dry, so it is still not a water or heater signal; 0x0036 and 0x0024 stayed `0000` and `00` in the whole run, also during the dry period and with the heater cycling, so they are not dry flags and not fault flags for heater cycling.
- **Warm-up of 0x003c** again: 987 to 1119 in the first 2.5 h of the 18 Nov run (about 1000, 1040, 1080, 1107 and 1119 per 30 min) and 966 to 1099 in the 19 Nov run (96 min), the same curve as in section 10.

**Bridge memory (diagnostic sensors in the export).** Free stack of the BLE task: 5792 B at the start of the export, then 5744, 5680 and 5648 B after the three heating periods at 01:11, 03:11 and 05:23 on 19 Nov (it only falls when a deeper code path is run for the first time; 144 B in 35 h, flattening). Free stack of the MQTT task: 5452 B, unchanged. **Lowest free heap: 5128 B, unchanged in 35 h.** The stacks are comfortable (about 70 % and 67 % free of 8 kB). The 5 kB lowest free heap is low in absolute terms, but it did not move in 35 hours of heating cycles, a dry period, a refill and BLE reconnects, so there is no leak visible in it. It probably comes from the start-up (Wi-Fi, BLE, TLS or the portal) and was reached before the export began. The current free heap, the restart count and the uptime are not in this export, so the margin and any restart cannot be judged from it.

## 8. Home Assistant history of 19 Oct (bridge via MQTT, power meter, room sensor)

Source: a Home Assistant history export (times below are local time, UTC+2). Entities: the bridge's switches and sensors, a power meter on the fireplace socket and a separate Bluetooth room thermometer.

**Power consumption (power meter)**

- Flame effect only: about 154-166 W (steady about 166 W; the first minute was lower, about 154 W).
- Flame effect + heater on low: 987-1004 W, so the heating element draws about **825 W** on low power. Fits the "0.9 kW" label. High power was not used in this session.
- Switching on shows up in the meter within 1-2 s of the state change. Switching the heater off was visible in the meter 7.4 s after the state change (the meter may report late, or the fireplace may keep the element on briefly; unexplained).

**Internal temperature and the thermostat (relevant for experiment 11)**

- Heater started at 19:02:46 with the setpoint at 21 °C (the fireplace's default; the setpoint entity still showed 19 from the previous session until the heater started).
- The internal sensor (0x0055) rose from 18.3 °C to 22.2 °C in 11.5 minutes while the power stayed at about 990 W the whole time. It passed the setpoint of 21 °C at about 19:08 (a quarter of the time in) and was 1.2 °C above it when the heater was switched off by hand at 19:14:23: **the heating element did not switch off when 0x0055 exceeded the setpoint.**
- The room thermometer stayed at 18.2-18.4 °C during the whole session. So the fireplace probably regulates on a different sensor than 0x0055 (for example a sensor in the air inlet, which sees room air), and 0x0055 is a sensor in or near the warm air stream. This is a conclusion from one run; a run with a setpoint below the room temperature would confirm it.
- After the heater was switched off, 0x0055 kept rising from 22.2 to 23.0 °C within 2 minutes (still rising), as seen before.
- With the effect on and the heater off, 0x0055 sat at 17.6-18.0 °C, about 0.5 °C below the room thermometer (the mist cools the sensor).
- Initial heating slope here was about 0.5 °C/min, half of what was seen on 17 Oct (about 1.0 °C/min); the cause is unknown.
- Noise: consecutive readings jump by 0.1-0.3 °C (for example 17.7 / 17.8 / 17.9) without any change in conditions.

**Bridge behaviour seen in the data**

- Readings arrive in Home Assistant about every 5 s while the temperature changes, which matches the 5 s poll interval.
- After the bridge restarted (about 18:51), Home Assistant showed the old retained value (20.9 °C) for about 30 s; the first real reading after the Bluetooth link came up was 17.9 °C. State topics are retained, so stale values are shown while the fireplace link is down.
- At 18:54:23 the fireplace switch went on (the meter showed 154 W 2 s later), went off at 18:54:38 (meter 0 W 1.3 s later, so the fireplace **really** switched off) and went on again at 18:55:43. This is the "on, off, on" the owner noticed. Because the power really dropped, this was not only a display glitch caused by a late read-back; what sent the off command at 18:54:38 was not established (check the Home Assistant logbook for the context of that state change and the bridge's serial log).
