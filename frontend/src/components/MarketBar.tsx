import { useEffect, useRef, useState } from "react";
import type { SimulateResult } from "../api";

interface Props {
  onBurst: () => Promise<SimulateResult>;
}

const AUTO_EVERY_MS = 3000;
const AUTO_MAX_MS = 60_000; // auto-play stops itself so an idle tab can't load the shared server

export function MarketBar({ onBurst }: Props) {
  const [busy, setBusy] = useState(false);
  const [auto, setAuto] = useState(false);
  const [remaining, setRemaining] = useState(0);
  const [last, setLast] = useState<SimulateResult | null>(null);
  const [error, setError] = useState<string | null>(null);
  const burstRef = useRef(onBurst);
  burstRef.current = onBurst;

  async function burst() {
    setBusy(true);
    setError(null);
    try {
      setLast(await burstRef.current());
    } catch (err) {
      setError((err as Error).message);
      setAuto(false);
    } finally {
      setBusy(false);
    }
  }

  useEffect(() => {
    if (!auto) return;
    const started = Date.now();
    burst();
    const id = setInterval(() => {
      const left = AUTO_MAX_MS - (Date.now() - started);
      setRemaining(Math.max(0, Math.ceil(left / 1000)));
      if (left <= 0) return setAuto(false);
      if (!document.hidden) burst();
    }, AUTO_EVERY_MS);
    setRemaining(AUTO_MAX_MS / 1000);
    return () => clearInterval(id);
  }, [auto]);

  return (
    <div className="marketbar" role="region" aria-label="Market simulation">
      <div className="marketbar-text">
        <strong>Market activity</strong>
        <span className="muted">
          Simulated traders are random orders the server sends through the same C++ engine API you use — watch the book,
          trades and price react. Their trades are tagged “sim”.
        </span>
      </div>
      <div className="marketbar-actions">
        <button type="button" disabled={busy || auto} onClick={burst}>
          Simulate traders
        </button>
        <button type="button" className={auto ? "on" : ""} aria-pressed={auto} onClick={() => setAuto((a) => !a)}>
          {auto ? `■ Stop auto-play (${remaining}s)` : "▶ Auto-play"}
        </button>
      </div>
      <div className="marketbar-status muted" aria-live="polite">
        {error
          ? <span className="ask-text">{error}</span>
          : last
            ? `Last burst: ${last.submitted} orders${last.cancelled ? `, ${last.cancelled} cancels` : ""} → ${last.trades} trade${last.trades === 1 ? "" : "s"}`
            : "No simulated activity yet from this tab."}
      </div>
    </div>
  );
}
