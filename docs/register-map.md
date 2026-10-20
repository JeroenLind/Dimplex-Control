# Register map: Dimplex Opti-myst electric fireplace (BLE)

Applies to: Dimplex Opti-myst fireplace with Bluetooth LE remote, Pro Range family (e.g. CAS400LNH). Device name `FI7539<Dimplex>`, firmware `EP4120`.

This document describes the GATT registers of the fireplace: handle, UUID, size, values and meaning. Registers are addressed by their **GATT handle** (the UUIDs are not unique or descriptive enough to look them up by). Access requires a bonded BLE connection with passkey entry (legacy pairing, 6-digit passkey). The fireplace accepts **one BLE connection at a time**, so the original remote cannot connect while another client is connected.

## Conventions

- Values are hex bytes in the order they are read or written. "LE16" is a 2-byte unsigned number, least significant byte first (`1400` = 20).
- "R" = readable, "R/W" = readable and writable.

Confidence levels:

- **Certain**: confirmed by reading and writing on a real device, or observed consistently in the official remote's traffic.
- **Plausible**: consistent with all observations and the user manual, but not specifically verified.
- **Unknown**: the value is known, the meaning is not.

## 1. Control and status

| Handle | UUID | Size | Access | Values | Meaning | Confidence |
|---|---|---|---|---|---|---|
| 0x0040 | 0x4008 | 2 B | R/W (write 1 byte) | `0000` off / `0601` effect on / `0101` heater on | **Main status.** Byte 1: `00` = everything off (heater too), `06` = flame effect on, heater off, `01` = flame effect on, heater on. Byte 2 is `01` while the flame effect is on and for a while after switching off (run-on indication), see the note below | Certain |
| 0x0014 | 0x1001 | 6 B | R/W (write 2 bytes) | `06..` heater off / `02..` heater on | **Heater status.** Byte 1 is `02` with the heater on and `06` with the heater off. The register shows which mode is switched on, not whether the heating element is drawing power at that moment (the thermostat cycles the element). Written only to start the heater; stopping is done through 0x0040 and the fireplace resets this register itself | Certain |
| 0x0042 | 0x4009 | 1 B | R/W | 1 to 6 | **Flame level** F1 to F6. Independent of on/off; the level is kept while the fireplace is off | Certain |
| 0x0018 | 0x1003 | LE16 | R/W | 0, or 15 to 30 | **Setpoint in °C.** Reads 0 while the heater is off. Writing only has an effect while the heater is on. The fireplace starts at 21 °C every time the heater is switched on, regardless of the previous setting. Valid range is given by 0x001e | Certain |
| 0x0034 | 0x1023 | LE16 | R | same as 0x0018 | Read-only mirror of the setpoint | Certain |
| 0x0048 | 0x4012 | 1 B | R/W | `01` low / `03` high | **Heater power:** `01` = low (0.9 kW), `03` = high (1.8 kW). The setting is stored by the fireplace: it is kept when the heater or the fireplace is switched off and after the controller restarts. Not tested after a mains power reset. Other values have not been seen | Certain |
| 0x004f | 0x0003 | 2 B | R | `0100` low / `0101` high | Byte 2 follows the heater power setting | Plausible |
| 0x0055 | 0x0008 | 3 B | R | `01 <degrees> <tenths>`, e.g. `011507` = 21.7 °C | **Temperature of the sensor inside the fireplace** (air near the heater), **not** the room temperature. It rises about 1 °C per minute with the heater on and keeps rising for several minutes after the heater is switched off; the fireplace has no external sensor. Resolution 0.1 °C, noise about ±0.1 °C | Almost certain |
| 0x0076 | 0x4010 | 1 B | R/W | `03` | Volume 0 to 6 on models with a sound effect. The FI7539 has no sound effect; the register exists and reads `03` but its effect has not been verified on this unit | Plausible |
| 0x0063 | 0x4004 | 2 B | R | `0000` standby; F1 `003a` (58), F2 `0042` (66), F3 `004b` (75), F6 `0064` (100) | **Intensity of the effect in %** for the current flame level, about 58 + 8.4 per step (so F4 about 83 and F5 about 92; not measured). Not the flame level itself (that is 0x0042) | Certain that it follows 0x0042; unit % plausible |
| 0x003c | 0x4003 | 3 B | R | `010000` standby, then roughly 960 to 1050 (`01` + LE16) | Measured value of unknown unit. Not related to flame level or heater power; roughly follows time and temperature. `ffff` (65535) occasionally appears as an invalid reading, also in standby | Unknown |
| 0x004d | 0x0002 | 3 B | R | `010000` / `010100` | Byte 2 is 1 while the flame effect is on ("effect active" flag) | Plausible |
| 0x005d | 0x4000 | 2 B | R | `0100` standby / `0164` effect on | Byte 2 = 0x64 (100) while the effect is on: "on + intensity" | Plausible |
| 0x005f, 0x0065 | 0x4001, 0x4005 | 2 B | R | `0100` standby / `0101` effect on | Byte 2 follows "effect on" | Plausible |
| 0x0053 | 0x0007 | 1 B | R | `00` or `03` | Becomes `03` after the fireplace has been switched off for the first time after power-up, and then stays `03` (also with effect and heater on). Not an effect flag; probably a session or start-up state | Unknown |
| 0x0051 | 0x0004 | 2 B | R | `0100` / `0101` | Changes on its own (counter or measurement). Not a setting | Unknown |

**Byte 2 of 0x0040.** It is `01` while the flame effect is on and still `01` about a minute after switching off, but `00` after about half an hour. This fits an "effect active or just switched off" flag (run-on time). Whether it also indicates a low water level (the manual mentions two beeps) has not been tested with an empty tank.

## 2. Command sequences

These are the writes the original remote uses. All writes are GATT write requests (with response). After a write the new state shows up in the status registers, usually within a few hundred milliseconds but sometimes later, so read back until the expected value appears instead of assuming a fixed delay.

| Action | Write(s) | Resulting state |
|---|---|---|
| Flame effect on | 0x0040 = `06` | 0x0040 = `0600`/`0601` |
| Heater on (only while the flame effect is on) | 0x0040 = `01`, wait about 0.65 s, then 0x0014 = `0200` | 0x0040 = `0100`/`0101`, 0x0014 = `02....`, 0x0018 = `1500` (21 °C) |
| Setpoint | 0x0018 = LE16 degrees (20 = `1400`, 19 = `1300`), heater must be on | 0x0018 and 0x0034 show the new value |
| Heater power | 0x0048 = `01` (low) or `03` (high), heater on | 0x0048 shows the new value |
| Flame level | 0x0042 = 1..6 | 0x0042 and 0x0063 change |
| Heater off (effect stays on) | 0x0040 = `06` | 0x0014 = `06....`, 0x0018 = `0000` |
| Everything off | 0x0040 = `00` | 0x0040 = `0000`, heater off |

Notes:

- Writing `06` to 0x0040 while the heater is on switches the heater off (it means "effect on, heater off").
- The remote reads back the written register after every write and polls 0x0040, 0x0014, 0x0024, 0x0034, 0x0036, 0x0026, 0x0042, 0x0048 and 0x0076 every few seconds.
- When connecting, the remote writes the clock (0x0012) and performs the handshake on 0x0087 (section 5).
- Whether the second write for heater on (0x0014 = `0200`) is strictly required has not been tested.

## 3. Limits and settings (read-only, constant)

| Handle | UUID | Value | Meaning | Confidence |
|---|---|---|---|---|
| 0x001e | 0x1006 | `0f001e00` | Two LE16: **15** and **30** = minimum and maximum setpoint in °C (30 matches the manual's maximum) | Plausible |
| 0x001a | 0x1004 | `07000f00` | Two LE16: **7** and **15**. The 7 matches the frost protection temperature (7 °C) in the manual | 7 plausible, 15 unknown |
| 0x001c | 0x1005 | `00` | Possibly the °C/°F unit | Unknown |
| 0x0038 | 0x1028 | `1800` | LE16 24, constant | Unknown |

## 4. Clock

| Handle | UUID | Size | Layout |
|---|---|---|---|
| 0x0010 | 0x0006 | 7 B | `century year month day hour minute flag` |
| 0x0012 | 0x0106 | 8 B | same, plus an eighth byte: seconds. **This is the register to write** to set the clock |

All fields are plain binary numbers (not BCD). Example: 2026-10-14 19:45:04 = `14 1a 0a 0e 13 2d 01 04`.

- **Certain:** the structure; the remote writes the clock every time it connects.
- **Plausible:** the eighth byte is seconds (it varies while the minute stays the same).
- **Plausible, not tested:** the seventh byte (`01`) is the daylight-saving flag. The remote always writes `01`.
- The fireplace keeps its own time; a client that writes the clock should use real local time, since a bogus date (for example 2019-01-01) is accepted and stays until it is corrected.

## 5. Session and handshake

| Handle | UUID | Value | Note | Confidence |
|---|---|---|---|---|
| 0x0087 | 0x2001 | `000200` | Handshake register. The original remote writes `010200`, reads `010702`, writes `000000`, reads `000200`. Writing `010100` followed by `000000` is also accepted. The purpose is not known | Unknown |
| 0x0085 | 0x1050 | `4000160000000000` | constant | Unknown |
| 0x0074 | 0x1050 | 8x `00` | constant | Unknown |

## 6. Weekly timer

The manual describes four periods per day (P1-P4), each with a switch-on time, a switch-off time and a temperature, for every day of the week.

| Handle | UUID | Function |
|---|---|---|
| 0x0016 | 0x1002 | **Select and read a day.** Write: `00 03 <day 0-6>` plus 17x `00` (20 bytes). Read: `00 03 <day>` + 4 slots of 6 bytes + 4 padding bytes |
| 0x0028 | 0x1012 | slot P1 of the selected day |
| 0x002a | 0x1013 | slot P2 |
| 0x002c | 0x1014 | slot P3 |
| 0x002e | 0x1015 | slot P4 |

Slot layout (in 0x0016: 6 bytes; in 0x0028-0x002e: header `00 03 <day>`, then 5 bytes, then `00 00`):

```
<on_hour> <on_min> <off_hour> <off_min> <temperature °C> [00]
```

An unused slot is `00 00 00 00 0f 00` (00:00-00:00, 15 °C). Example: `06 1e 07 2d 13 00` = 06:30-07:45 at 19 °C.

- **Certain:** the structure and that the values are readable times and temperatures. **Plausible:** day index 0 = Monday, 6 = Sunday.
- **Not verified:** whether writing these registers changes the stored program, and how timer mode is switched on and off. The mode selection register (manual / timer / frost protection) has not been found.

## 7. Identification (read-only, constant)

| Handle | UUID | Value | Meaning | Confidence |
|---|---|---|---|---|
| 0x0003 | 0x2a00 | `4649373533393c44696d706c65783e` | Device name: `FI7539<Dimplex>` | Certain |
| 0x0005 | 0x2a01 | `0000` | GAP appearance | Certain |
| 0x0007 | 0x2a04 | `ffffffff0a006400` | GAP preferred connection parameters | Certain |
| 0x0009 | 0x2aa6 | `00` | GAP central address resolution | Certain |
| 0x0095 | 0x200a | `455034313230` | Firmware version: `EP4120` | Certain |
| 0x009b | 0x200d | `4630303042354530` | Serial number, e.g. `F000B5E0` (also present in the advertisement while idle) | Certain |
| 0x008d | 0x2004 | `3c44696d706c65783e00...` | Text `<Dimplex>` (20 bytes, zero padded) | Plausible |
| 0x0093 | 0x2009 | `05` | unknown (hardware revision?) | Unknown |
| 0x0097 | 0x200b | `8e16` | unknown (LE16 5774) | Unknown |
| 0x0099 | 0x200c | `000000000000` | unknown | Unknown |
| 0x008f | 0x2005 | 32x `00` | empty text field | Unknown |

## 8. Other registers

All of these were constant in every observation unless noted, and their meaning is **unknown**.

| Handle | UUID | Value | Note |
|---|---|---|---|
| 0x0020 | 0x1007 | 9x `00` | |
| 0x0022 | 0x1008 | `00` | |
| 0x0024 | 0x100d | `00` | |
| 0x0026 | 0x1011 | `00` | Was `35` (53) once, so not strictly constant: counter or measurement? |
| 0x0030 | 0x1016 | `0010` | |
| 0x0032 | 0x1017 | `0003` | |
| 0x0036 | 0x1027 | `0000` | |
| 0x003a | 0x102a | `0013010100000700` | looks like the date 2019-01-01 (factory date?) |
| 0x003e | 0x4006 | `010000` | |
| 0x0044 | 0x400a | `0a02` | |
| 0x0046 | 0x400b | `04` + 8x `64` | eight times 100 after a `04`: brightness or channels? |
| 0x004b | 0x0001 | `00000000` | |
| 0x0057 | 0x100e | `00` | |
| 0x0059 | 0x1010 | `47` | 71 |
| 0x005b | 0x1020 | `000000` | |
| 0x0061 | 0x4002 | `0100` | |
| 0x0067 | 0x4007 | `01` | |
| 0x0069 | 0x4011 | `00` | |
| 0x006c | 0x1019 | `00` | |
| 0x006e | 0x0005 | `000000` | |
| 0x0072 | 0x1031 | `00` | |
| 0x0078 | 0x4013 | 6x `00` | |
| 0x007a | 0x4014 | `ff01020000000100 00` | |
| 0x007c | 0x4015 | 5x `00` | |
| 0x007f | 0x000a | `ffffffff00000000` | |
| 0x0081, 0x0083 | 0x000b, 0x000c | 13x `50` | text `PPPPPPPPPPPPP`: probably an empty or uninitialised field |
| 0x0089 | 0x2002 | 6x `00` | |
| 0x008b | 0x2003 | 3x `00` | |

## 9. Manual functions and their registers

| Function (manual) | Register | Confidence |
|---|---|---|
| Flame effect on/off | 0x0040: `06` on, `00` off | Certain |
| Flame level F1-F6 | 0x0042 (0x0063 = intensity in %) | Certain |
| Sound/volume (models with sound) | 0x0076 | Plausible |
| Heater on/off | on: 0x0040 `01` + 0x0014 `0200`; off: 0x0040 `06`; status in 0x0014 byte 1 | Certain |
| Heater setting HI/LO (1.8 / 0.9 kW) | 0x0048: `03` high, `01` low | Certain |
| Setpoint (default 21 °C) | write 0x0018, 0x0034 mirrors | Certain |
| Maximum / minimum setpoint (30 / 15 °C) | 0x001e | Plausible |
| Frost protection (7 °C) | 0x001a | Plausible |
| Temperature measurement (inside the fireplace) | 0x0055 | Almost certain |
| Weekly program P1-P4 | 0x0016, 0x0028-0x002e | Certain (structure) |
| Date and time, daylight saving | 0x0010, 0x0012 | Certain (daylight-saving flag plausible) |
| Mode selection: timer / manual / frost protection | not found | Open |
| °C / °F | 0x001c (?) | Unknown |
| Adaptive Start, child lock | not found | Open |
| Low water warning (two beeps) | 0x0040 byte 2 (?) | Unknown |

## 10. Open questions

- Mode selection (manual / timer / frost protection) and how the timer is switched on and off.
- The daylight-saving flag in the clock (does it change when the clock is set in winter time?).
- °C/°F and the meaning of 0x001c.
- The meaning of 0x0053, 0x0051, 0x0026 and 0x003c.
- Intensity values for F4 and F5 in 0x0063.
- Behaviour of byte 2 of 0x0040 with an empty water tank.
- Which measurement the fireplace uses internally for its thermostat.
- Whether the heater power setting (0x0048) survives a mains power reset.
