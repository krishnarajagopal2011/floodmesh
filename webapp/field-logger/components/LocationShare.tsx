"use client";
/**
 * "Share my location": the phone's GPS stands in for the unit's position
 * (units have no GPS), which gives the distances in the analysis.
 *
 * Nothing is sent before the volunteer reads what is recorded and agrees.
 * The agreement is remembered for one unit and one login only (see
 * readConsent), so a phone that is passed on, or logged in as another unit,
 * shows the consent text again.
 * Positions come from watchPosition and are posted at most every 20 s,
 * always the newest fix. Browsers stop geolocation in background pages, so
 * sharing lasts only while this page is open and on screen; a Screen Wake
 * Lock keeps the screen on where the browser supports it.
 */
import { useCallback, useEffect, useRef, useState } from "react";
import type { LocationView } from "@/lib/queries";
import { age, clock } from "./format";

const SEND_EVERY_MS = 20_000;
// watchPosition may go quiet while the phone does not move; ask for a fresh
// fix so a stationary unit still has a position near every reading.
const STILL_REFRESH_MS = 45_000;
// Stored per unit, holding the login it was given in. Before, one browser-
// wide flag let any later login on the phone skip the consent text.
const CONSENT_PREFIX = "fm-location-consent";
const consentKey = (unit: string) => `${CONSENT_PREFIX}:${unit}`;

type Phase = "idle" | "consent" | "sharing" | "denied" | "unsupported";

interface Sent {
  at: number;
  accuracy: number | null;
}

/**
 * True only if the volunteer agreed for this unit during this login.
 * `loginId` changes with every login, so consent never outlives the login
 * it was given in, even when the session simply expired without a logout.
 */
function readConsent(unit: string, loginId: string): boolean {
  try {
    localStorage.removeItem(CONSENT_PREFIX); // the old browser-wide flag
    return localStorage.getItem(consentKey(unit)) === loginId;
  } catch {
    return false;
  }
}

function writeConsent(unit: string, loginId: string) {
  try {
    localStorage.setItem(consentKey(unit), loginId);
  } catch {
    /* private mode: consent is simply asked again next time */
  }
}

/** Forget every unit's location consent on this browser (called on logout). */
export function forgetLocationConsent() {
  try {
    for (let i = localStorage.length - 1; i >= 0; i--) {
      const k = localStorage.key(i);
      if (k && (k === CONSENT_PREFIX || k.startsWith(`${CONSENT_PREFIX}:`))) localStorage.removeItem(k);
    }
  } catch {
    /* nothing stored, nothing to forget */
  }
}

export default function LocationShare({
  unit,
  loginId,
  isAdmin,
  lastStored,
  now,
}: {
  unit: string;
  /** Identifies the current login (see readConsent). */
  loginId: string;
  isAdmin: boolean;
  lastStored: LocationView | null;
  now: number;
}) {
  const [phase, setPhase] = useState<Phase>("idle");
  const [agreedBefore, setAgreedBefore] = useState(false);
  const [lastSent, setLastSent] = useState<Sent | null>(null);
  const [lastFix, setLastFix] = useState<Sent | null>(null);
  const [problem, setProblem] = useState<string | null>(null);
  const [wakeLockOn, setWakeLockOn] = useState<boolean | null>(null);

  const watchId = useRef<number | null>(null);
  const pending = useRef<GeolocationPosition | null>(null);
  const sendTimer = useRef<ReturnType<typeof setTimeout> | null>(null);
  const stillTimer = useRef<ReturnType<typeof setInterval> | null>(null);
  const lastSendAt = useRef(0);
  const lastFixAt = useRef(0);
  const wakeLock = useRef<WakeLockSentinel | null>(null);
  const sharing = useRef(false);

  useEffect(() => {
    setAgreedBefore(readConsent(unit, loginId));
    if (typeof navigator !== "undefined" && !("geolocation" in navigator)) setPhase("unsupported");
  }, [unit, loginId]);

  const acquireWakeLock = useCallback(async () => {
    if (!("wakeLock" in navigator)) {
      setWakeLockOn(false);
      return;
    }
    try {
      wakeLock.current = await navigator.wakeLock.request("screen");
      setWakeLockOn(true);
      wakeLock.current.addEventListener("release", () => setWakeLockOn(false));
    } catch {
      setWakeLockOn(false);
    }
  }, []);

  const stop = useCallback(() => {
    sharing.current = false;
    if (watchId.current !== null) navigator.geolocation.clearWatch(watchId.current);
    watchId.current = null;
    if (sendTimer.current) clearTimeout(sendTimer.current);
    sendTimer.current = null;
    if (stillTimer.current) clearInterval(stillTimer.current);
    stillTimer.current = null;
    pending.current = null;
    wakeLock.current?.release().catch(() => {});
    wakeLock.current = null;
    setWakeLockOn(null);
  }, []);

  const flush = useCallback(async () => {
    sendTimer.current = null;
    const pos = pending.current;
    if (!pos || !sharing.current) return;
    pending.current = null;
    lastSendAt.current = Date.now();
    try {
      const res = await fetch("/api/location", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({
          lat: pos.coords.latitude,
          lon: pos.coords.longitude,
          accuracy: pos.coords.accuracy,
          altitude: pos.coords.altitude,
          ts: pos.timestamp,
        }),
      });
      if (res.status === 401) {
        stop();
        setPhase("idle");
        setProblem("Your login has expired, so sharing stopped. Log in again.");
        return;
      }
      if (!res.ok) throw new Error(`HTTP ${res.status}`);
      setLastSent({ at: Date.now(), accuracy: pos.coords.accuracy });
      setProblem(null);
    } catch (err) {
      setProblem(`Could not send the position (${(err as Error).message}). Will try again.`);
      // Keep it for the next window unless a newer fix has arrived.
      pending.current ??= pos;
      if (sharing.current && !sendTimer.current) sendTimer.current = setTimeout(flush, SEND_EVERY_MS);
    }
  }, [stop]);

  const onPosition = useCallback(
    (pos: GeolocationPosition) => {
      if (!sharing.current) return;
      lastFixAt.current = Date.now();
      setLastFix({ at: Date.now(), accuracy: pos.coords.accuracy });
      pending.current = pos;
      if (!sendTimer.current) {
        const wait = Math.max(0, lastSendAt.current + SEND_EVERY_MS - Date.now());
        sendTimer.current = setTimeout(flush, wait);
      }
    },
    [flush],
  );

  const onGeoError = useCallback(
    (err: GeolocationPositionError) => {
      if (err.code === err.PERMISSION_DENIED) {
        stop();
        setPhase("denied");
      } else {
        setProblem(`No GPS fix yet (${err.message || "timeout"}). Stay outdoors with a clear view of the sky.`);
      }
    },
    [stop],
  );

  const start = useCallback(() => {
    writeConsent(unit, loginId);
    setAgreedBefore(true);
    setProblem(null);
    sharing.current = true;
    setPhase("sharing");
    const opts: PositionOptions = { enableHighAccuracy: true, maximumAge: 10_000, timeout: 60_000 };
    watchId.current = navigator.geolocation.watchPosition(onPosition, onGeoError, opts);
    stillTimer.current = setInterval(() => {
      if (Date.now() - lastFixAt.current > STILL_REFRESH_MS) {
        navigator.geolocation.getCurrentPosition(onPosition, onGeoError, { ...opts, maximumAge: 0 });
      }
    }, 15_000);
    acquireWakeLock();
  }, [unit, loginId, onPosition, onGeoError, acquireWakeLock]);

  // The wake lock is dropped whenever the page is hidden; take it again on return.
  useEffect(() => {
    const onVisible = () => {
      if (document.visibilityState === "visible" && sharing.current) acquireWakeLock();
    };
    document.addEventListener("visibilitychange", onVisible);
    return () => document.removeEventListener("visibilitychange", onVisible);
  }, [acquireWakeLock]);

  useEffect(() => stop, [stop]);

  if (isAdmin) {
    return (
      <section className="card">
        <h2>Phone location</h2>
        <p className="muted">Location sharing is done from the volunteer&apos;s own login for unit {unit}.</p>
        <StoredLine lastStored={lastStored} now={now} />
      </section>
    );
  }

  return (
    <section className={`card ${phase === "sharing" ? "ok" : phase === "denied" ? "bad" : ""}`}>
      <h2>Share my location</h2>

      {phase === "unsupported" && <p className="badText">This browser cannot share its location.</p>}

      {phase === "idle" && (
        <>
          <p>
            Your phone&apos;s position tells us how far apart the units were when they heard each other. Units
            have no GPS of their own.
          </p>
          <div className="row">
            <button type="button" onClick={() => (agreedBefore ? start() : setPhase("consent"))}>
              {agreedBefore ? "Start sharing again" : "Share my location"}
            </button>
            {agreedBefore && (
              <button type="button" className="secondary" onClick={() => setPhase("consent")}>
                What is recorded?
              </button>
            )}
          </div>
        </>
      )}

      {phase === "consent" && (
        <div className="card warn" style={{ margin: "8px 0 0" }}>
          <p>
            <strong>What is recorded:</strong> your phone&apos;s latitude, longitude, altitude and GPS accuracy,
            with the time, at most once every 20 seconds, stored against <strong>unit {unit}</strong>.
          </p>
          <p>
            <strong>Why:</strong> to work out the distance between units for each message received in this field
            test. It is not used for anything else.
          </p>
          <p>
            <strong>Who sees it:</strong> the test organisers. Other volunteers do not.
          </p>
          <p>
            <strong>When it stops:</strong> when you press Stop, close this page, or lock the phone. Nothing is
            sent in the background. Keep this page open with the screen on while you want to share.
          </p>
          <div className="row">
            <button type="button" onClick={start}>
              I agree, start sharing
            </button>
            <button type="button" className="secondary" onClick={() => setPhase("idle")}>
              Not now
            </button>
          </div>
        </div>
      )}

      {phase === "sharing" && (
        <>
          <div className="big okText">Sharing</div>
          <p>
            Last sent:{" "}
            <strong>
              {lastSent ? `${clock(new Date(lastSent.at).toISOString())} (±${Math.round(lastSent.accuracy ?? 0)} m)` : "waiting for GPS…"}
            </strong>
          </p>
          {lastFix && !lastSent && <p className="muted">GPS fix ±{Math.round(lastFix.accuracy ?? 0)} m, sending shortly.</p>}
          <p className="muted">
            Keep this page open and the screen on. Sharing stops if you close the page or lock the phone.
            {wakeLockOn === true && " The screen will stay on while sharing."}
            {wakeLockOn === false && " This browser cannot keep the screen on; turn off auto-lock if you can."}
          </p>
          <div className="row">
            <button
              type="button"
              className="danger"
              onClick={() => {
                stop();
                setPhase("idle");
              }}
            >
              Stop sharing
            </button>
          </div>
        </>
      )}

      {phase === "denied" && (
        <>
          <p className="badText">Location permission is blocked for this site.</p>
          <p>
            Allow location for this page in the browser&apos;s site settings (the icon left of the address), then
            press the button again.
          </p>
          <div className="row">
            <button type="button" onClick={() => setPhase("consent")}>
              Try again
            </button>
          </div>
        </>
      )}

      {problem && <p className="badText">{problem}</p>}
      <StoredLine lastStored={lastStored} now={now} />
    </section>
  );
}

function StoredLine({ lastStored, now }: { lastStored: LocationView | null; now: number }) {
  if (!lastStored?.ts) return <p className="muted">No position stored yet.</p>;
  return (
    <p className="muted">
      Last position on the server: {clock(lastStored.ts)} ({age(lastStored.ts, now)})
      {lastStored.accuracy !== null ? `, ±${Math.round(lastStored.accuracy)} m` : ""}
    </p>
  );
}
