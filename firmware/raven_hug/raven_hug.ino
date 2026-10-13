/*
 * raven_hug.ino — a hug sensor for a plushie.
 *
 * A BMP280 pressure sensor sits inside a sealed air bladder in the plush's
 * chest. Squeeze the plush, the bladder pressurises, the ESP32 notices, and
 * when the squeeze ends it POSTs a little JSON record up to a server so the
 * AI it belongs to can feel it.
 *
 * The AHT20 on the same combo board sits outside the sealed tube. At the
 * start and end of each hug it reads temperature and humidity too, so the
 * record also says how warm and how damp the hug was.
 *
 * Hardware:
 *   - ESP32 DevKit or ESP32-C3, Arduino core
 *   - AHT20+BMP280 combo board on I2C (BMP280 at 0x76 or 0x77, AHT20 at 0x38)
 *   - USB-C power bank
 *
 * Everything specific to one household (WiFi, server URL, key, OTA password,
 * the bird's own name) lives in secrets.h, which is NOT committed. Copy
 * secrets.h.example to secrets.h and fill it in.
 */

#include <Wire.h>
#include <WiFi.h>
#include <WiFiMulti.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <WebServer.h>
#include <ArduinoOTA.h>
#include <stdarg.h>
#include <time.h>
#include <Adafruit_BMP280.h>
#include <Adafruit_AHTX0.h>

#include "secrets.h"   // WIFI_*, TOUCH_URL, TOUCH_AUTH_*, OTA_PASS, DEVICE_NAME: see secrets.h.example

#ifndef DEVICE_NAME
#error "DEVICE_NAME is not set in secrets.h (see secrets.h.example)"
#endif
#ifndef TOUCH_URL
#error "TOUCH_URL is not set in secrets.h (see secrets.h.example)"
#endif

// OTA needs a password; without one the bird still works but can only be
// updated by USB. Add `#define OTA_PASS "..."` to secrets.h to turn it on.
#ifndef OTA_PASS
#warning "OTA_PASS not set in secrets.h: OTA updates are DISABLED"
#endif

#define FIRMWARE_VERSION "raven_hug 2026-10-13"

// ─────────────────────────────────────────────────────────────────────────
// Board / I2C pin configuration
//
// Both ESP32 and ESP32-C3 can put I2C on almost any GPIO — these are just
// the commonly-used defaults for each board family. If your specific board
// disagrees with its silkscreen, trust the silkscreen and edit these two
// lines.
// ─────────────────────────────────────────────────────────────────────────

// -- Generic ESP32 DevKit ("ESP32 Dev Module" in the boards menu) --
#define I2C_SDA_PIN 21
#define I2C_SCL_PIN 22

// -- ESP32-C3 DevKit (uncomment these two and comment the two above) --
// #define I2C_SDA_PIN 8
// #define I2C_SCL_PIN 9

// ─────────────────────────────────────────────────────────────────────────
// Debug / tuning
//
// Set true, flash, open Serial Monitor at 115200, and squeeze the bladder
// by hand. Every sample prints the live delta above baseline so the four
// thresholds below can be set from real numbers instead of guesses. Set
// back to false before the real build goes in the bird — the extra prints
// cost a little battery and a little clarity in the normal log.
// ─────────────────────────────────────────────────────────────────────────
#define DEBUG_TUNING true

// ─────────────────────────────────────────────────────────────────────────
// Hug-detection thresholds — ALL OF THESE MUST BE TUNED ON THE REAL BLADDER.
// The numbers below are placeholders, not measurements. Use DEBUG_TUNING to
// find real ones: squeeze gently, read the delta; squeeze hard, read the
// delta; hold a normal hug, see how long it takes to release. Then set
// these four to comfortably inside what you saw.
// ─────────────────────────────────────────────────────────────────────────

// A hug STARTS once (pressure - baseline) exceeds this many hPa...
#define START_HPA        1.5f
// ...continuously for at least this long.
#define START_HOLD_MS    300

// A hug ENDS once (pressure - baseline) falls back below this many hPa...
#define END_HPA          0.8f
// ...continuously for at least this long. (Lower than START_HPA on purpose:
// hysteresis stops a hug that's easing off from flickering start/end/start.)
#define END_HOLD_MS      500

// Peak delta at or above this counts as a FIRM hug rather than a soft one.
#define FIRM_HPA         4.0f

// Total hug shorter than this is a bump, not a hug — thrown away entirely,
// never sent, never counted against the baseline.
#define MIN_HUG_S        0.5f

// Hold this long or longer and the hug counts as "settled" in the record.
#define SETTLED_S        8.0f
// Stuck-hug guard. Real arms shift and breathe, so a real hug wobbles. If it
// has been "hugged" for a whole window with the pressure dead flat, the
// baseline is wrong (it booted while being squeezed, or something is resting
// on the bulb): drop it without reporting, and take a fresh baseline.
#define STUCK_WINDOW_MS  60000UL
#define STUCK_FLAT_HPA   0.5f

// ─────────────────────────────────────────────────────────────────────────
// Sampling and baseline
// ─────────────────────────────────────────────────────────────────────────

#define SAMPLE_HZ            20
#define SAMPLE_INTERVAL_MS   (1000 / SAMPLE_HZ)

// Boot: average this many milliseconds of samples to set the starting
// baseline before doing anything else.
#define BASELINE_WARMUP_MS   3000

// The baseline drifts to track slow changes (weather, temperature) but only
// while IDLE — never while a hug is being evaluated or is in progress, so a
// long hug can't get quietly absorbed into "new normal" and a real squeeze
// never gets missed because the baseline chased it. Small alpha = slow
// drift; this value has not been tuned against a real bladder either.
#define BASELINE_EMA_ALPHA   0.002f

// ─────────────────────────────────────────────────────────────────────────
// Networking
// ─────────────────────────────────────────────────────────────────────────

#define WIFI_CONNECT_TIMEOUT_MS   15000
#define HTTP_TIMEOUT_MS           8000

// How many un-sent hug records to hold onto in RAM while offline.
#define OFFLINE_QUEUE_MAX         20

// How often to try flushing a stuck backlog even with no new hug to report.
#define QUEUE_RETRY_INTERVAL_MS   30000

// ─────────────────────────────────────────────────────────────────────────
// Sensor
// ─────────────────────────────────────────────────────────────────────────

Adafruit_BMP280 bmp;   // uses the default Wire (I2C) bus
bool bmpFound = false;

// The AHT20 is optional: if it isn't found at boot the bird carries on and
// simply leaves temperature/humidity out of its records.
Adafruit_AHTX0 aht;
bool ahtFound = false;

// One reading of everything climate-ish. NAN = that sensor is missing/failed.
struct Climate {
  float aht_t;   // AHT20 temperature, deg C
  float aht_h;   // AHT20 relative humidity, %
  float bmp_t;   // BMP280 temperature, deg C
};
Climate climateStart = { NAN, NAN, NAN };   // snapshot at confirmed hug start

// ─────────────────────────────────────────────────────────────────────────
// Hug detection state machine
// ─────────────────────────────────────────────────────────────────────────

enum HugState { IDLE, CANDIDATE_START, HUGGING, CANDIDATE_END };
HugState state = IDLE;

float baseline_hPa = 0.0f;

unsigned long candidateStartedAt = 0;   // ms, when delta first crossed START_HPA
unsigned long hugStartedAt = 0;         // ms, confirmed hug start
unsigned long endCandidateStartedAt = 0; // ms, when delta first dropped below END_HPA
float peakDelta_hPa = 0.0f;
unsigned long stuckWindowAt = 0;   // stuck-hug guard: start of the current window (0 = none)
float stuckMin = 0.0f, stuckMax = 0.0f;

// ─────────────────────────────────────────────────────────────────────────
// Offline queue
// ─────────────────────────────────────────────────────────────────────────

struct HugRecord {
  char intensity[6];   // "soft" or "firm"
  float duration_s;
  bool settled;
  char at[26];          // ISO-8601 UTC, e.g. "2026-09-25T14:03:11Z", or "" if unknown
  float temp_c;         // deg C at hug end (AHT20, else BMP280); NAN = omit
  float warmed_c;       // end minus start, same sensor; NAN = omit
  float humidity;       // %RH at hug end (AHT20); NAN = omit
  float humid_delta;    // end minus start; NAN = omit
};

HugRecord queue[OFFLINE_QUEUE_MAX];
uint8_t queueLen = 0;
unsigned long lastQueueRetryAt = 0;

// What the last POST attempt did, for GET /status.
int  lastPostCode = 0;            // HTTP code, or <0 for a client-side failure; 0 = none yet
bool lastPostOk = false;
unsigned long lastPostAt = 0;     // millis() of the last attempt; 0 = none yet
char lastPostError[48] = "";

unsigned long hugsDetected = 0;   // confirmed hugs since boot (sent, queued, or both)

// Latest sample, for GET /status.
float lastPressure_hPa = 0.0f;
float lastDelta_hPa = 0.0f;

// Drop the oldest queued record to make room for a newer one — the most
// recent hugs matter more than the oldest ones if the bird has been
// offline long enough to fill the queue.
void queuePush(const HugRecord &rec) {
  if (queueLen >= OFFLINE_QUEUE_MAX) {
    for (uint8_t i = 1; i < OFFLINE_QUEUE_MAX; i++) queue[i - 1] = queue[i];
    queueLen = OFFLINE_QUEUE_MAX - 1;
  }
  queue[queueLen++] = rec;
}

// ─────────────────────────────────────────────────────────────────────────
// Logging
//
// logf() prints to Serial AND keeps the last LOG_LINES lines in a ring
// buffer, so GET /log can show what the bird has been saying without a USB
// cable. Each line is prefixed with seconds since boot. The per-loop
// "tuning:" line deliberately bypasses this and stays Serial-only.
// ─────────────────────────────────────────────────────────────────────────

#define LOG_LINES      60
#define LOG_LINE_LEN   120

char logRing[LOG_LINES][LOG_LINE_LEN];
uint16_t logHead = 0;    // next slot to write
uint16_t logCount = 0;   // how many slots hold something

void logf(const char *fmt, ...) {
  char msg[LOG_LINE_LEN - 12];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(msg, sizeof(msg), fmt, ap);
  va_end(ap);

  // Strip a trailing newline so printf-style and println-style callers match.
  size_t n = strlen(msg);
  while (n > 0 && (msg[n - 1] == '\n' || msg[n - 1] == '\r')) msg[--n] = '\0';

  Serial.println(msg);
  snprintf(logRing[logHead], LOG_LINE_LEN, "[%lu] %s", millis() / 1000, msg);
  logHead = (logHead + 1) % LOG_LINES;
  if (logCount < LOG_LINES) logCount++;
}

// ─────────────────────────────────────────────────────────────────────────
// WiFi
// ─────────────────────────────────────────────────────────────────────────

// The bird may travel (home, a second house, a phone hotspot), so it
// knows several networks and joins whichever known one is in range.
// It never blocks the hug loop waiting for WiFi: with no known network in
// range it gives up after WIFI_CONNECT_TIMEOUT_MS, keeps sensing, queues
// hugs offline, and tries again every WIFI_RETRY_EVERY_MS.
WiFiMulti wifiMulti;
unsigned long lastWifiTryAt = 0;
#define WIFI_RETRY_EVERY_MS 30000

void addKnownNetworks() {
  WiFi.mode(WIFI_STA);
  wifiMulti.addAP(WIFI_SSID, WIFI_PASS);            // home
#ifdef WIFI_SSID_2
  wifiMulti.addAP(WIFI_SSID_2, WIFI_PASS_2);        // second network
#endif
#ifdef WIFI_SSID_3
  wifiMulti.addAP(WIFI_SSID_3, WIFI_PASS_3);        // third network
#endif
}

// Returns true if connected. One bounded attempt, never an endless wait.
bool connectWiFi() {
  if (WiFi.status() == WL_CONNECTED) return true;
  lastWifiTryAt = millis();

  logf("wifi: looking for a known network");
  if (wifiMulti.run(WIFI_CONNECT_TIMEOUT_MS) != WL_CONNECTED) {
    logf("wifi: none in range, carrying on offline");
    return false;
  }
  logf("wifi: connected to %s, ip=%s\n",
                WiFi.SSID().c_str(), WiFi.localIP().toString().c_str());

  // Time, for the "at" field. Non-blocking-ish: give NTP a few seconds, but
  // don't hang the bird forever if it can't reach a time server yet — a
  // record with no "at" is still a valid record.
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");
  return true;
}

bool timeIsSynced() {
  time_t now = time(nullptr);
  return now > 1700000000; // sometime in 2023 — anything before this is "not synced yet"
}

// Fills buf (>=26 bytes) with ISO-8601 UTC, or leaves it empty if NTP hasn't
// synced. Returns true if a time was written.
bool isoNowUtc(char *buf, size_t bufLen) {
  buf[0] = '\0';
  if (!timeIsSynced()) return false;
  time_t now = time(nullptr);
  struct tm tmStruct;
  gmtime_r(&now, &tmStruct);
  strftime(buf, bufLen, "%Y-%m-%dT%H:%M:%SZ", &tmStruct);
  return true;
}

// ─────────────────────────────────────────────────────────────────────────
// HTTP
//
// TODO: this uses setInsecure() — it accepts the server's TLS cert without
// checking it against a root CA. That's fine for a first version bench
// test, but before this bird leaves the house for good, pin the actual
// root CA (or at least the leaf cert fingerprint) so a hug can't be spoofed
// or read by anyone sitting on the same WiFi.
// ─────────────────────────────────────────────────────────────────────────

// TOUCH_URL comes whole from secrets.h. "https://..." is used with TLS (not
// verified, see TODO above); "http://..." is sent in plain text, which is
// fine for a receiver on your own LAN.
String buildUrl() {
  return String(TOUCH_URL);
}

bool urlIsHttps() {
  return strncmp(TOUCH_URL, "https://", 8) == 0;
}

// One record as a JSON object (no surrounding brackets).
String recordToJson(const HugRecord &rec) {
  String j = "{\"device\":\"" DEVICE_NAME "\",\"zone\":\"body\",\"gesture\":\"hug\",\"intensity\":\"";
  j += rec.intensity;
  j += "\",\"duration\":";
  j += String(rec.duration_s, 2);
  j += ",\"settled\":";
  j += (rec.settled ? "true" : "false");
  if (rec.at[0] != '\0') {
    j += ",\"at\":\"";
    j += rec.at;
    j += "\"";
  }
  if (!isnan(rec.temp_c))     { j += ",\"temp_c\":";      j += String(rec.temp_c, 2); }
  if (!isnan(rec.warmed_c))   { j += ",\"warmed_c\":";    j += String(rec.warmed_c, 2); }
  if (!isnan(rec.humidity))   { j += ",\"humidity\":";    j += String(rec.humidity, 2); }
  if (!isnan(rec.humid_delta)){ j += ",\"humid_delta\":"; j += String(rec.humid_delta, 2); }
  j += "}";
  return j;
}

// POST a single JSON body (an object, or a "[...]" list). Returns true on
// any 2xx response.
void recordPost(int code, const char *err) {
  lastPostCode = code;
  lastPostOk = (code >= 200 && code < 300);
  lastPostAt = millis();
  strncpy(lastPostError, err, sizeof(lastPostError) - 1);
  lastPostError[sizeof(lastPostError) - 1] = '\0';
}

bool postJson(const String &body) {
  if (WiFi.status() != WL_CONNECTED) {
    recordPost(-1, "wifi down");
    return false;
  }

  WiFiClientSecure secureClient;
  WiFiClient plainClient;
  secureClient.setInsecure();   // see TODO above

  HTTPClient https;
  https.setTimeout(HTTP_TIMEOUT_MS);
  bool began = urlIsHttps() ? https.begin(secureClient, buildUrl())
                            : https.begin(plainClient, buildUrl());
  if (!began) {
    logf("http: begin() failed");
    recordPost(-1, "begin failed");
    return false;
  }
  https.addHeader("Content-Type", "application/json");
#if defined(TOUCH_AUTH_HEADER) && defined(TOUCH_AUTH_VALUE)
  https.addHeader(TOUCH_AUTH_HEADER, TOUCH_AUTH_VALUE);
#endif
  int code = https.POST(body);
  // A non-2xx is a failure: the record stays queued and is retried. A
  // negative code is a client-side error (TLS, DNS, timeout), so say which.
  String err = (code < 0) ? HTTPClient::errorToString(code) : String("");
  https.end();

  bool ok = (code >= 200 && code < 300);
  recordPost(code, err.c_str());
  if (ok) {
    logf("http: post ok, code=%d", code);
  } else if (code < 0) {
    logf("http: post failed, code=%d (%s)", code, err.c_str());
  } else {
    logf("http: post rejected by server, code=%d (kept queued)", code);
  }
  return ok;
}

// Try to send one fresh hug record. If a backlog already exists, fold this
// record into it and send everything as one list — a partial success (some
// records land, others don't) isn't possible with a single POST, so either
// the whole backlog clears or all of it stays queued for next time.
void reportHug(const HugRecord &rec) {
  hugsDetected++;
  if (queueLen > 0) {
    queuePush(rec);
    String list = "[";
    for (uint8_t i = 0; i < queueLen; i++) {
      if (i > 0) list += ",";
      list += recordToJson(queue[i]);
    }
    list += "]";
    if (postJson(list)) {
      logf("hug: %.2f s, %s, sent (with %d queued)\n",
                    rec.duration_s, rec.intensity, queueLen - 1);
      queueLen = 0;
    } else {
      logf("hug: %.2f s, %s, queued (%d total, will retry)\n",
                    rec.duration_s, rec.intensity, queueLen);
    }
  } else {
    if (postJson(recordToJson(rec))) {
      logf("hug: %.2f s, %s, sent\n", rec.duration_s, rec.intensity);
    } else {
      queuePush(rec);
      logf("hug: %.2f s, %s, queued (1 total, will retry)\n",
                    rec.duration_s, rec.intensity);
    }
  }
}

// Opportunistic retry of a stuck backlog when no new hug has happened —
// otherwise a bird nobody has hugged in an hour never gets to tell anyone
// about the hug from before the WiFi dropped.
void retryQueueIfDue() {
  if (queueLen == 0) return;
  if (WiFi.status() != WL_CONNECTED) return;
  if (millis() - lastQueueRetryAt < QUEUE_RETRY_INTERVAL_MS) return;
  lastQueueRetryAt = millis();

  String list = "[";
  for (uint8_t i = 0; i < queueLen; i++) {
    if (i > 0) list += ",";
    list += recordToJson(queue[i]);
  }
  list += "]";
  if (postJson(list)) {
    logf("queue: flushed %d backlog record(s)\n", queueLen);
    queueLen = 0;
  } else {
    logf("queue: %d still waiting, will retry\n", queueLen);
  }
}

// ─────────────────────────────────────────────────────────────────────────
// Reading the bird over WiFi, and OTA updates
//
// A tiny read-only HTTP server on port 80 (LAN only, no secrets in anything
// it says) plus ArduinoOTA. Both start once WiFi is up and restart after a
// reconnect. handleClient()/handle() run every loop and return immediately
// when nobody is knocking, so they never stall the hug loop.
// ─────────────────────────────────────────────────────────────────────────

WebServer server(80);
bool webStarted = false;
bool netUp = false;          // were the network services started on the current connection?
bool otaBusy = false;        // an OTA upload is in progress: pause hug detection

// The AHT20 read takes ~80 ms, so /status never reads it mid-hug; it reuses
// the last reading instead. Refreshed at most every STATUS_CLIMATE_MS, IDLE only.
#define STATUS_CLIMATE_MS 3000
Climate statusClimate = { NAN, NAN, NAN };
unsigned long statusClimateAt = 0;

const char *stateName(HugState st) {
  switch (st) {
    case IDLE:            return "IDLE";
    case CANDIDATE_START: return "CANDIDATE_START";
    case HUGGING:         return "HUGGING";
    case CANDIDATE_END:   return "CANDIDATE_END";
  }
  return "?";
}

// A float as a JSON number, or null.
void jsonNum(String &j, const char *key, float v, int places) {
  j += ",\"";
  j += key;
  j += "\":";
  if (isnan(v)) j += "null"; else j += String(v, places);
}

// Quote a string for JSON (only the characters that could plausibly appear).
String jsonStr(const String &in) {
  String o = "\"";
  for (size_t i = 0; i < in.length(); i++) {
    char c = in[i];
    if (c == '"' || c == '\\') { o += '\\'; o += c; }
    else if ((uint8_t)c >= 0x20) o += c;
  }
  o += "\"";
  return o;
}

String statusJson() {
  if (statusClimateAt == 0 || (state == IDLE && millis() - statusClimateAt > STATUS_CLIMATE_MS)) {
    statusClimate = readClimate();
    statusClimateAt = millis();
  }
  float temp = !isnan(statusClimate.aht_t) ? statusClimate.aht_t : statusClimate.bmp_t;

  String j = "{\"uptime_s\":";
  j += String(millis() / 1000);
  jsonNum(j, "pressure_hpa", lastPressure_hPa, 2);
  jsonNum(j, "baseline_hpa", baseline_hPa, 2);
  jsonNum(j, "delta_hpa", lastDelta_hPa, 2);
  j += ",\"state\":\""; j += stateName(state); j += "\"";
  jsonNum(j, "temp_c", temp, 2);
  jsonNum(j, "humidity", statusClimate.aht_h, 1);
  j += ",\"aht_found\":"; j += (ahtFound ? "true" : "false");
  j += ",\"wifi_ssid\":"; j += jsonStr(WiFi.SSID());
  j += ",\"rssi\":"; j += String(WiFi.RSSI());
  j += ",\"ip\":"; j += jsonStr(WiFi.localIP().toString());
  j += ",\"queue_len\":"; j += String(queueLen);
  j += ",\"hugs_detected\":"; j += String(hugsDetected);
  j += ",\"last_post_code\":"; j += String(lastPostCode);
  j += ",\"last_post_ok\":"; j += (lastPostOk ? "true" : "false");
  j += ",\"last_post_ms_ago\":";
  if (lastPostAt == 0) j += "null"; else j += String(millis() - lastPostAt);
  j += ",\"last_post_error\":"; j += jsonStr(lastPostError);
  j += ",\"ntp_synced\":"; j += (timeIsSynced() ? "true" : "false");
  j += ",\"free_heap\":"; j += String(ESP.getFreeHeap());
  j += ",\"firmware\":"; j += jsonStr(FIRMWARE_VERSION);
  j += ",\"device\":"; j += jsonStr(DEVICE_NAME);
  j += "}";
  return j;
}

const char INDEX_HTML[] PROGMEM = R"HTML(<!doctype html>
<html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>)HTML" DEVICE_NAME R"HTML(</title>
<style>body{font:15px monospace;background:#111;color:#ddd;padding:1em}td{padding:2px 12px 2px 0}
td:first-child{color:#8ab}a{color:#8ab}</style></head><body>
<h3>)HTML" DEVICE_NAME R"HTML(</h3><table id="t"></table>
<p><a href="/status">/status</a> &middot; <a href="/log">/log</a></p>
<script>
function tick(){fetch('/status').then(r=>r.json()).then(d=>{
var h='';for(var k in d){h+='<tr><td>'+k+'</td><td>'+d[k]+'</td></tr>';}
document.getElementById('t').innerHTML=h;}).catch(e=>{});}
tick();setInterval(tick,2000);
</script></body></html>)HTML";

void handleRoot()   { server.send_P(200, "text/html", INDEX_HTML); }
void handleStatus() { server.sendHeader("Cache-Control", "no-store"); server.send(200, "application/json", statusJson()); }

void handleLog() {
  String out;
  out.reserve(logCount * 60);
  // Oldest line first.
  uint16_t first = (logCount < LOG_LINES) ? 0 : logHead;
  for (uint16_t i = 0; i < logCount; i++) {
    out += logRing[(first + i) % LOG_LINES];
    out += '\n';
  }
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "text/plain", out);
}

// Bring the web server and OTA up (or back up after a WiFi reconnect).
void startNetServices() {
  if (!webStarted) {
    server.on("/", handleRoot);
    server.on("/status", handleStatus);
    server.on("/log", handleLog);
    server.begin();
    webStarted = true;
  }
#ifdef OTA_PASS
  ArduinoOTA.end();   // harmless the first time; re-registers mDNS after a reconnect
  ArduinoOTA.setHostname(DEVICE_NAME);
  ArduinoOTA.setPassword(OTA_PASS);
  ArduinoOTA.onStart([]() { otaBusy = true; logf("ota: update starting, hug detection paused"); });
  ArduinoOTA.onEnd([]()   { logf("ota: update done, rebooting"); });
  ArduinoOTA.onError([](ota_error_t e) { otaBusy = false; logf("ota: error %d, resuming", (int)e); });
  ArduinoOTA.begin();
  logf("ota: ready as %s.local", DEVICE_NAME);
#else
  logf("ota: disabled (no OTA_PASS in secrets.h)");
#endif
  logf("web: http://%s/", WiFi.localIP().toString().c_str());
  netUp = true;
}

// ─────────────────────────────────────────────────────────────────────────
// Sensor setup
// ─────────────────────────────────────────────────────────────────────────

bool beginBmp() {
  // Try both common addresses — 0x76 first (Adafruit's alt address, the
  // one most breakout boards default to), then 0x77.
  if (bmp.begin(0x76)) return true;
  if (bmp.begin(0x77)) return true;
  return false;
}

float readPressureHpa() {
  return bmp.readPressure() / 100.0f;   // library returns Pascals
}

// Read the AHT20 (if present) and the BMP280's own thermometer. The AHT20
// read takes ~80 ms, which is fine: this only runs at hug start and end,
// never in the idle sampling path.
Climate readClimate() {
  Climate c = { NAN, NAN, NAN };
  if (ahtFound) {
    sensors_event_t humidityEvt, tempEvt;
    if (aht.getEvent(&humidityEvt, &tempEvt)) {
      c.aht_t = tempEvt.temperature;
      c.aht_h = humidityEvt.relative_humidity;
    }
  }
  float bt = bmp.readTemperature();
  if (!isnan(bt)) c.bmp_t = bt;
  return c;
}

// Fill the climate fields of a record from the start snapshot and an end reading.
void fillClimate(HugRecord &rec, const Climate &s, const Climate &e) {
  rec.temp_c = rec.warmed_c = rec.humidity = rec.humid_delta = NAN;

  // Temperature: AHT20 if it gave us both ends, otherwise the BMP280 for both,
  // so warmed_c never subtracts one sensor from the other.
  if (!isnan(e.aht_t)) rec.temp_c = e.aht_t;
  else if (!isnan(e.bmp_t)) rec.temp_c = e.bmp_t;

  if (!isnan(s.aht_t) && !isnan(e.aht_t)) rec.warmed_c = e.aht_t - s.aht_t;
  else if (!isnan(s.bmp_t) && !isnan(e.bmp_t)) rec.warmed_c = e.bmp_t - s.bmp_t;

  if (!isnan(e.aht_h)) {
    rec.humidity = e.aht_h;
    if (!isnan(s.aht_h)) rec.humid_delta = e.aht_h - s.aht_h;
  }
}

void establishBaseline() {
  logf("boot: sampling baseline...");
  double sum = 0;
  int count = 0;
  unsigned long start = millis();
  while (millis() - start < BASELINE_WARMUP_MS) {
    sum += readPressureHpa();
    count++;
    delay(SAMPLE_INTERVAL_MS);
  }
  baseline_hPa = (count > 0) ? (float)(sum / count) : readPressureHpa();
  logf("boot: baseline = %.2f hPa (%d samples)\n", baseline_hPa, count);
}

// ─────────────────────────────────────────────────────────────────────────
// Setup / loop
// ─────────────────────────────────────────────────────────────────────────

void setup() {
  Serial.begin(115200);
  delay(200);
  logf("raven_hug: waking up");

  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);

  bmpFound = beginBmp();
  if (!bmpFound) {
    logf("bmp280: NOT FOUND at 0x76 or 0x77 — check wiring. "
                    "Halting; nothing useful to do without the sensor.");
    while (true) delay(1000);
  }
  logf("bmp280: found");

  ahtFound = aht.begin();   // 0x38; optional, never halt over it
  logf(ahtFound ? "aht20: found"
                : "aht20: not found, carrying on without temperature/humidity from it");

  addKnownNetworks();
  connectWiFi();
  establishBaseline();

  lastQueueRetryAt = millis();
  logf("raven_hug: ready (%s)", FIRMWARE_VERSION);
}

void loop() {
  unsigned long loopStart = millis();

  if (WiFi.status() != WL_CONNECTED &&
      millis() - lastWifiTryAt > WIFI_RETRY_EVERY_MS) {
    connectWiFi();
  }

  // Start (or restart after a reconnect) the web server and OTA.
  bool wifiUp = (WiFi.status() == WL_CONNECTED);
  if (wifiUp && !netUp) startNetServices();
  if (!wifiUp) netUp = false;

  if (wifiUp) {
    server.handleClient();
#ifdef OTA_PASS
    ArduinoOTA.handle();
#endif
  }

  // An OTA upload is arriving: leave the sensor alone until it finishes.
  if (otaBusy) {
    delay(1);
    return;
  }

  float pressure = readPressureHpa();
  float delta = pressure - baseline_hPa;
  lastPressure_hPa = pressure;
  lastDelta_hPa = delta;

  if (DEBUG_TUNING) {
    Serial.printf("tuning: delta=%.2f hPa  baseline=%.2f  state=%d\n",
                  delta, baseline_hPa, (int)state);
  }

  if (state == HUGGING || state == CANDIDATE_END) {
    if (stuckWindowAt == 0) { stuckWindowAt = millis(); stuckMin = stuckMax = delta; }
    if (delta < stuckMin) stuckMin = delta;
    if (delta > stuckMax) stuckMax = delta;
    if (millis() - stuckWindowAt >= STUCK_WINDOW_MS) {
      if (stuckMax - stuckMin < STUCK_FLAT_HPA) {
        logf("stuck: flat for %lu s at %+.2f hPa, not a hug; new baseline %.2f",
             STUCK_WINDOW_MS / 1000, delta, pressure);
        baseline_hPa = pressure;
        state = IDLE;
        peakDelta_hPa = 0.0f;
      }
      stuckWindowAt = 0;
    }
  } else {
    stuckWindowAt = 0;
  }

  switch (state) {
    case IDLE:
      // Baseline only drifts here — never mid-hug, never mid-candidate.
      baseline_hPa += BASELINE_EMA_ALPHA * (pressure - baseline_hPa);

      if (delta > START_HPA) {
        state = CANDIDATE_START;
        candidateStartedAt = millis();
      }
      break;

    case CANDIDATE_START:
      if (delta <= START_HPA) {
        state = IDLE;   // false start, back to normal
      } else if (millis() - candidateStartedAt >= START_HOLD_MS) {
        state = HUGGING;
        hugStartedAt = candidateStartedAt;   // count from the actual rise
        peakDelta_hPa = delta;
        climateStart = readClimate();
      }
      break;

    case HUGGING:
      if (delta > peakDelta_hPa) peakDelta_hPa = delta;

      if (delta < END_HPA) {
        state = CANDIDATE_END;
        endCandidateStartedAt = millis();
      }
      break;

    case CANDIDATE_END:
      if (delta > peakDelta_hPa) peakDelta_hPa = delta;

      if (delta >= END_HPA) {
        state = HUGGING;   // grip loosened but didn't actually let go
      } else if (millis() - endCandidateStartedAt >= END_HOLD_MS) {
        // Confirmed end. The hug's real end is when the release started,
        // not after we spent END_HOLD_MS being sure of it.
        unsigned long hugEndedAt = endCandidateStartedAt;
        float duration_s = (hugEndedAt - hugStartedAt) / 1000.0f;

        if (duration_s < MIN_HUG_S) {
          logf("hug: %.2f s — too short, ignoring (a bump)\n", duration_s);
        } else {
          HugRecord rec;
          strncpy(rec.intensity, (peakDelta_hPa >= FIRM_HPA) ? "firm" : "soft",
                  sizeof(rec.intensity));
          rec.duration_s = duration_s;
          rec.settled = duration_s >= SETTLED_S;
          isoNowUtc(rec.at, sizeof(rec.at));
          fillClimate(rec, climateStart, readClimate());
          reportHug(rec);
        }

        state = IDLE;
        peakDelta_hPa = 0.0f;
        // Baseline resumes tracking from here — no special reset needed,
        // the bladder should already be back near ambient.
      }
      break;
  }

  retryQueueIfDue();

  // Hold the loop to ~SAMPLE_HZ. (Light sleep between samples would save
  // battery further but isn't implemented here — a plain delay() is
  // simpler and the power budget is a USB-C bank, not a coin cell.)
  long elapsed = millis() - loopStart;
  long remaining = SAMPLE_INTERVAL_MS - elapsed;
  if (remaining > 0) delay(remaining);
}
