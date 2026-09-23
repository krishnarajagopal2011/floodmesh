/**
 * FloodMesh - BLE provisioning mode, unit side of FMREG v1.
 * See docs/ble-provisioning-protocol.md for the contract and fm_prov.h.
 *
 * Threading: NimBLE callbacks run on the BLE host task. They only copy the
 * written command into a buffer and raise a flag; all parsing, crypto and NVS
 * work happens on the Arduino loop task in fmProvRun(). The INFO read
 * callback builds its JSON from plain globals that only the loop task writes.
 */
#include "fm_prov.h"

#include <ArduinoJson.h>
#include <NimBLEDevice.h>
#include <esp_system.h>

#include "fm_ids.h"
#include "fm_role.h"

#ifndef FLOODMESH_VERSION
#define FLOODMESH_VERSION "dev"
#endif

namespace {

const char *kSvcUuid  = "6e3f0001-8a4c-4b8e-9f10-f10d3e5a0001";
const char *kInfoUuid = "6e3f0002-8a4c-4b8e-9f10-f10d3e5a0001";
const char *kCmdUuid  = "6e3f0003-8a4c-4b8e-9f10-f10d3e5a0001";
const char *kRespUuid = "6e3f0004-8a4c-4b8e-9f10-f10d3e5a0001";

const size_t kCmdMax = 600;

#if FM_BOARD_PROTO_V2
const char *kBoard = "proto_v2";
#else
const char *kBoard = "heltec_v3";
#endif

char g_pin[7];                 // 6 digits
char g_nonce[33];              // 32 hex chars
volatile uint8_t g_fails = 0;
volatile bool g_connected = false;
volatile uint32_t g_lastConnMs = 0;

// Command hand-off from the BLE task to the loop task.
portMUX_TYPE g_mux = portMUX_INITIALIZER_UNLOCKED;
char g_cmdBuf[kCmdMax + 1];
volatile size_t g_cmdLen = 0;
volatile bool g_cmdPending = false;
volatile bool g_cmdTooLong = false;

NimBLECharacteristic *g_resp = nullptr;

void newNonce() {
  uint8_t n[16];
  for (int i = 0; i < 16; i += 4) {
    const uint32_t r = esp_random();
    memcpy(n + i, &r, 4);
  }
  fmToHex(n, sizeof(n), g_nonce);
}

void newPin() {
  snprintf(g_pin, sizeof(g_pin), "%06lu", (unsigned long)(esp_random() % 1000000UL));
}

std::string buildInfo() {
  char fp[17];
  fmAdminFingerprint(fp);
  char devPub[FM_P256_PUB_LEN * 2 + 1] = "";
  uint8_t pub[FM_P256_PUB_LEN];
  if (fmRoleDevPub(pub)) fmToHex(pub, sizeof(pub), devPub);

  const FmAdminSource src = fmAdminSource();
  char out[512];
  snprintf(out, sizeof(out),
           "{\"v\":1,\"cs\":\"%s\",\"fw\":\"%s\",\"board\":\"%s\",\"role\":\"%s\","
           "\"exp\":%lu,\"now\":%lu,\"admin\":\"%s\",\"adminFp\":\"%s\","
           "\"nonce\":\"%s\",\"devPub\":\"%s\",\"hasCert\":%s,\"fails\":%u}",
           fmCallSign(), FLOODMESH_VERSION, kBoard, fmRoleName(fmRole()),
           (unsigned long)fmRoleExpiry(), (unsigned long)fmTimeNow(),
           src == FM_ADMIN_COMPILED ? "compiled"
           : src == FM_ADMIN_STORED ? "stored"
                                    : "none",
           fp, g_nonce, devPub, fmRoleHasCert() ? "true" : "false", (unsigned)g_fails);
  return std::string(out);
}

class ServerCb : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer *) override {
    g_connected = true;
    g_lastConnMs = millis();
  }
  void onDisconnect(NimBLEServer *) override {
    g_connected = false;
    g_lastConnMs = millis();
  }
};

class InfoCb : public NimBLECharacteristicCallbacks {
  void onRead(NimBLECharacteristic *c) override { c->setValue(buildInfo()); }
};

class CmdCb : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic *c) override {
    // getValue() is std::string on older NimBLE and NimBLEAttValue on 1.4+;
    // both offer data() and length().
    auto v = c->getValue();
    const char *d = (const char *)v.data();
    const size_t n = v.length();
    portENTER_CRITICAL(&g_mux);
    if (n > kCmdMax) {
      g_cmdTooLong = true;
    } else {
      memcpy(g_cmdBuf, d, n);
      g_cmdBuf[n] = '\0';
      g_cmdLen = n;
      g_cmdTooLong = false;
    }
    g_cmdPending = true;
    portEXIT_CRITICAL(&g_mux);
  }
};

void respond(const char *json) {
  Serial.printf("[PROV] <- %s\n", json);
  if (!g_resp) return;
  g_resp->setValue(std::string(json));
  g_resp->notify();
}

void respondErr(const char *code) {
  char b[64];
  snprintf(b, sizeof(b), "{\"ok\":false,\"err\":\"%s\"}", code);
  respond(b);
}

/** Check auth = hex(HMAC-SHA256(PIN, nonce + "|" + op)). */
bool authOk(const char *op, const char *auth) {
  if (!op || !auth || strlen(auth) != 64) return false;
  char msg[33 + 1 + 24];
  snprintf(msg, sizeof(msg), "%s|%s", g_nonce, op);
  uint8_t mac[32];
  fmHmacSha256((const uint8_t *)g_pin, 6, (const uint8_t *)msg, strlen(msg), mac);
  char hex[65];
  fmToHex(mac, sizeof(mac), hex);
  // Constant-time compare, case-insensitive on the received side.
  uint8_t diff = 0;
  for (size_t i = 0; i < 64; i++) {
    char a = auth[i];
    if (a >= 'A' && a <= 'F') a = (char)(a - 'A' + 'a');
    diff |= (uint8_t)(a ^ hex[i]);
  }
  return diff == 0;
}

bool adminVerify(const char *message, const char *sigHex) {
  uint8_t admin[FM_P256_PUB_LEN], sig[FM_P256_SIG_LEN];
  if (!fmAdminKey(admin)) return false;
  if (!fmFromHex(sigHex, sig, sizeof(sig))) return false;
  return fmP256Verify(admin, (const uint8_t *)message, strlen(message), sig);
}

volatile bool g_rebootRequested = false;
FmProvDraw g_draw = nullptr;
char g_status[32] = "waiting for app";

void redraw() {
  if (!g_draw) return;
  char l2[24], l3[24];
  snprintf(l2, sizeof(l2), "%s  PIN %s", fmCallSign(), g_pin);
  const uint32_t rem = fmRoleRemainingS();
  if (fmRole() == FM_ROLE_RESPONDER) {
    snprintf(l3, sizeof(l3), "role RSP %lud %luh", (unsigned long)(rem / 86400),
             (unsigned long)((rem % 86400) / 3600));
  } else {
    snprintf(l3, sizeof(l3), "role %s", fmRoleName(fmRole()));
  }
  g_draw("PROVISIONING", l2, l3, g_status);
}

void setStatus(const char *s) {
  snprintf(g_status, sizeof(g_status), "%s", s);
  redraw();
}

void handle(const char *json) {
  Serial.printf("[PROV] -> %s\n", json);
  if (g_fails >= FM_PROV_MAX_FAILS) { respondErr("locked"); return; }

  JsonDocument doc;
  if (deserializeJson(doc, json) != DeserializationError::Ok) { respondErr("json"); return; }
  const char *op = doc["op"] | "";
  const char *auth = doc["auth"] | "";
  if (!*op) { respondErr("op"); return; }

  if (!authOk(op, auth)) {
    g_fails = (uint8_t)(g_fails + 1);
    Serial.printf("[PROV] wrong PIN proof (%u/%u)\n", (unsigned)g_fails,
                  (unsigned)FM_PROV_MAX_FAILS);
    respondErr(g_fails >= FM_PROV_MAX_FAILS ? "locked" : "auth");
    setStatus(g_fails >= FM_PROV_MAX_FAILS ? "LOCKED: too many PINs" : "wrong PIN");
    return;
  }

  char out[256];

  if (strcmp(op, "set_admin") == 0) {
    if (fmAdminSource() != FM_ADMIN_NONE) { respondErr("admin_set"); return; }
    uint8_t pub[FM_P256_PUB_LEN];
    const char *pubHex = doc["pub"] | "";
    // Reject anything that is not a valid point on P-256 before trusting it.
    if (!fmFromHex(pubHex, pub, sizeof(pub)) || !fmP256PubValid(pub)) {
      respondErr("badkey");
      return;
    }
    if (!fmAdminSetKey(pub)) { respondErr("storage"); return; }
    char fp[17];
    fmAdminFingerprint(fp);
    snprintf(out, sizeof(out), "{\"ok\":true,\"adminFp\":\"%s\"}", fp);
    newNonce();
    respond(out);
    setStatus("admin key installed");
    return;
  }

  if (strcmp(op, "set_role") == 0) {
    if (fmAdminSource() == FM_ADMIN_NONE) { respondErr("no_admin"); return; }
    const char *roleStr = doc["role"] | "";
    FmRole role;
    if (!fmRoleParse(roleStr, &role)) { respondErr("role"); return; }
    const uint32_t now = doc["now"] | 0UL;
    const uint32_t exp = doc["exp"] | 0UL;
    const char *sig = doc["sig"] | "";

    char msg[128];
    snprintf(msg, sizeof(msg), "FMREG1|%s|%s|%lu|%lu|%s", fmCallSign(), roleStr,
             (unsigned long)now, (unsigned long)exp, g_nonce);
    if (!adminVerify(msg, sig)) { respondErr("sig"); setStatus("bad admin signature"); return; }

    if (now < FM_TIME_MIN_VALID) { respondErr("exp"); return; }
    if (role == FM_ROLE_RESPONDER) {
      if (!(exp > now && (exp - now) <= FM_RESPONDER_MAX_S)) { respondErr("exp"); return; }
    } else if (exp != 0) {
      respondErr("exp");
      return;
    }

    fmTimeSet(now);
    uint8_t devPub[FM_P256_PUB_LEN];
    if (!fmRoleApply(role, role == FM_ROLE_RESPONDER ? exp : 0, devPub)) {
      respondErr("storage");
      return;
    }
    char devHex[FM_P256_PUB_LEN * 2 + 1] = "";
    if (role == FM_ROLE_RESPONDER) fmToHex(devPub, sizeof(devPub), devHex);
    snprintf(out, sizeof(out), "{\"ok\":true,\"role\":\"%s\",\"exp\":%lu,\"devPub\":\"%s\"}",
             fmRoleName(role), (unsigned long)fmRoleExpiry(), devHex);
    newNonce();
    respond(out);
    char st[32];
    snprintf(st, sizeof(st), "now %s", fmRoleName(role));
    setStatus(st);
    return;
  }

  if (strcmp(op, "set_cert") == 0) {
    uint8_t devPub[FM_P256_PUB_LEN];
    if (!fmRoleDevPub(devPub)) { respondErr("nocert_state"); return; }
    const uint32_t exp = doc["exp"] | 0UL;
    if (exp != fmRoleExpiry()) { respondErr("exp"); return; }
    const char *sigHex = doc["sig"] | "";
    char devHex[FM_P256_PUB_LEN * 2 + 1];
    fmToHex(devPub, sizeof(devPub), devHex);
    char msg[220];
    snprintf(msg, sizeof(msg), "FMCERT1|%s|responder|%lu|%s", fmCallSign(), (unsigned long)exp,
             devHex);
    if (!adminVerify(msg, sigHex)) { respondErr("sig"); return; }
    uint8_t sig[FM_P256_SIG_LEN];
    fmFromHex(sigHex, sig, sizeof(sig));
    if (!fmRoleSetCert(sig)) { respondErr("storage"); return; }
    newNonce();
    respond("{\"ok\":true}");
    setStatus("responder certified");
    return;
  }

  if (strcmp(op, "reboot") == 0) {
    respond("{\"ok\":true}");
    setStatus("done - rebooting");
    g_rebootRequested = true;
    return;
  }

  respondErr("op");
}

}  // namespace

void fmProvRun(FmProvDraw draw) {
  g_draw = draw;
  newPin();
  newNonce();

  char name[8 + FM_CALLSIGN_LEN];
  snprintf(name, sizeof(name), "FM-%s", fmCallSign());

  Serial.println("=========================================================");
  Serial.printf("  PROVISIONING MODE  %s  PIN %s\n", name, g_pin);
  Serial.println("=========================================================");

  NimBLEDevice::init(name);
  NimBLEDevice::setMTU(517);
  NimBLEServer *server = NimBLEDevice::createServer();
  server->setCallbacks(new ServerCb());
  NimBLEService *svc = server->createService(kSvcUuid);
  NimBLECharacteristic *info = svc->createCharacteristic(kInfoUuid, NIMBLE_PROPERTY::READ);
  info->setCallbacks(new InfoCb());
  NimBLECharacteristic *cmd = svc->createCharacteristic(kCmdUuid, NIMBLE_PROPERTY::WRITE);
  cmd->setCallbacks(new CmdCb());
  g_resp = svc->createCharacteristic(kRespUuid, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);
  g_resp->setValue(std::string("{}"));
  svc->start();

  NimBLEAdvertising *adv = NimBLEDevice::getAdvertising();
  adv->addServiceUUID(kSvcUuid);
  adv->setName(name);
  adv->setScanResponse(true);
  adv->start();

  redraw();

  const uint32_t start = millis();
  g_lastConnMs = start;
  bool wasConnected = false;
  uint32_t rebootAt = 0;

  for (;;) {
    const uint32_t now = millis();

    if (g_connected != wasConnected) {
      wasConnected = g_connected;
      if (wasConnected) newNonce();   // fresh nonce per connection
      setStatus(wasConnected ? "app connected" : "waiting for app");
    }

    if (g_cmdPending) {
      char local[kCmdMax + 1];
      bool tooLong;
      portENTER_CRITICAL(&g_mux);
      tooLong = g_cmdTooLong;
      memcpy(local, g_cmdBuf, g_cmdLen + 1);
      g_cmdPending = false;
      portEXIT_CRITICAL(&g_mux);
      if (tooLong) respondErr("json");
      else handle(local);
    }

    if (g_rebootRequested && rebootAt == 0) rebootAt = now + 500;
    const bool idle = !g_connected && (now - g_lastConnMs) >= FM_PROV_IDLE_TIMEOUT_MS;
    const bool capped = (now - start) >= FM_PROV_MAX_MS;
    const bool locked = g_fails >= FM_PROV_MAX_FAILS && !g_connected;
    if ((rebootAt && (int32_t)(now - rebootAt) >= 0) || idle || capped || locked) {
      Serial.printf("[PROV] leaving provisioning mode (%s)\n",
                    rebootAt ? "app finished" : idle ? "idle timeout"
                                          : capped ? "session cap" : "too many wrong PINs");
      setStatus("rebooting...");
      delay(300);
      NimBLEDevice::deinit(true);
      ESP.restart();
    }
    fmRoleLoop(now);
    delay(20);
  }
}
