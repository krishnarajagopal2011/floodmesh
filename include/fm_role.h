/**
 * FloodMesh - unit role (civilian / relay / responder), responder keys and
 * expiry, wall-clock time, and the P-256 crypto helpers they need.
 *
 * Roles are set over BLE by the super admin (fm_prov, protocol in
 * docs/ble-provisioning-protocol.md). A unit with no stored role is a
 * civilian; civilians need no registration.
 *
 * Responder: the unit generates its own P-256 key pair when it becomes a
 * responder; the private key never leaves it. At expiry the private key and
 * the admin certificate are DELETED and the unit becomes a civilian again.
 * Renewal means registering again over BLE, which creates a new key.
 *
 * Time: there is no RTC crystal and no GPS. The admin app sets the clock at
 * registration; the ESP32 keeps counting through light/deep sleep and soft
 * resets; the last known time is saved to NVS every hour and restored at
 * boot. The clock is only ever moved forward, so a power-off shortens nothing
 * but can extend a responder by the length of the power-off. That leak is
 * bounded and accepted (docs/architecture.md section 6.2).
 *
 * Storage: NVS namespace "fmrole".
 */
#pragma once
#include <Arduino.h>

#ifndef FM_RESPONDER_MAX_S
#define FM_RESPONDER_MAX_S 864000UL     // 10 days: the longest responder grant
#endif
#ifndef FM_RESPONDER_WARN_S
#define FM_RESPONDER_WARN_S 172800UL    // warn on screen from 2 days before expiry
#endif
#ifndef FM_TIME_SAVE_MS
#define FM_TIME_SAVE_MS 3600000UL       // persist the clock hourly
#endif
#ifndef FM_TIME_MIN_VALID
#define FM_TIME_MIN_VALID 1700000000UL  // anything earlier means "never set"
#endif

// Admin public key built into the firmware (130 hex chars, uncompressed
// P-256). Empty = none compiled in; one can then be installed once over BLE.
#ifndef FM_ADMIN_PUBKEY_HEX
#define FM_ADMIN_PUBKEY_HEX ""
#endif

#define FM_P256_PUB_LEN  65   // 0x04 || X || Y
#define FM_P256_PRIV_LEN 32
#define FM_P256_SIG_LEN  64   // r || s

enum FmRole : uint8_t {
  FM_ROLE_CIVILIAN = 0,
  FM_ROLE_RELAY = 1,
  FM_ROLE_RESPONDER = 2,
};

enum FmAdminSource : uint8_t {
  FM_ADMIN_NONE = 0,
  FM_ADMIN_COMPILED = 1,
  FM_ADMIN_STORED = 2,
};

// ---------------------------------------------------------------- lifecycle
/** Load role, keys and saved time from NVS. Call once at boot, after Serial. */
void fmRoleBegin();

/**
 * Periodic housekeeping: saves the clock hourly and checks responder expiry
 * once a minute. Returns true exactly once when a responder grant expires
 * (the key has already been deleted by then) so the UI can say so.
 */
bool fmRoleLoop(uint32_t nowMs);

// ---------------------------------------------------------------- role
FmRole      fmRole();
const char *fmRoleName(FmRole r);          // "civilian" / "relay" / "responder"
const char *fmRoleTag(FmRole r);           // "CIV" / "RLY" / "RSP"
bool        fmRoleParse(const char *s, FmRole *out);
uint32_t    fmRoleExpiry();                // Unix seconds; 0 unless responder
/** Seconds of responder grant left, 0 if not a responder or unknown time. */
uint32_t    fmRoleRemainingS();

/**
 * Change role. For FM_ROLE_RESPONDER a fresh key pair is generated and
 * devPubOut (65 bytes) receives the public half. For the other roles any
 * responder key and certificate are deleted and devPubOut is untouched.
 * Returns false only on a storage or key-generation failure.
 */
bool fmRoleApply(FmRole role, uint32_t expiry, uint8_t *devPubOut);

bool fmRoleDevPub(uint8_t out[FM_P256_PUB_LEN]);   // false unless responder
bool fmRoleHasCert();
bool fmRoleSetCert(const uint8_t sig[FM_P256_SIG_LEN]);

/** Sign msg with the responder private key (for later mesh messages). */
bool fmRoleSign(const uint8_t *msg, size_t len, uint8_t sig[FM_P256_SIG_LEN]);

// ---------------------------------------------------------------- admin key
FmAdminSource fmAdminSource();
bool          fmAdminKey(uint8_t out[FM_P256_PUB_LEN]);
/** Trust-on-first-use install. Fails if any admin key is already present. */
bool          fmAdminSetKey(const uint8_t pub[FM_P256_PUB_LEN]);
/** First 16 hex chars of SHA-256(admin key); empty string if none. */
void          fmAdminFingerprint(char out[17]);

/** Delete admin key (if stored), role, responder key, certificate. */
void fmRoleFactoryReset();

// ---------------------------------------------------------------- time
/** Current Unix time in seconds, or 0 if the clock has never been set. */
uint32_t fmTimeNow();
/** Set the clock. Ignored if it would move the clock backwards. */
void     fmTimeSet(uint32_t unixSeconds);

// ---------------------------------------------------------------- crypto
void fmSha256(const uint8_t *data, size_t len, uint8_t out[32]);
void fmHmacSha256(const uint8_t *key, size_t keyLen, const uint8_t *msg, size_t len,
                  uint8_t out[32]);
bool fmP256Verify(const uint8_t pub[FM_P256_PUB_LEN], const uint8_t *msg, size_t len,
                  const uint8_t sig[FM_P256_SIG_LEN]);
/** True if pub is a valid uncompressed point on P-256. */
bool fmP256PubValid(const uint8_t pub[FM_P256_PUB_LEN]);
bool fmP256Generate(uint8_t priv[FM_P256_PRIV_LEN], uint8_t pub[FM_P256_PUB_LEN]);
bool fmP256Sign(const uint8_t priv[FM_P256_PRIV_LEN], const uint8_t *msg, size_t len,
                uint8_t sig[FM_P256_SIG_LEN]);

/** Hex helpers: lowercase output; input accepts either case. */
void fmToHex(const uint8_t *in, size_t len, char *out);          // out: 2*len+1
bool fmFromHex(const char *hex, uint8_t *out, size_t outLen);    // exact length
