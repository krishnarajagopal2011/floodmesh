# BLE provisioning protocol — FMREG v1

The contract between the unit firmware (`src/fm_prov.cpp`) and the admin app
(`app/floodmesh_admin/`). Both sides must follow this document exactly; change
it here first.

Scope (prototype v2): a super admin uses the app to set a unit's **role**:
`civilian` (default), `relay`, or `responder` (expires after at most 10 days).
Civilian units need no registration. See `docs/architecture.md` §6.

---

## 1. Entering provisioning mode (unit side)

- **Hold `#` on the keypad while powering on** (or while pressing RST).
- Serial alternative for the bench: type `prov` + Enter at 115200 baud.
- The OLED shows `PROVISIONING`, the call sign and a **6-digit PIN**
  (random each time).
- Bluetooth is only on in this mode. It exits (reboots into normal mode)
  after **5 minutes without a connection**, on the `reboot` command, or after
  **5 failed PIN attempts**.
- The radio, audio and codec are not started in this mode.

## 2. Advertising and GATT layout

| Item | Value |
|---|---|
| Device name | `FM-<CALLSIGN>` e.g. `FM-K7Q2M` |
| Service UUID | `6e3f0001-8a4c-4b8e-9f10-f10d3e5a0001` |
| INFO characteristic | `6e3f0002-8a4c-4b8e-9f10-f10d3e5a0001` — READ |
| CMD characteristic | `6e3f0003-8a4c-4b8e-9f10-f10d3e5a0001` — WRITE (with response; long writes allowed) |
| RESP characteristic | `6e3f0004-8a4c-4b8e-9f10-f10d3e5a0001` — READ + NOTIFY |

The service UUID is in the advertising packet; the app filters scans on it.
The app should request MTU 517. All payloads are UTF-8 JSON objects, one per
write/read, no trailing newline. Maximum CMD size: 512 bytes (the ATT attribute limit).

Flow: read INFO → write CMD → wait for a RESP notification (or read RESP) →
read INFO again to refresh.

## 3. INFO (read)

```json
{
  "v": 1,
  "cs": "K7Q2M",
  "fw": "0.4.0",
  "board": "proto_v2",
  "role": "civilian",
  "exp": 0,
  "now": 0,
  "admin": "none",
  "adminFp": "",
  "nonce": "9f2c...(32 hex chars)",
  "devPub": "",
  "hasCert": false,
  "fails": 0
}
```

| Field | Meaning |
|---|---|
| `role` | `civilian` \| `relay` \| `responder` |
| `exp` | Responder expiry, Unix seconds UTC. 0 unless responder |
| `now` | Unit's current Unix time, 0 if it has never been set |
| `admin` | `none` (no admin key yet), `compiled` (built into firmware), `stored` (installed over BLE) |
| `adminFp` | First 16 hex chars of SHA-256(admin public key, 65-byte uncompressed form). Empty if none |
| `nonce` | 16 random bytes as 32 lowercase hex chars. New on every connection and after every successful command |
| `devPub` | Unit's own responder public key, 130 hex chars (uncompressed P-256). Empty unless responder |
| `hasCert` | Whether an admin certificate for `devPub` is stored |
| `fails` | Failed PIN attempts this session (mode exits at 5) |

## 4. Authentication

### 4.1 PIN proof (every command)
Every CMD carries `"auth"`:

```
auth = lowercase hex( HMAC-SHA256( key = the 6 PIN digits as ASCII,
                                   msg = nonce + "|" + op ) )
```

`nonce` is the value from the most recent INFO read, `op` the command name.
A wrong `auth` returns `{"ok":false,"err":"auth"}` and counts a failure.

The PIN proves the operator can see this unit's screen. It is not what
authorises a role change; the admin signature below does that.

### 4.2 Admin signature (role commands)
- Curve **P-256 (secp256r1)**, **ECDSA with SHA-256**.
- Public key: 65-byte uncompressed point `04 || X || Y`, 130 hex chars.
- Signature: raw `r || s`, each left-padded to 32 bytes, 128 hex chars.
- The unit verifies with the admin public key, which is either compiled in
  (`-D FM_ADMIN_PUBKEY_HEX="04..."`) or installed once with `set_admin`.

## 5. Commands (CMD write)

### 5.1 `set_admin` — install the admin public key (first use only)
Allowed only when INFO `admin` is `none`. After that the key can only be
removed by a factory reset on the unit itself.
```json
{"op":"set_admin","pub":"04ab...(130 hex)","auth":"..."}
```
Response: `{"ok":true,"adminFp":"..."}`

### 5.2 `set_role` — make the unit civilian, relay or responder
```json
{"op":"set_role","role":"responder","now":1790000000,"exp":1790864000,
 "sig":"...(128 hex)","auth":"..."}
```
Signed message (ASCII, fields joined with `|`, no spaces):
```
FMREG1|<cs>|<role>|<now>|<exp>|<nonce>
```
- `cs` is the unit's call sign from INFO; `nonce` the INFO nonce. A signature
  therefore can't be replayed on another unit or in another session.
- `now` is the phone's current Unix time; the unit sets its clock from it.
- `exp`: responder only; must satisfy `now < exp <= now + FM_RESPONDER_MAX_S`
  (864000 s = 10 days). For `civilian` and `relay` send `"exp":0`.

Unit behaviour:
| role | Effect |
|---|---|
| `civilian` | Deletes any responder key and certificate; role = civilian |
| `relay` | Deletes any responder key and certificate; role = relay |
| `responder` | Generates a **new** P-256 key pair inside the unit (the private key never leaves it), stores role + expiry |

Response: `{"ok":true,"role":"responder","exp":1790864000,"devPub":"04..."}`
(`devPub` empty for civilian/relay).

### 5.3 `set_cert` — store the admin certificate for a responder key
Sent right after a successful `set_role` responder.
```json
{"op":"set_cert","exp":1790864000,"sig":"...(128 hex)","auth":"..."}
```
Signed message:
```
FMCERT1|<cs>|responder|<exp>|<devPub hex, lowercase>
```
The unit verifies it with the admin key, checks `exp` matches its stored
expiry, and stores it. This certificate is what other units will check when a
responder message arrives (later firmware). Response: `{"ok":true}`

### 5.4 `reboot` — leave provisioning mode
```json
{"op":"reboot","auth":"..."}
```
Response `{"ok":true}`, then the unit reboots into normal mode ~500 ms later.

### 5.5 Errors
`{"ok":false,"err":"<code>"}` with one of: `json`, `op`, `auth`, `locked`,
`no_admin`, `admin_set`, `badkey`, `sig`, `cs`, `role`, `exp`, `nocert_state`,
`storage`.

## 6. Expiry on the unit

- The unit keeps time from `now` at registration, keeps counting through
  sleep, and saves the last known time to flash every hour; the clock only
  moves forward.
- Checked at boot and every minute: when `now >= exp`, the unit **deletes the
  responder private key and certificate** and becomes `civilian`, and shows
  `RESPONDER EXPIRED` once.
- From 2 days before expiry the standby screen shows a warning.
- Testing: the app offers a "10-minute test expiry"; nothing special is needed
  in the firmware because any `exp` up to 10 days is accepted.

## 7. Factory reset (unit side, no app)

Hold **SOS + `0`** while powering on, keep holding for 10 s: deletes admin
key, role, responder key and certificate, and the operator call-sign override.

## 8. Security notes (prototype)

- An attacker who records the BLE exchange could brute-force the 6-digit PIN
  offline from `auth`. That reveals only this session's PIN; role changes still
  need the admin signature, and the nonce changes after every command.
- `set_admin` trusts the first key it's given (trust on first use), protected
  only by the PIN. For production, compile the admin key into the firmware.
- The admin private key never leaves the phone. It is stored encrypted with a
  key derived from the admin passphrase.
