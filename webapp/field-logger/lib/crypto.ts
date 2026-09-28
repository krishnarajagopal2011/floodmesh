/**
 * HMAC helpers for unit uploads (protocol section 1) and session cookies.
 * Every comparison of a secret-derived value is constant-time.
 */
import { createHash, createHmac, timingSafeEqual } from "node:crypto";

export function hmacSha256(key: Buffer | string, data: Buffer | string): Buffer {
  return createHmac("sha256", key).update(data).digest();
}

/** Constant-time string equality that does not leak the length either. */
export function safeEqual(a: string, b: string): boolean {
  const ha = createHash("sha256").update(a, "utf8").digest();
  const hb = createHash("sha256").update(b, "utf8").digest();
  return timingSafeEqual(ha, hb);
}

/**
 * Check X-FM-Signature: lowercase hex HMAC-SHA256 of the raw body bytes,
 * keyed with the fleet log key.
 *
 * The protocol gives the key as a hex string (FM_LOG_KEY / LOG_KEY) but does
 * not say whether the HMAC key is the decoded bytes or the ASCII text of that
 * string. Both are derived from the same secret, so accepting either costs no
 * security and keeps a unit uploading whichever way its firmware reads it.
 * firmware_v4 (fm_log.cpp) and the simulator sign with the decoded bytes.
 */
export function verifyUploadSignature(raw: Buffer, signatureHex: string, logKeyHex: string): boolean {
  const sig = signatureHex.trim().toLowerCase();
  if (!/^[0-9a-f]{64}$/.test(sig)) return false;
  const given = Buffer.from(sig, "hex");
  const asBytes = hmacSha256(Buffer.from(logKeyHex, "hex"), raw);
  const asText = hmacSha256(Buffer.from(logKeyHex, "utf8"), raw);
  // Evaluate both before combining, so timing does not reveal which matched.
  const a = timingSafeEqual(given, asBytes);
  const b = timingSafeEqual(given, asText);
  return a || b;
}
