import type { Book, Level } from "../api";
import { formatMid, formatTicks } from "../price";

const LEVELS_SHOWN = 12;

interface Props {
  book: Book | null;
  onSelectOrder: (id: number) => void;
}

export function OrderBook({ book, onSelectOrder }: Props) {
  if (!book) return <section className="panel"><h2>Order Book</h2><p className="muted">Waiting for engine…</p></section>;

  const scale = book.price_scale;
  const asks = book.asks.slice(0, LEVELS_SHOWN);
  const bids = book.bids.slice(0, LEVELS_SHOWN);
  const maxQty = Math.max(1, ...asks.map((l) => l.quantity), ...bids.map((l) => l.quantity));

  const row = (lvl: Level, side: "bid" | "ask") => (
    <tr
      key={`${side}-${lvl.price}`}
      className={`ladder-row ${side}`}
      title={`FIFO queue: ${lvl.orders.map((o) => `#${o.id} × ${o.leaves}`).join(", ")}${lvl.order_count > lvl.orders.length ? ", …" : ""}\nClick to select the front order for cancel`}
      onClick={() => onSelectOrder(lvl.orders[0].id)}
    >
      <td className="num price">{formatTicks(lvl.price, scale)}</td>
      <td className="num depth-cell">
        <span className="depth-bar" style={{ width: `${(lvl.quantity / maxQty) * 100}%` }} />
        <span className="depth-val">{lvl.quantity.toLocaleString()}</span>
      </td>
      <td className="num muted">{lvl.order_count}</td>
    </tr>
  );

  return (
    <section className="panel">
      <h2>Order Book</h2>
      <dl className="quote">
        <div><dt>Best bid</dt><dd className="bid-text">{book.best_bid !== null ? formatTicks(book.best_bid, scale) : "—"}</dd></div>
        <div><dt>Best ask</dt><dd className="ask-text">{book.best_ask !== null ? formatTicks(book.best_ask, scale) : "—"}</dd></div>
        <div><dt>Spread</dt><dd>{book.spread !== null ? formatTicks(book.spread, scale) : "—"}</dd></div>
        <div><dt>Mid</dt><dd>{book.best_bid !== null && book.best_ask !== null ? formatMid(book.best_bid, book.best_ask, scale) : "—"}</dd></div>
      </dl>

      <table className="ladder">
        <thead>
          <tr><th className="num">Price</th><th className="num">Size</th><th className="num">Orders</th></tr>
        </thead>
        <tbody>
          {asks.length === 0 && <tr><td colSpan={3} className="empty">no asks</td></tr>}
          {/* Asks displayed highest→lowest so the best ask sits next to the spread. */}
          {[...asks].reverse().map((l) => row(l, "ask"))}
          <tr className="spread-row">
            <td colSpan={3}>
              {book.spread !== null ? `spread ${formatTicks(book.spread, scale)}` : "no two-sided market"}
            </td>
          </tr>
          {bids.map((l) => row(l, "bid"))}
          {bids.length === 0 && <tr><td colSpan={3} className="empty">no bids</td></tr>}
        </tbody>
      </table>
      <p className="footnote">
        {book.bid_levels} bid / {book.ask_levels} ask levels · {book.resting_orders} resting orders
        {(book.bid_levels > LEVELS_SHOWN || book.ask_levels > LEVELS_SHOWN) && ` · top ${LEVELS_SHOWN} shown`}
      </p>
    </section>
  );
}
