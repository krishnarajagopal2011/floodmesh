/** HMAC and constant-time comparison for the admin login and session cookie. */
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
