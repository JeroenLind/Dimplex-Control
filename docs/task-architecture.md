# Architecture specification: ESP32 task layout (Dimplex bridge)

Revised version of the proposal, checked against the sketch (originally 790 lines, version of 17 Oct). Section 1 says what in the proposal is right and what is not. Sections 2 to 7 are the specification that was implemented. Section 9 describes the current state of the code (identifiers are the current names; see `CHANGES.md` for the old ones).

---

## 1. Review of the proposal

### What is right
- The web server stays on core 1. The Arduino `loop()` already runs there, so nothing changes.
- MQTT in its own FreeRTOS task with `vTaskDelay` is the right direction.
- Protecting shared state with a mutex is necessary.

### What is wrong or missing
1. **The hiccup does not come from MQTT alone.** In the original `loop()` everything ran one after another on core 1, and much of it blocks:
   - `mqttReconnect()` every 3 s. `PubSubClient.connect()` waits for the TCP connection (several seconds) and then for the CONNACK (by default up to 15 s, depending on the PubSubClient version). With an unreachable or slow broker the page is almost permanently stalled.
   - `MDNS.queryHost(..., 2000)` blocks up to 2 s, repeated every 4 s while `homeassistant.local` does not resolve.
   - BLE connect: `connect()` has a 5 s timeout, a scan takes 4 s, repeated every 5 s. This happens when the remote holds the connection, the fireplace is off or out of range, or after a connect error in auto mode.
   - Polling every 8 s: a series of GATT reads of a few tens of ms each.
   - The web `/set` handler did BLE writes synchronously, with `delay(650)` + `delay(800)` + a state read. One click on "heater on" kept the web server busy for more than 1.5 s.

   Moving only MQTT to core 0 solves the first point (and part of the second). The acceptance criterion (<50 ms) would still not be met, because BLE work stays on core 1.

2. **`PubSubClient` is not thread-safe.** The proposal only protected the status variables. But `publish()` was also called from the web handler and the BLE paths. If `client.loop()` runs on core 0 while the web handler publishes on core 1, they share one buffer and one socket. Rule: **only the MQTT task touches the MQTT client and the Wi-Fi client.**

3. **The MQTT callback executed BLE commands.** In an MQTT task that would mean BLE I/O from core 0 while `loop()` on core 1 also reads BLE. NimBLE allows one running GATT operation per connection, so the second fails (`BLE_HS_EBUSY`) or the state gets confused. Rule: **only one task touches NimBLE** (the BLE task), and commands reach it through a queue.

4. **The "direct I/O" section mentions I2C/SPI/UART, but here BLE is the shared bus.** The same principle applies, with one difference: a mutex does not help, because a BLE operation takes seconds. Never hold a mutex during BLE or network calls. Ownership per task plus a queue is safer.

5. **Stack 4096 is too tight.** The discovery publishing builds JSON strings of about 700 B, and `String` use, `Serial.printf` and `PubSubClient.connect()` are in there too. Use 8192 for the MQTT task. The BLE task also gets 8192 (NimBLE client calls plus `printf`). Log `uxTaskGetStackHighWaterMark()` to verify.

6. **WDT criterion.** A blocking connect does not cause a watchdog reset, because lwIP calls yield the CPU. A task that never waits does: it starves the idle task of core 0. That is why `vTaskDelay` is mandatory in every task loop. The criterion is rewritten into something measurable (section 7).

7. **`std::atomic` is not enough.** The status consists of several related fields (register bytes, setpoint, history). One atomic per field gives a skewed snapshot. Use one structure under one mutex and let readers take a copy (section 4). Atomics/`volatile` only for single flags (`g_bleConnected`, `g_blePaired`, `g_bleEnabled`). The NimBLE callbacks write those from the host task.

8. **`String` in shared state.** A `String` that is modified in one task while another copies it can corrupt the heap. In the shared structure use `char[]`.

9. **Core 0 is already busy** (Wi-Fi, lwIP, the NimBLE host task, which runs on core 0 by default; check `CONFIG_BT_NIMBLE_PINNED_TO_CORE` in `nimconfig.h`). Priority 1 for the MQTT and BLE tasks is fine: they run below Wi-Fi/BT and mostly block on I/O. If core 0 turns out too busy, the BLE task can move to core 1 (its waiting then does not cost `loop()` anything). That is a setting, not an architecture change.

10. **Commands from the web page become asynchronous.** The result is no longer in the redirect response but appears on the page once the BLE task is done. That must be visible in the UI (section 5).

---

## 2. Overview of the tasks

| Task | Core | Prio | Stack | Owner of |
| :--- | :--- | :--- | :--- | :--- |
| **`loop()`** (Arduino) | 1 | 1 | default | `WebServer`, NTP start, mDNS start |
| **`mqttTask`** | 0 | 1 | 8192 | `WiFiClient`, `PubSubClient`, broker resolve |
| **`bleTask`** | 0 | 1 | 8192 | all NimBLE calls, BLE state, commands |
| **`ledTask`** | 1 | 1 | 2048 | the LED pins (reads a few volatile flags only) |
| Wi-Fi / lwIP / NimBLE host | 0 | system | n/a | system |

Who owns what is a hard rule:
- `loop()` touches neither BLE nor MQTT. It only reads a snapshot of the status and puts commands in the queue.
- `mqttTask` does not touch BLE. It puts commands in the same queue and reads a snapshot of the status.
- `bleTask` touches neither MQTT nor the `WebServer`. It writes the status and reads the command queue.
- `ledTask` only reads status flags and writes the LED pins.

Tasks start at the end of `setup()`, after the setup portal has finished and Wi-Fi is connected. The config portal stays blocking at start-up, which is not a problem. (`ledTask` starts earlier, so the LED can blink while the portal is open.)

---

## 3. Command queue (web and MQTT to the BLE task)

```cpp
enum CommandType : uint8_t { CMD_FLAME_EFFECT, CMD_HEATER, CMD_HEATER_POWER, CMD_SETPOINT, CMD_SETPOINT_DELTA,
                             CMD_FLAME_LEVEL, CMD_BLE_RELEASE, CMD_BLE_RESUME };
enum CommandSource : uint8_t { SOURCE_WEB = 0, SOURCE_MQTT = 1 };
struct Command { CommandType type; int16_t argument; CommandSource source; };
static QueueHandle_t g_commandQueue = xQueueCreate(8, sizeof(Command));
```

- Always send with `xQueueSend(g_commandQueue, &c, 0)` (no waiting). If the queue is full, show "Busy, try again" and do nothing.
- `CMD_SETPOINT_DELTA` (the +/- buttons) is only converted into an absolute setpoint in the BLE task, based on the value at that moment. Two quick clicks therefore add up.
- The BLE task handles one command at a time, in order of arrival. The existing logic is unchanged: heater only on when the effect is on, 650 ms between the two writes, setpoint only when the heater is on, clamped to the setpoint limits.
- After a command the BLE task publishes a fresh snapshot, so the new state goes to MQTT at once.
- `bleReleaseLink` does not leave the normal task loop: it sets `g_bleEnabled=false`, drops the link, and the BLE task then skips connect attempts until `CMD_BLE_RESUME`.

---

## 4. Shared state (BLE task writes, web and MQTT read)

One structure, one mutex, copying instead of holding for long.

```cpp
struct SharedState {
  bool valid, connected, ready, bleEnabled, haveTarget;                   // link state
  uint8_t regControl[2], regHeater[2], regHeaterPower, flameLevel, regTemperature[3], ...;   // live registers
  int setpoint, setpointMin, setpointMax;
  char message[96]; uint32_t messageTimeMs;                               // last result message and when it was set
  uint32_t lastReadMs;                                                    // when the registers were last read
  StatusSample history[HISTORY_SIZE]; int historyCount, historyHead; float tempMin, tempMax; uint32_t lastTempChangeMs;
};
static SharedState g_shared;  static SemaphoreHandle_t g_snapshotMutex = xSemaphoreCreateMutex();
```

- Writer (BLE task): `xSemaphoreTake(g_snapshotMutex, portMAX_DELAY)`, update fields, `Give`. Never a BLE read or write inside the lock: first read into local variables, then lock briefly and copy.
- Readers (`loop()`, `mqttTask`): take the lock, copy into a private static snapshot, give, then render or publish from the copy. Copying a few kB takes microseconds.
- Only one mutex in the whole program, so no deadlocks. No `Serial.printf` inside the lock.
- `g_bleConnected`, `g_blePaired`, `g_bleEnabled` etc. stay `volatile`, because the NimBLE callbacks write them from the host task.
- The configuration (`cfg_*`) is only read after `setup()` and needs no lock.

---

## 5. Task behaviour

### `loop()` (core 1)
```
every pass:
  start = micros()                         // measures the maximum gap between passes (diagnostics)
  if Wi-Fi connected:
    once: configTzTime(...), MDNS.begin(...), startWebServer()
    webServer.handleClient()
  delay(1)
```
So no MQTT reconnect, no BLE connect, no polling and no clock write in `loop()`.

Web handlers:
- `handleSet()` reads the arguments, puts one or more `Command`s in the queue and answers immediately with `303 /`. A few ms at most.
- `handlePage()` renders the frame plus the live block; `handleLive()` renders only the live block, which the page script fetches every 3 s (1 s while commands are pending or the link is connecting). `handleCsv()` renders the status log. All take a snapshot and render from it.
- The page has a **System** line: MQTT connected yes/no and number of failed attempts, BLE link, commands waiting, max `loop()` gap and max web handler duration, stack high-water mark per task, free heap.

### `mqttTask` (core 0)
```
loop:
  if no Wi-Fi:                             vTaskDelay(500); continue
  if server not known:                     resolve (mDNS/DNS), backoff on failure
  if not connected:                        connect, with backoff 3 s -> 6 -> 12 -> 30 s (reset on success)
                                           after connecting: availability, subscribe, discovery, forgetPublishedValues()
  else:                                    mqttClient.loop()
                                           take a snapshot; publish every value that differs from what was published
  vTaskDelay(10)
```
- The diff logic (`g_published*`) lives in this task. The BLE task knows nothing about MQTT.
- `onMqttMessage()` only converts topic and payload into a `Command` and calls `xQueueSend(..., 0)`. No BLE in the callback. Mapping: `power/set` to `CMD_FLAME_EFFECT`, `level/set` to `CMD_FLAME_LEVEL`, `heater/set` to `CMD_HEATER`, `setpoint/set` to `CMD_SETPOINT`, `ble/set` to `CMD_BLE_RESUME` / `CMD_BLE_RELEASE`. For on/off switches the expected state is echoed at once, and older values are held back until the command has finished (or 8 s have passed), so a Home Assistant switch does not flip back.
- `setSocketTimeout(3)` so a slow broker does not hold the task for 15 s.
- Broker unreachable: the task keeps retrying with backoff. The rest (web and BLE) does not notice.

### `bleTask` (core 0)
```
loop:
  if g_bleEnabled and not connected and 5 s since last attempt:   bleConnectToFireplace()      // may take seconds
  if connected+paired and not ready:                               findFireplaceCharacteristics(); sendInitSequence(); ready
  if ready and clock not set and NTP valid:                        writeFireplaceClock()
  if a command is in the queue (waits max 100 ms on the queue):    execute it, read back until confirmed, update snapshot
  if ready and 5 s since last poll:                                readFireplaceState(), update snapshot
```
- Waiting on the queue (`xQueueReceive(g_commandQueue, &c, pdMS_TO_TICKS(100))`) replaces a separate `vTaskDelay` and gives a fast response to commands.
- All existing `delay(...)` calls in the `ble*` functions are allowed: they are now in their own task.
- After a write, `readUntil()` reads back until the fireplace reports the new state (at most 3-4 s), so the snapshot never contains the old state as the "result" of a command.
- `onDisconnect` clears `g_bleReady`; the BLE task notices on its next pass and reconnects.
- The scan path (4 s) only runs without a fixed address. With `cfg_fireplaceAddress` filled in, no scan happens.

---

## 6. Migration of the original code

| Original | Becomes |
| :--- | :--- |
| `loop()`: MQTT reconnect, `mqttClient.loop()`, BLE connect, characteristic lookup, clock write, polling | `mqttTask` / `bleTask` (section 5) |
| `/set` handler: direct `ble*()` calls and `publish` | `Command` in queue, no publish |
| MQTT callback | only puts a `Command` in the queue (plus the optimistic echo) |
| polling and publishing | BLE part (reading) to `bleTask`, publish part (diff) to `mqttTask` |
| `g_msg` (`String`) | `char message[96]` in `SharedState` |
| `g_hist`, `g_st`, `g_hb`, ... | fields of `SharedState`, written under the mutex |
| broker resolve, MQTT reconnect | in `mqttTask`, with backoff |
| BLE release/resume | commands `CMD_BLE_RELEASE` / `CMD_BLE_RESUME` |
| `setup()`, setup portal, NimBLE init | stays; tasks and queue are started at the end |

---

## 7. Acceptance criteria (measurable)

Measure with the **System** line on the page and the serial log.

1. **Web response.** Max web handler duration and max `loop()` gap stay below 50 ms (typically <10 ms), measured over 10 minutes, in each of these situations:
   a. everything normal;
   b. broker unreachable (stop Mosquitto or block the port);
   c. fireplace out of range or off (BLE connect keeps failing);
   d. the remote holds the BLE link;
   e. a command is running ("heater on" takes about 1.5 s in the BLE task).
2. **No resets.** No watchdog reset or crash in 24 hours, also during (b) and (c). Stack high-water mark of both tasks stays above 1 kB.
3. **No heap leak.** Free heap fluctuates but does not decline over 24 hours.
4. **Commands.** Web and MQTT commands are executed in order and exactly once. The result is on the page within about 2 s. Two quick `+` clicks give +2.
5. **Consistency.** A page or publication never shows a mix of two states (for example setpoint from the new and temperature from the old poll). This follows from the snapshot under the mutex.
6. **Recovery.** After a Wi-Fi outage, a broker restart and a BLE reconnect the bridge republishes all values and the page stays usable.
7. **Functionally equal.** The existing control works as before the rebuild (effect, heater, setpoint, power, flame, `/csv`, Connect/Release).

## 8. Known limitations and choices

- The sketch cannot be compiled for the ESP32 in the author's environment (no Arduino libraries). The queue and snapshot logic can be checked with g++ stubs, but the first real test is on the ESP32.
- The space for `SharedState` (about 6 kB with 120 history rows) is in ordinary RAM; that is no problem. A longer history can be had by increasing `HISTORY_SIZE` or lowering the logging frequency.
- Core 0 for the BLE task is a choice. If the BLE connection drops more often after the rebuild, try core 1.

---

## 9. Implementation status

Everything in sections 2 to 7 is implemented in `Dimplex_MQTT_v2.ino`, plus the later additions (heater/setpoint/Bluetooth control via MQTT, status LEDs, live web block, read-back until confirmed, optimistic echo of MQTT commands).

### Deviations from the specification
- `pending` (commands in progress) is not in `SharedState`. The web handler calculates it from the queue length plus the flag `g_commandRunning`.
- The diagnostic values (max loop gap, max web request, stack, failed MQTT attempts, dropped commands) are separate `volatile` variables, not part of `SharedState`. Each is written by one task only and only displayed.
- Messages from the BLE task are shown for 8 s (`MESSAGE_SHOW_TIME_MS`) based on a timestamp. Messages the web layer produces itself ("Busy ...", "Timing maxima reset") are kept separately in `g_webMessage`.
- The link flags (`g_bleReady`, `g_bleConnected`, `g_blePaired`, `g_registersRead`, ...) are `volatile`, not `std::atomic`. The NimBLE callbacks write them from the host task, and each flag is a single 32-bit-aligned write. ThreadSanitizer reports them as races, but they were left this way deliberately (same pattern as the old code).
- An extra "reset" link on the System line (`/set?diag=reset`) to clear the maxima per test situation.

### What was tested (on a PC, with a simulated fireplace, broker and web client)
- web: `/set` answers in about 1 ms with 303, the BLE action follows afterwards, the result appears on the page;
- `-1`, `-1` shortly after each other gives 21 → 19;
- MQTT commands (`level/set`, `power/set`, `heater/set`, `setpoint/set`, `ble/set`) go through the same queue;
- a broker that does not answer for 3 s per attempt, for 10 s: slowest page response 5 ms, BLE commands keep working, backoff gives 2 attempts in 11 s, after recovery everything is republished;
- BLE link gone with reconnect attempts of 4 s: slowest page response 2 ms, then recovery and republishing;
- queue full (slow BLE): "Busy" message, 5 commands dropped, queue then drains;
- Release and Connect;
- a slow fireplace (reports its new state after 1.5 s): no flicker of the switch state on MQTT.
- Across all runs the slowest `handleClient()` was 4-5 ms and the longest pause between two `loop()` passes 8 to 35 ms (noise from the test machine, no blocking in the sketch).

### What could not be tested
- The sketch has not been compiled or run on a real ESP32 (no Arduino libraries in the author's environment). On the PC it was compiled against own stub headers without errors or warnings.
- Real NimBLE, Wi-Fi and lwIP timing. The stack size (8192) is an estimate: check "Free stack" on the **System** line of the page; if it stays below 1 kB, increase the stack.
- `setSocketTimeout()` requires PubSubClient 2.8 or newer.

### Known behaviour
- The BLE task only executes commands once a running `bleConnectToFireplace()` (up to about 5 s) has finished. If you click "Release" while the bridge is trying in vain to connect (because the remote holds the link), it can take up to 5 s to take effect. The page shows "Working..." meanwhile.
- Static RAM: three snapshots of about 4.5 kB (13.5 kB total), plus 2 × 8 kB task stacks.
