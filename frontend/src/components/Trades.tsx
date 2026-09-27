import type { Trade } from "../api";
import { formatTicks } from "../price";

interface Props {
  trades: Trade[];
  scale: number;
}

function time(ms: number): string {
  const d = new Date(ms);
  return d.toLocaleTimeString("en-GB", { hour12: false }) + "." + String(d.getMilliseconds()).padStart(3, "0");
}

export function Trades({ trades, scale }: Props) {
  return (
    <section className="panel">
      <h2>Recent Trades</h2>
      <div className="scroll">
        <table className="grid">
          <thead>
            <tr>
              <th className="num">Seq</th>
              <th>Time</th>
              <th>Aggressor</th>
              <th className="num">Price</th>
              <th className="num">Qty</th>
              <th className="num">Maker</th>
              <th className="num">Taker</th>
            </tr>
          </thead>
          <tbody>
            {trades.length === 0 && (
              <tr><td colSpan={7} className="empty">No trades yet — submit an order that crosses the spread</td></tr>
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
                <td className="num muted">#{t.maker_id}</td>
                <td className="num muted">#{t.taker_id}</td>
              </tr>
            ))}
          </tbody>
        </table>
      </div>
      <p className="footnote">Executions at the maker's (resting order's) price. Time is server receipt time.</p>
    </section>
  );
}
