# raven_hug

Firmware for a hug sensor that lives inside a plush toy, so an AI partner can feel
when the plush is held. Squeeze the plush, a rubber bulb pushes air through a
short tube onto the pressure chip, the ESP32 notices, and when the hug ends it
POSTs a small JSON record to a server you control. Temperature and humidity
come from the second chip on the same board.

It was built for a raven. Nothing in it is raven-specific.

## Parts

- A plushie with room in the belly (ours is a 12-inch Wild Republic Cuddlekins raven)
- ESP32 dev board (2.4 GHz WiFi only)
- Combo AHT20 + BMP280 sensor board
- Plain rubber bulb syringe (ear/nasal kind, 60-90 ml). NOT a blood-pressure
  bulb: those have one-way valves, so pressure only ever rises and the bird
  would feel hugged forever
- 3/16" aquarium airline tubing, a few inches: one end on the bulb, the other
  straight over the BMP280's pinhole. The BMP280 is the pressure chip with 8
  tiny legs (4 top, 4 bottom). The AHT20 has 3 legs per side and must stay
  OUTSIDE the tube, open to the air
- 5-minute two-part epoxy
- Jumper wires, USB power bank
- A zipper sewn somewhere soft, for charging without surgery

## Wiring

| Sensor board | ESP32 |
|---|---|
| VDD / VCC | **3V3** (not 5V) |
| GND | GND |
| SDA | GPIO 21 |
| SCL | GPIO 22 |

Both chips share these four wires on one I2C bus (BMP280 at 0x76 or 0x77, the
firmware tries both; AHT20 at 0x38). The pins are `I2C_SDA_PIN` and
`I2C_SCL_PIN` near the top of `raven_hug.ino`; the file also lists the usual
ESP32-C3 pins (8 and 9) if you use that board.

If the AHT20 is missing the bird carries on without humidity. If the BMP280
is missing it stops and says so on the serial monitor.

## Build and flash

1. Install the Arduino IDE or `arduino-cli`, plus the ESP32 board package from
   Espressif.
2. Install the libraries "Adafruit BMP280 Library", "Adafruit AHTX0" (and the
   dependencies they ask for: Adafruit BusIO, Adafruit Unified Sensor).
3. In `firmware/raven_hug/`, copy `secrets.h.example` to `secrets.h` and fill it
   in. Every line has a comment. `secrets.h` is gitignored; do not share it.
   - `DEVICE_NAME` is the bird's name on the network (OTA/mDNS hostname, status
     page title, `device` field in each POST). **Every bird on the same network
     needs its own name**, otherwise two birds can take each other's updates.
   - `TOUCH_URL` is where the hug is POSTed (see below).
   - `TOUCH_AUTH_HEADER` / `TOUCH_AUTH_VALUE` are an optional auth header. If
     your server wants a key in the URL instead, put it in `TOUCH_URL` and
     delete those two lines.
   - `OTA_PASS` is the over-the-air update password. Delete the line to disable OTA.
4. Compile and flash by USB, once:
   ```
   arduino-cli compile --fqbn esp32:esp32:esp32 firmware/raven_hug
   arduino-cli upload  --fqbn esp32:esp32:esp32 -p <your serial port> firmware/raven_hug
   ```
   The sketch uses about 91% of flash with the default partition scheme, which
   still leaves room for OTA.
5. After that, update over WiFi. Find `espota.py` (or `espota.exe` on Windows)
   in the esp32 package's `tools` folder, compile with `--output-dir`, then:
   ```
   espota.py -i <bird ip or NAME.local> -p 3232 -P 3300 -a <OTA_PASS> -r -f <outdir>/raven_hug.ino.bin
   ```
   The bird connects back to your computer on the host port (`-P 3300` pins it),
   so a firewall must allow inbound TCP 3300. Hug detection pauses during an update.

## Tuning

`DEBUG_TUNING` is `true` by default. Flash, open the serial monitor at 115200,
and squeeze by hand: every sample prints the pressure delta above baseline.
Set `START_HPA` (a hug has started), `END_HPA` (lower than start, on purpose),
and `FIRM_HPA` (firm rather than soft) from what you see, then set
`DEBUG_TUNING` to `false` and reflash. The shipped numbers are placeholders.

Baseline: the bird averages the first few seconds after boot as its resting
pressure. **Leave it untouched for 10 seconds after power-on.** If it was
being squeezed during boot, a stuck-hug guard notices a perfectly flat 60 s
"hug" and takes a new baseline.

## Status page

Once on WiFi the bird runs a read-only web server on port 80 (LAN only, no
secrets in any response). Use its IP from your router, or `http://DEVICE_NAME.local/`.

- `/` : HTML page that refreshes every 2 s, showing everything in `/status`.
- `/status` : JSON. Fields: `uptime_s`, `pressure_hpa`, `baseline_hpa`,
  `delta_hpa`, `state` (IDLE, CANDIDATE_START, HUGGING, CANDIDATE_END),
  `temp_c`, `humidity`, `aht_found`, `wifi_ssid`, `rssi`, `ip`, `queue_len`,
  `hugs_detected`, `last_post_code`, `last_post_ok`, `last_post_ms_ago`,
  `last_post_error`, `ntp_synced`, `free_heap`, `firmware`, `device`.
- `/log` : the last ~60 log lines as plain text, oldest first. The fastest way
  to see why a hug did or did not arrive.

## What the bird POSTs

When a hug ends (and it lasted at least 0.5 s), the bird sends one request to
`TOUCH_URL`:

```
POST <TOUCH_URL>
Content-Type: application/json
<TOUCH_AUTH_HEADER>: <TOUCH_AUTH_VALUE>     (only if both are set)

{"device":"my-bird","zone":"body","gesture":"hug","intensity":"firm","duration":12.35,"settled":true,"at":"2026-10-13T14:03:11Z","temp_c":27.41,"warmed_c":1.20,"humidity":48.50,"humid_delta":3.10}
```

| Field | Meaning |
|---|---|
| `device` | `DEVICE_NAME` |
| `zone` | always `"body"` |
| `gesture` | always `"hug"` |
| `intensity` | `"soft"` or `"firm"` (peak pressure at or above `FIRM_HPA`) |
| `duration` | seconds, two decimals |
| `settled` | true if held for `SETTLED_S` (8 s) or more |
| `at` | UTC ISO-8601 time the hug ended. Omitted if NTP has not synced yet |
| `temp_c` | temperature at the end (AHT20, or BMP280 if there is no AHT20) |
| `warmed_c` | temperature at end minus start |
| `humidity` | %RH at the end |
| `humid_delta` | humidity end minus start |

The four climate fields are left out if the sensor could not be read.

Any 2xx response counts as success. Anything else (or no WiFi) and the hug is
kept in a queue of the last ~20, and retried. **When replaying a backlog, the
body is a JSON list of such objects instead of a single object**, so a receiver
must accept both. `https://` URLs use TLS without verifying the certificate
(see the TODO in the sketch); `http://` is plain text, fine on your own LAN.

### Writing a receiver

`receiver/receiver.py` is a complete example in Python's standard library: it
checks the key, accepts a single object or a list, and appends one JSON line
per hug to a file.

```
RAVEN_KEY=change-me python3 receiver/receiver.py --port 8080 --out hugs.jsonl
```

with `TOUCH_URL "http://<your computer's LAN ip>:8080/touch"`, `TOUCH_AUTH_HEADER "X-Touch-Key"`
and `TOUCH_AUTH_VALUE "change-me"` in `secrets.h`. Test it without the bird:

```
curl -X POST localhost:8080/touch -H 'X-Touch-Key: change-me' -H 'Content-Type: application/json' \
  -d '{"device":"my-bird","zone":"body","gesture":"hug","intensity":"soft","duration":3.2,"settled":false}'
```

**If your memory server keeps a list, enum or check constraint of allowed touch
kinds, add `"hug"` (gesture) and `"body"` (zone) in BOTH the server code and
the database.** This broke us twice: first the code rejected it, then the
database did, and both look like the sensor failing.

## Lessons from building it

- **Epoxy, not hot glue, not super glue.** Hot glue held pressure but does not
  grip the slick sensor board, metal lid or PVC tube, and peeled when the tube
  was tugged while stuffing (it failed 7+ times). Super glue wicks into the
  pinhole and fogs the sensor. Use 5-minute two-part epoxy, a thin ring around
  the OUTSIDE of the tube base, away from the pinhole.
- **Glue the tube to the chip LAST, in place**, after everything else is
  arranged inside the plush, so nothing moves the joint afterwards. Check the
  pinhole is clear, then hands off until it has cured.
- The pinhole may sit near the tube wall. That is fine as long as it is inside.
- Strain relief: mount the board on a stiff card and tie the tube to the card a
  few cm back with a small slack loop. Stuff around the card, never pull on the tube.
- The AHT20 stays outside the tube, or it cannot measure the plush's air.
- The power bank may switch itself off when the draw is low and not restart on
  its own. Press its button after plugging in. Point its button and the ESP32's
  little buttons away from where a hug lands.
- Wrap cables and tube around the bulb so nothing tugs on the joint.
- Check the receiving end, not only the bird: `/log` shows `http: post ok` or
  the server's status code.

## License

MIT, see `LICENSE`.
