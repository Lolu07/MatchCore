// Dollar <-> tick conversion. This is the only place the UI deals with
// dollars; everything sent to or received from the engine is integer ticks.
// String/integer arithmetic only — no floating point touches a price.

function decimalsOf(scale: number): number {
  return Math.round(Math.log10(scale)); // scale is a power of ten (100 → 2)
}

/** "100.5" → 10050 at scale 100. Returns null if not representable in whole ticks. */
export function dollarsToTicks(input: string, scale: number): number | null {
  const decimals = decimalsOf(scale);
  const m = /^(\d+)(?:\.(\d*))?$/.exec(input.trim());
  if (!m) return null;
  const frac = m[2] ?? "";
  if (frac.length > decimals) return null; // finer than one tick
  const ticks = Number(m[1]) * scale + Number(frac.padEnd(decimals, "0") || "0");
  return Number.isSafeInteger(ticks) ? ticks : null;
}

/** 10050 → "100.50" at scale 100. */
export function formatTicks(ticks: number, scale: number): string {
  const decimals = decimalsOf(scale);
  const sign = ticks < 0 ? "-" : "";
  const abs = Math.abs(ticks);
  const whole = Math.floor(abs / scale).toLocaleString("en-US");
  if (decimals === 0) return sign + whole;
  return `${sign}${whole}.${String(abs % scale).padStart(decimals, "0")}`;
}

/** Midpoint of two tick prices; shows a half tick exactly (e.g. "100.005"). */
export function formatMid(bid: number, ask: number, scale: number): string {
  const sum = bid + ask;
  return sum % 2 === 0 ? formatTicks(sum / 2, scale) : formatTicks(sum * 5, scale * 10);
}
