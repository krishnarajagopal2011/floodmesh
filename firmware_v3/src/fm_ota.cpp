/**
 * FloodMesh V3 - WiFi update mode. See fm_ota.h.
 */
#include "fm_ota.h"

#include <HTTPUpdateServer.h>
#include <Preferences.h>
#include <Update.h>
#include <WebServer.h>
#include <WiFi.h>
#include <esp_ota_ops.h>
#include <esp_system.h>

#include "fm_ids.h"

#ifndef FM_NVS_KEY_WIFI_SSID
#define FM_NVS_KEY_WIFI_SSID "wifissid"
#endif
#ifndef FM_NVS_KEY_WIFI_PASS
#define FM_NVS_KEY_WIFI_PASS "wifipass"
#endif

namespace {

enum State : uint8_t { ST_IDLE, ST_JOINING, ST_READY };

// Set while a window is open. It survives the reboot at the end of an update
// (RTC memory is kept across a software reset) and is garbage after power-on,
// which is why fmOtaResumePending() also checks the reset reason.
constexpr uint32_t kResumeMagic = 0x4F544152UL;   // "OTAR"
RTC_NOINIT_ATTR uint32_t g_resume;

FmProvDraw g_draw = nullptr;
State g_state = ST_IDLE;
uint32_t g_openedAt = 0, g_windowMs = 0, g_joinStart = 0;
volatile bool g_closeReq = false;
bool g_serverSetup = false;
size_t g_lastDrawKb = 0;

char g_ssid[33] = "";
bool g_hasPass = false;
char g_host[16] = "";
char g_ip[16] = "";
char g_image[9] = "";

WebServer g_server(80);
HTTPUpdateServer g_updater(false);   // false: it must not echo anything to serial

/**
 * Milliseconds from `since` to `now`, signed. loop() reads `now` before
 * pollSerial() can open the window, so `since` may be a few ms AFTER `now`; an
 * unsigned difference would wrap to ~49 days and close the window at once.
 */
int32_t elapsed(uint32_t now, uint32_t since) { return (int32_t)(now - since); }

/** Call sign without its pad spaces. */
void trimmedCallSign(char *out, size_t n) {
  snprintf(out, n, "%s", fmCallSign());
  for (size_t i = strlen(out); i > 0 && out[i - 1] == ' '; i--) out[i - 1] = '\0';
}

void makeHost() {
  char cs[8];
  trimmedCallSign(cs, sizeof(cs));
  snprintf(g_host, sizeof(g_host), "fm-%s", cs);
  for (char *p = g_host; *p; p++) *p = (char)tolower((unsigned char)*p);
}

bool printable(const char *s) {
  for (; *s; s++) {
    if (*s < 0x20 || *s > 0x7E) return false;
  }
  return true;
}

void setupServerOnce() {
  if (g_serverSetup) return;
  g_serverSetup = true;

  g_server.on("/id", HTTP_GET, []() {
    char cs[8], b[112];
    trimmedCallSign(cs, sizeof(cs));
    snprintf(b, sizeof(b), "FloodMesh %s %s %s %s up=%lu\n", cs, FLOODMESH_VERSION, g_image,
             WiFi.macAddress().c_str(), (unsigned long)(millis() / 1000));
    g_server.send(200, "text/plain", b);
  });

#ifdef FM_OTA_PASSWORD
  g_server.on("/close", HTTP_GET, []() {
    if (!g_server.authenticate("fm", FM_OTA_PASSWORD)) return g_server.requestAuthentication();
    g_server.send(200, "text/plain", "closing\n");
    g_closeReq = true;
  });
  g_updater.setup(&g_server, "/update", "fm", FM_OTA_PASSWORD);
#endif

  // HTTPUpdateServer starts Update with the whole free slot as its size, so a
  // percentage would be meaningless: show kilobytes received instead.
  Update.onProgress([](size_t done, size_t) {
    const size_t kb = done / 1024;
    if (done == 0) Serial.println("[OTA] receiving an update - do not power off");
    if (g_draw && (done == 0 || kb >= g_lastDrawKb + 64)) {
      g_lastDrawKb = kb;
      char l3[22];
      snprintf(l3, sizeof(l3), "%u KB received", (unsigned)kb);
      g_draw("UPDATING", g_host, l3, "do not power off");
    }
  });
}

void shutdown(const char *why) {
  if (g_state == ST_READY) g_server.stop();
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  g_state = ST_IDLE;
  g_ip[0] = '\0';
  g_resume = 0;
  if (why) Serial.printf("[OTA] window closed (%s), WiFi off\n", why);
}

}  // namespace

// ---------------------------------------------------------------- API
void fmOtaBegin(FmProvDraw draw) {
  g_draw = draw;
  makeHost();   // again in fmOtaStart(): the call sign can change in between
  const esp_app_desc_t *d = esp_ota_get_app_description();
  snprintf(g_image, sizeof(g_image), "%02x%02x%02x%02x", d->app_elf_sha256[0],
           d->app_elf_sha256[1], d->app_elf_sha256[2], d->app_elf_sha256[3]);

  Preferences p;
  if (p.begin(FM_NVS_NAMESPACE, true)) {
    p.getString(FM_NVS_KEY_WIFI_SSID, g_ssid, sizeof(g_ssid));
    g_hasPass = p.isKey(FM_NVS_KEY_WIFI_PASS);
    p.end();
  }
  Serial.printf("[OTA] image %s, WiFi network %s%s%s\n", g_image, g_ssid[0] ? "'" : "",
                g_ssid[0] ? g_ssid : "not set", g_ssid[0] ? "'" : "");
}

bool fmOtaResumePending() {
  const bool r = (g_resume == kResumeMagic) && esp_reset_reason() == ESP_RST_SW;
  g_resume = 0;
  return r;
}

bool fmOtaSetSsid(const char *ssid) {
  const size_t n = ssid ? strlen(ssid) : 0;
  if (n < 1 || n > 32 || !printable(ssid)) return false;
  Preferences p;
  if (!p.begin(FM_NVS_NAMESPACE, false)) return false;
  const bool ok = p.putString(FM_NVS_KEY_WIFI_SSID, ssid) == n;
  p.end();
  if (ok) snprintf(g_ssid, sizeof(g_ssid), "%s", ssid);
  return ok;
}

bool fmOtaSetPass(const char *pass) {
  const size_t n = pass ? strlen(pass) : 0;
  if ((n != 0 && (n < 8 || n > 63)) || !printable(pass ? pass : "")) return false;
  Preferences p;
  if (!p.begin(FM_NVS_NAMESPACE, false)) return false;
  // putString() returns 0 for an empty string, so an open network is stored
  // as a present-but-empty key and checked with isKey().
  const bool ok = p.putString(FM_NVS_KEY_WIFI_PASS, pass) == n && p.isKey(FM_NVS_KEY_WIFI_PASS);
  p.end();
  if (ok) g_hasPass = true;
  return ok;
}

void fmOtaForget() {
  Preferences p;
  if (p.begin(FM_NVS_NAMESPACE, false)) {
    p.remove(FM_NVS_KEY_WIFI_SSID);
    p.remove(FM_NVS_KEY_WIFI_PASS);
    p.end();
  }
  g_ssid[0] = '\0';
  g_hasPass = false;
}

bool fmOtaHasSsid() { return g_ssid[0] != '\0'; }
const char *fmOtaSsid() { return g_ssid; }
bool fmOtaHasPass() { return g_hasPass; }

bool fmOtaStart(uint32_t ms) {
#ifndef FM_OTA_PASSWORD
  (void)ms;
  Serial.println("[OTA] refused - this build has no FM_OTA_PASSWORD (set it at build "
                 "time, see README section 7)");
  return false;
#else
  if (!g_ssid[0] || !g_hasPass) {
    Serial.println("[OTA] refused - no WiFi network stored. Send "
                   "'wifi ssid <name>' then 'wifi pass <password>'");
    return false;
  }
  g_openedAt = millis();
  g_windowMs = ms;
  g_resume = kResumeMagic;
  if (g_state != ST_IDLE) {
    Serial.printf("[OTA] window extended to %lu min\n", (unsigned long)(ms / 60000UL));
    return true;
  }

  char pass[64] = "";
  Preferences p;
  if (p.begin(FM_NVS_NAMESPACE, true)) {
    p.getString(FM_NVS_KEY_WIFI_PASS, pass, sizeof(pass));
    p.end();
  }
  makeHost();
  WiFi.persistent(false);          // the credentials live in our NVS keys only
  WiFi.setHostname(g_host);        // before mode(): the 2.0.x core applies it at STA start
  WiFi.mode(WIFI_STA);
  WiFi.begin(g_ssid, pass[0] ? pass : nullptr);
  memset(pass, 0, sizeof(pass));

  g_state = ST_JOINING;
  g_joinStart = g_openedAt;
  g_lastDrawKb = 0;
  Serial.printf("[OTA] window open %lu min, joining '%s' as %s\n",
                (unsigned long)(ms / 60000UL), g_ssid, g_host);
  return true;
#endif
}

void fmOtaStop() {
  if (g_state != ST_IDLE) g_closeReq = true;
}

bool fmOtaActive() { return g_state != ST_IDLE; }

FmOtaEvent fmOtaLoop(uint32_t now) {
  if (g_state == ST_IDLE) return FM_OTA_EV_NONE;
  if (g_closeReq) {
    g_closeReq = false;
    shutdown("closed on request");
    return FM_OTA_EV_CLOSED;
  }

  if (g_state == ST_JOINING) {
    if (WiFi.status() == WL_CONNECTED) {
      snprintf(g_ip, sizeof(g_ip), "%s", WiFi.localIP().toString().c_str());
      setupServerOnce();
      g_server.begin();
      g_state = ST_READY;
      Serial.printf("[OTA] ready at http://%s/ (%s, image %s)\n", g_ip, g_host, g_image);
      return FM_OTA_EV_JOINED;
    }
    if (elapsed(now, g_joinStart) >= (int32_t)FM_OTA_JOIN_MS) {
      Serial.printf("[OTA] could not join '%s' (WiFi status %d). Check: hotspot on, "
                    "2.4 GHz band, WPA2, password.\n",
                    g_ssid, (int)WiFi.status());
      shutdown(nullptr);
      return FM_OTA_EV_FAILED;
    }
    return FM_OTA_EV_NONE;
  }

  g_server.handleClient();   // blocks for the few seconds an update transfer takes
  if (elapsed(now, g_openedAt) >= (int32_t)g_windowMs) {
    shutdown("time is up");
    return FM_OTA_EV_CLOSED;
  }
  return FM_OTA_EV_NONE;
}

const char *fmOtaHost() { return g_host; }
const char *fmOtaIp() { return g_ip; }

uint32_t fmOtaMinutesLeft(uint32_t now) {
  if (g_state == ST_IDLE) return 0;
  const int32_t e = elapsed(now, g_openedAt);
  const uint32_t used = e < 0 ? 0 : (uint32_t)e;
  return used >= g_windowMs ? 0 : (g_windowMs - used + 59999UL) / 60000UL;
}

const char *fmOtaImageId() { return g_image; }
