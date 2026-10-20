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
8. **Water and run-on time:** dumps every minute after switching off for 0x0040 byte 2, and effect on with an empty tank.
9. **Other unknown registers:** 0x0051 (flips on its own), 0x0053, 0x0026, 0x003c. First only observe, then interpret.
10. ~~Temperature source 0x0055~~ **settled:** internal fireplace sensor (section 5).
11. **Thermostat:** set the setpoint below the room temperature (e.g. 16 with a room at 18) with a power meter and see whether the heating element switches off, and set it a few degrees above to check it keeps heating. Section 8 shows the element does not follow 0x0055 reaching the setpoint.
12. **History:** the bridge keeps about 120 rows (one row per change or per minute). For longer logs: lower the logging frequency or enlarge the buffer.
13. Only when everything above is done: a write test of one register at a time, with your hand on the power switch, because it is a heater.

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
