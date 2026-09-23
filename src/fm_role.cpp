/**
 * FloodMesh - roles, responder keys, expiry, time and P-256 helpers.
 * See fm_role.h.
 */
#include "fm_role.h"

#include <Preferences.h>
#include <esp_system.h>
#include <sys/time.h>
#include <time.h>

#include <mbedtls/ecdsa.h>
#include <mbedtls/ecp.h>
#include <mbedtls/md.h>

namespace {

const char *kNs = "fmrole";
const char *kKeyRole = "role";
const char *kKeyExp = "exp";
const char *kKeyAdmin = "admin";
const char *kKeyPriv = "dpriv";
const char *kKeyPub = "dpub";
const char *kKeyCert = "cert";
const char *kKeyTime = "tsave";

FmRole   g_role = FM_ROLE_CIVILIAN;
uint32_t g_exp = 0;
bool     g_hasDev = false;
uint8_t  g_devPriv[FM_P256_PRIV_LEN];
uint8_t  g_devPub[FM_P256_PUB_LEN];
bool     g_hasCert = false;

FmAdminSource g_adminSrc = FM_ADMIN_NONE;
uint8_t       g_admin[FM_P256_PUB_LEN];

uint32_t g_lastSaveMs = 0;
uint32_t g_lastCheckMs = 0;
bool     g_expiredEvent = false;

int rngFn(void *, unsigned char *out, size_t len) {
  while (len > 0) {
    const uint32_t r = esp_random();
    const size_t n = len < 4 ? len : 4;
    memcpy(out, &r, n);
    out += n;
    len -= n;
  }
  return 0;
}

void wipe(void *p, size_t n) {
  volatile uint8_t *v = (volatile uint8_t *)p;
  while (n--) *v++ = 0;
}

bool validPubFormat(const uint8_t pub[FM_P256_PUB_LEN]) { return fmP256PubValid(pub); }

/** Drop the responder key, certificate and expiry (RAM and NVS). */
void clearResponder(Preferences &p) {
  p.remove(kKeyPriv);
  p.remove(kKeyPub);
  p.remove(kKeyCert);
  p.remove(kKeyExp);
  wipe(g_devPriv, sizeof(g_devPriv));
  memset(g_devPub, 0, sizeof(g_devPub));
  g_hasDev = false;
  g_hasCert = false;
  g_exp = 0;
}

void saveTimeNow() {
  const uint32_t t = fmTimeNow();
  if (t == 0) return;
  Preferences p;
  if (!p.begin(kNs, false)) return;
  if (p.getUInt(kKeyTime, 0) < t) p.putUInt(kKeyTime, t);
  p.end();
}

}  // namespace

// ---------------------------------------------------------------- lifecycle
void fmRoleBegin() {
  Preferences p;
  if (!p.begin(kNs, false)) {
    Serial.println("[ROLE] !! NVS unavailable - running as civilian");
    return;
  }

  // Time first: a responder's expiry is judged against it.
  const uint32_t saved = p.getUInt(kKeyTime, 0);
  if (saved >= FM_TIME_MIN_VALID && fmTimeNow() < saved) fmTimeSet(saved);

  // Admin key: compiled in wins over stored.
  const char *hex = FM_ADMIN_PUBKEY_HEX;
  if (strlen(hex) == FM_P256_PUB_LEN * 2 && fmFromHex(hex, g_admin, FM_P256_PUB_LEN) &&
      validPubFormat(g_admin)) {
    g_adminSrc = FM_ADMIN_COMPILED;
  } else if (p.getBytes(kKeyAdmin, g_admin, FM_P256_PUB_LEN) == FM_P256_PUB_LEN &&
             validPubFormat(g_admin)) {
    g_adminSrc = FM_ADMIN_STORED;
  } else {
    g_adminSrc = FM_ADMIN_NONE;
    if (strlen(hex) != 0) {
      Serial.println("[ROLE] !! FM_ADMIN_PUBKEY_HEX is malformed (need 130 hex chars "
                     "starting 04) - ignored");
    }
  }

  const uint8_t r = p.getUChar(kKeyRole, FM_ROLE_CIVILIAN);
  g_role = (r <= FM_ROLE_RESPONDER) ? (FmRole)r : FM_ROLE_CIVILIAN;
  if (g_role == FM_ROLE_RESPONDER) {
    g_exp = p.getUInt(kKeyExp, 0);
    g_hasDev = p.getBytes(kKeyPriv, g_devPriv, FM_P256_PRIV_LEN) == FM_P256_PRIV_LEN &&
               p.getBytes(kKeyPub, g_devPub, FM_P256_PUB_LEN) == FM_P256_PUB_LEN;
    uint8_t sig[FM_P256_SIG_LEN];
    g_hasCert = p.getBytes(kKeyCert, sig, sizeof(sig)) == sizeof(sig);
    if (!g_hasDev || g_exp == 0) {
      Serial.println("[ROLE] !! responder state incomplete - reverting to civilian");
      clearResponder(p);
      g_role = FM_ROLE_CIVILIAN;
      p.putUChar(kKeyRole, FM_ROLE_CIVILIAN);
    }
  }
  p.end();

  char fp[17];
  fmAdminFingerprint(fp);
  Serial.printf("[ROLE] %s%s, admin key %s%s%s, clock %s\n", fmRoleName(g_role),
                g_role == FM_ROLE_RESPONDER ? (g_hasCert ? " (certified)" : " (NO cert)") : "",
                g_adminSrc == FM_ADMIN_COMPILED ? "compiled-in"
                : g_adminSrc == FM_ADMIN_STORED ? "stored"
                                                : "NONE",
                fp[0] ? " fp " : "", fp, fmTimeNow() ? "set" : "NOT SET");
  // Expiry is checked on the first fmRoleLoop() call.
  g_lastCheckMs = millis() - 60000UL;
}

bool fmRoleLoop(uint32_t nowMs) {
  if (nowMs - g_lastSaveMs >= FM_TIME_SAVE_MS) {
    g_lastSaveMs = nowMs;
    saveTimeNow();
  }
  if (nowMs - g_lastCheckMs >= 60000UL) {
    g_lastCheckMs = nowMs;
    const uint32_t t = fmTimeNow();
    if (g_role == FM_ROLE_RESPONDER && t != 0 && t >= g_exp) {
      Serial.println("[ROLE] responder grant EXPIRED - key deleted, now civilian");
      fmRoleApply(FM_ROLE_CIVILIAN, 0, nullptr);
      g_expiredEvent = true;
    }
  }
  if (g_expiredEvent) {
    g_expiredEvent = false;
    return true;
  }
  return false;
}

// ---------------------------------------------------------------- role
FmRole fmRole() { return g_role; }

const char *fmRoleName(FmRole r) {
  switch (r) {
    case FM_ROLE_RELAY:     return "relay";
    case FM_ROLE_RESPONDER: return "responder";
    default:                return "civilian";
  }
}

const char *fmRoleTag(FmRole r) {
  switch (r) {
    case FM_ROLE_RELAY:     return "RLY";
    case FM_ROLE_RESPONDER: return "RSP";
    default:                return "CIV";
  }
}

bool fmRoleParse(const char *s, FmRole *out) {
  if (!s || !out) return false;
  if (strcmp(s, "civilian") == 0) { *out = FM_ROLE_CIVILIAN; return true; }
  if (strcmp(s, "relay") == 0) { *out = FM_ROLE_RELAY; return true; }
  if (strcmp(s, "responder") == 0) { *out = FM_ROLE_RESPONDER; return true; }
  return false;
}

uint32_t fmRoleExpiry() { return g_role == FM_ROLE_RESPONDER ? g_exp : 0; }

uint32_t fmRoleRemainingS() {
  const uint32_t t = fmTimeNow();
  if (g_role != FM_ROLE_RESPONDER || t == 0 || t >= g_exp) return 0;
  return g_exp - t;
}

bool fmRoleApply(FmRole role, uint32_t expiry, uint8_t *devPubOut) {
  Preferences p;
  if (!p.begin(kNs, false)) return false;
  clearResponder(p);

  if (role == FM_ROLE_RESPONDER) {
    uint8_t priv[FM_P256_PRIV_LEN], pub[FM_P256_PUB_LEN];
    if (!fmP256Generate(priv, pub)) {
      p.end();
      return false;
    }
    const bool ok = p.putBytes(kKeyPriv, priv, sizeof(priv)) == sizeof(priv) &&
                    p.putBytes(kKeyPub, pub, sizeof(pub)) == sizeof(pub) &&
                    p.putUInt(kKeyExp, expiry) == sizeof(uint32_t);
    if (!ok) {
      clearResponder(p);
      wipe(priv, sizeof(priv));
      p.putUChar(kKeyRole, FM_ROLE_CIVILIAN);
      g_role = FM_ROLE_CIVILIAN;
      p.end();
      return false;
    }
    memcpy(g_devPriv, priv, sizeof(priv));
    memcpy(g_devPub, pub, sizeof(pub));
    wipe(priv, sizeof(priv));
    g_hasDev = true;
    g_exp = expiry;
    if (devPubOut) memcpy(devPubOut, pub, sizeof(pub));
  }

  g_role = role;
  const bool ok = p.putUChar(kKeyRole, (uint8_t)role) == 1;
  p.end();
  saveTimeNow();
  Serial.printf("[ROLE] -> %s%s\n", fmRoleName(role),
                role == FM_ROLE_RESPONDER ? " (new key pair generated)" : "");
  return ok;
}

bool fmRoleDevPub(uint8_t out[FM_P256_PUB_LEN]) {
  if (g_role != FM_ROLE_RESPONDER || !g_hasDev) return false;
  memcpy(out, g_devPub, FM_P256_PUB_LEN);
  return true;
}

bool fmRoleHasCert() { return g_role == FM_ROLE_RESPONDER && g_hasCert; }

bool fmRoleSetCert(const uint8_t sig[FM_P256_SIG_LEN]) {
  if (g_role != FM_ROLE_RESPONDER || !g_hasDev) return false;
  Preferences p;
  if (!p.begin(kNs, false)) return false;
  const bool ok = p.putBytes(kKeyCert, sig, FM_P256_SIG_LEN) == FM_P256_SIG_LEN;
  p.end();
  g_hasCert = ok;
  return ok;
}

bool fmRoleSign(const uint8_t *msg, size_t len, uint8_t sig[FM_P256_SIG_LEN]) {
  if (g_role != FM_ROLE_RESPONDER || !g_hasDev) return false;
  return fmP256Sign(g_devPriv, msg, len, sig);
}

// ---------------------------------------------------------------- admin key
FmAdminSource fmAdminSource() { return g_adminSrc; }

bool fmAdminKey(uint8_t out[FM_P256_PUB_LEN]) {
  if (g_adminSrc == FM_ADMIN_NONE) return false;
  memcpy(out, g_admin, FM_P256_PUB_LEN);
  return true;
}

bool fmAdminSetKey(const uint8_t pub[FM_P256_PUB_LEN]) {
  if (g_adminSrc != FM_ADMIN_NONE || !validPubFormat(pub)) return false;
  Preferences p;
  if (!p.begin(kNs, false)) return false;
  const bool ok = p.putBytes(kKeyAdmin, pub, FM_P256_PUB_LEN) == FM_P256_PUB_LEN;
  p.end();
  if (ok) {
    memcpy(g_admin, pub, FM_P256_PUB_LEN);
    g_adminSrc = FM_ADMIN_STORED;
  }
  return ok;
}

void fmAdminFingerprint(char out[17]) {
  out[0] = '\0';
  if (g_adminSrc == FM_ADMIN_NONE) return;
  uint8_t h[32];
  fmSha256(g_admin, FM_P256_PUB_LEN, h);
  fmToHex(h, 8, out);
}

void fmRoleFactoryReset() {
  Preferences p;
  if (p.begin(kNs, false)) {
    clearResponder(p);
    p.remove(kKeyAdmin);
    p.remove(kKeyRole);
    p.end();
  }
  g_role = FM_ROLE_CIVILIAN;
  if (g_adminSrc == FM_ADMIN_STORED) g_adminSrc = FM_ADMIN_NONE;
  Serial.println("[ROLE] factory reset: admin key, role and responder key erased");
}

// ---------------------------------------------------------------- time
uint32_t fmTimeNow() {
  const time_t t = time(nullptr);
  return (t >= (time_t)FM_TIME_MIN_VALID) ? (uint32_t)t : 0;
}

void fmTimeSet(uint32_t unixSeconds) {
  if (unixSeconds < FM_TIME_MIN_VALID) return;
  if (unixSeconds <= fmTimeNow()) return;   // forward only
  struct timeval tv;
  tv.tv_sec = (time_t)unixSeconds;
  tv.tv_usec = 0;
  settimeofday(&tv, nullptr);
}

// ---------------------------------------------------------------- crypto
void fmSha256(const uint8_t *data, size_t len, uint8_t out[32]) {
  mbedtls_md(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), data, len, out);
}

void fmHmacSha256(const uint8_t *key, size_t keyLen, const uint8_t *msg, size_t len,
                  uint8_t out[32]) {
  mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), key, keyLen, msg, len, out);
}

bool fmP256Verify(const uint8_t pub[FM_P256_PUB_LEN], const uint8_t *msg, size_t len,
                  const uint8_t sig[FM_P256_SIG_LEN]) {
  uint8_t hash[32];
  fmSha256(msg, len, hash);

  mbedtls_ecp_group grp;
  mbedtls_ecp_point q;
  mbedtls_mpi r, s;
  mbedtls_ecp_group_init(&grp);
  mbedtls_ecp_point_init(&q);
  mbedtls_mpi_init(&r);
  mbedtls_mpi_init(&s);

  const bool ok = mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_SECP256R1) == 0 &&
                  mbedtls_ecp_point_read_binary(&grp, &q, pub, FM_P256_PUB_LEN) == 0 &&
                  mbedtls_ecp_check_pubkey(&grp, &q) == 0 &&
                  mbedtls_mpi_read_binary(&r, sig, 32) == 0 &&
                  mbedtls_mpi_read_binary(&s, sig + 32, 32) == 0 &&
                  mbedtls_ecdsa_verify(&grp, hash, sizeof(hash), &q, &r, &s) == 0;

  mbedtls_mpi_free(&s);
  mbedtls_mpi_free(&r);
  mbedtls_ecp_point_free(&q);
  mbedtls_ecp_group_free(&grp);
  return ok;
}

bool fmP256PubValid(const uint8_t pub[FM_P256_PUB_LEN]) {
  if (pub[0] != 0x04) return false;
  mbedtls_ecp_group grp;
  mbedtls_ecp_point q;
  mbedtls_ecp_group_init(&grp);
  mbedtls_ecp_point_init(&q);
  const bool ok = mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_SECP256R1) == 0 &&
                  mbedtls_ecp_point_read_binary(&grp, &q, pub, FM_P256_PUB_LEN) == 0 &&
                  mbedtls_ecp_check_pubkey(&grp, &q) == 0;
  mbedtls_ecp_point_free(&q);
  mbedtls_ecp_group_free(&grp);
  return ok;
}

bool fmP256Generate(uint8_t priv[FM_P256_PRIV_LEN], uint8_t pub[FM_P256_PUB_LEN]) {
  mbedtls_ecp_group grp;
  mbedtls_ecp_point q;
  mbedtls_mpi d;
  mbedtls_ecp_group_init(&grp);
  mbedtls_ecp_point_init(&q);
  mbedtls_mpi_init(&d);

  size_t olen = 0;
  const bool ok =
      mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_SECP256R1) == 0 &&
      mbedtls_ecp_gen_keypair(&grp, &d, &q, rngFn, nullptr) == 0 &&
      mbedtls_mpi_write_binary(&d, priv, FM_P256_PRIV_LEN) == 0 &&
      mbedtls_ecp_point_write_binary(&grp, &q, MBEDTLS_ECP_PF_UNCOMPRESSED, &olen, pub,
                                     FM_P256_PUB_LEN) == 0 &&
      olen == FM_P256_PUB_LEN;

  mbedtls_mpi_free(&d);
  mbedtls_ecp_point_free(&q);
  mbedtls_ecp_group_free(&grp);
  return ok;
}

bool fmP256Sign(const uint8_t priv[FM_P256_PRIV_LEN], const uint8_t *msg, size_t len,
                uint8_t sig[FM_P256_SIG_LEN]) {
  uint8_t hash[32];
  fmSha256(msg, len, hash);

  mbedtls_ecp_group grp;
  mbedtls_mpi d, r, s;
  mbedtls_ecp_group_init(&grp);
  mbedtls_mpi_init(&d);
  mbedtls_mpi_init(&r);
  mbedtls_mpi_init(&s);

  const bool ok = mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_SECP256R1) == 0 &&
                  mbedtls_mpi_read_binary(&d, priv, FM_P256_PRIV_LEN) == 0 &&
                  mbedtls_ecdsa_sign(&grp, &r, &s, &d, hash, sizeof(hash), rngFn, nullptr) ==
                      0 &&
                  mbedtls_mpi_write_binary(&r, sig, 32) == 0 &&
                  mbedtls_mpi_write_binary(&s, sig + 32, 32) == 0;

  mbedtls_mpi_free(&s);
  mbedtls_mpi_free(&r);
  mbedtls_mpi_free(&d);
  mbedtls_ecp_group_free(&grp);
  return ok;
}

void fmToHex(const uint8_t *in, size_t len, char *out) {
  static const char kHex[] = "0123456789abcdef";
  for (size_t i = 0; i < len; i++) {
    out[2 * i] = kHex[in[i] >> 4];
    out[2 * i + 1] = kHex[in[i] & 0x0F];
  }
  out[2 * len] = '\0';
}

static int hexNibble(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

bool fmFromHex(const char *hex, uint8_t *out, size_t outLen) {
  if (!hex || strlen(hex) != outLen * 2) return false;
  for (size_t i = 0; i < outLen; i++) {
    const int hi = hexNibble(hex[2 * i]);
    const int lo = hexNibble(hex[2 * i + 1]);
    if (hi < 0 || lo < 0) return false;
    out[i] = (uint8_t)((hi << 4) | lo);
  }
  return true;
}
