import type { Trade } from "../api";
import { formatTicks } from "../price";

interface Props {
  trades: Trade[];
  scale: number;
  mine: Set<number>;
}

function time(ms: number): string {
  const d = new Date(ms);
  return d.toLocaleTimeString("en-GB", { hour12: false }) + "." + String(d.getMilliseconds()).padStart(3, "0");
}

export function Trades({ trades, scale, mine }: Props) {
  const who = (id: number) => (mine.has(id) ? <strong className="you">#{id} you</strong> : `#${id}`);
  return (
    <section className="panel">
      <h2>Recent Trades</h2>
      <p className="panel-desc">
        Every trade the engine has executed, newest first. One incoming order can create several trades if it takes
        liquidity from more than one resting order. Trades involving your orders are marked “you”.
      </p>
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
              <tr><td colSpan={7} className="empty">No trades yet — follow step 2 of the walkthrough to make one</td></tr>
            )}
            {trades.map((t) => (
              <tr key={t.seq}>
                <td className="num muted">{t.seq}</td>
                <td className="muted">{time(t.time_ms)}</td>
                <td className={t.aggressor === "buy" ? "bid-text" : t.aggressor === "sell" ? "ask-text" : ""}>
                  {t.aggressor ? t.aggressor.toUpperCase() : "—"}
                </td>
                <td className="num">{formatTicks(t.price, scale)}</td>
                <td className="num">{t.quantity.toLocaleString()}</td>
                <td className="num muted">{who(t.maker_id)}</td>
                <td className="num muted">{who(t.taker_id)}</td>
              </tr>
            ))}
          </tbody>
        </table>
      </div>
      <p className="footnote">Seq is the engine's execution order. Time is when the server recorded the trade.</p>
    </section>
  );
}
