import { useEffect, useRef } from "react";
import type { Source, Trade } from "../api";
import { PriceChart } from "../charts/PriceChart";
import { formatTicks } from "../price";

interface Props {
  trades: Trade[]; // newest first
  scale: number;
  mine: Set<number>;
}

function time(ms: number): string {
  const d = new Date(ms);
  return d.toLocaleTimeString("en-GB", { hour12: false }) + "." + String(d.getMilliseconds()).padStart(3, "0");
}

const SOURCE_LABEL: Record<Source, string> = { seed: "sample", sim: "sim", visitor: "visitor" };

export function Trades({ trades, scale, mine }: Props) {
  // New trades flash once; nothing flashes on first load.
  const seen = useRef<number | null>(null);
  const newestSeen = seen.current;
  useEffect(() => {
    if (trades.length) seen.current = Math.max(seen.current ?? 0, trades[0].seq);
  });

  const who = (id: number, src: Source | null) =>
    mine.has(id) ? (
      <span className="tag you">#{id} you</span>
    ) : (
      <span>
        #{id}
        {src && src !== "visitor" && <span className={`tag ${src}`}>{SOURCE_LABEL[src]}</span>}
      </span>
    );

  const last = trades[0];
  const prev = trades[1];
  const change = last && prev ? last.price - prev.price : 0;

  return (
    <section className="panel">
      <div className="panel-head">
        <h2>Recent Trades</h2>
        {last && (
          <div className="ticker" aria-live="polite">
            <span className="muted">Last</span>
            <strong>${formatTicks(last.price, scale)}</strong>
            {prev && (
              <span className={change > 0 ? "bid-text" : change < 0 ? "ask-text" : "muted"}>
                {change > 0 ? "▲ +" : change < 0 ? "▼ −" : "— "}
                {formatTicks(Math.abs(change), scale)}
              </span>
            )}
          </div>
        )}
      </div>
      <p className="panel-desc">
        Every trade the engine executed, newest first — from you, other visitors, and the simulated traders (tagged
        “sim”). One incoming order can produce several trades.
      </p>

      <PriceChart trades={trades} scale={scale} />

      <div className="scroll">
        <table className="grid">
          <thead>
            <tr>
              <th className="num">Seq</th>
              <th>Time</th>
              <th><span className="term" title="The side of the incoming order that caused the trade by crossing the spread.">Aggressor</span></th>
              <th className="num">Price</th>
              <th className="num">Qty</th>
              <th className="num"><span className="term" title="The order that was already waiting in the book. The trade happens at its price.">Maker</span></th>
              <th className="num"><span className="term" title="The incoming order that crossed the spread.">Taker</span></th>
            </tr>
          </thead>
          <tbody>
            {trades.length === 0 && (
              <tr><td colSpan={7} className="empty">No trades yet — cross the spread, or press “Simulate traders” above</td></tr>
            )}
            {trades.map((t) => (
              <tr key={t.seq} className={newestSeen !== null && t.seq > newestSeen ? "flash" : ""}>
                <td className="num muted">{t.seq}</td>
                <td className="muted">{time(t.time_ms)}</td>
                <td className={t.aggressor === "buy" ? "bid-text" : t.aggressor === "sell" ? "ask-text" : ""}>
                  {t.aggressor ? t.aggressor.toUpperCase() : "—"}
                </td>
                <td className="num">{formatTicks(t.price, scale)}</td>
                <td className="num">{t.quantity.toLocaleString()}</td>
                <td className="num muted">{who(t.maker_id, t.maker_source)}</td>
                <td className="num muted">{who(t.taker_id, t.taker_source)}</td>
              </tr>
            ))}
          </tbody>
        </table>
      </div>
      <p className="footnote">Seq is the engine's execution order. Every trade executes at the maker's (resting order's) price.</p>
    </section>
  );
}
