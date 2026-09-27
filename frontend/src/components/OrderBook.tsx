import type { Book, Level } from "../api";
import { formatMid, formatTicks } from "../price";

const LEVELS_SHOWN = 12;

interface Props {
  book: Book | null;
  mine: Set<number>;
  onSelectOrder: (id: number) => void;
}

export function OrderBook({ book, mine, onSelectOrder }: Props) {
  if (!book) return <section className="panel"><h2>Order Book</h2><p className="muted">Connecting to the engine…</p></section>;

  const scale = book.price_scale;
  const asks = book.asks.slice(0, LEVELS_SHOWN);
  const bids = book.bids.slice(0, LEVELS_SHOWN);
  const maxQty = Math.max(1, ...asks.map((l) => l.quantity), ...bids.map((l) => l.quantity));

  const row = (lvl: Level, side: "bid" | "ask") => {
    // Prefer the visitor's own order at this level when clicking to cancel.
    const own = lvl.orders.find((o) => mine.has(o.id));
    return (
      <tr
        key={`${side}-${lvl.price}`}
        className={`ladder-row ${side}`}
        title={
          `${lvl.order_count} ${side === "bid" ? "buy" : "sell"} order(s) waiting at $${formatTicks(lvl.price, scale)}, first in line first:\n` +
          lvl.orders.map((o) => `#${o.id} × ${o.leaves}${mine.has(o.id) ? " (yours)" : ""}`).join(", ") +
          (lvl.order_count > lvl.orders.length ? ", …" : "") +
          `\nClick to select ${own ? "your" : "the front"} order for cancel.`
        }
        onClick={() => onSelectOrder((own ?? lvl.orders[0]).id)}
      >
        <td className="num price">
          {own && <span className="yours">yours</span>}
          {formatTicks(lvl.price, scale)}
        </td>
        <td className="num depth-cell">
          <span className="depth-bar" style={{ width: `${(lvl.quantity / maxQty) * 100}%` }} />
          <span className="depth-val">{lvl.quantity.toLocaleString()}</span>
        </td>
        <td className="num muted">{lvl.order_count}</td>
      </tr>
    );
  };

  return (
    <section className="panel">
      <h2>Order Book</h2>
      <p className="panel-desc">
        Every order waiting to trade. <span className="ask-text">Sellers (asks)</span> are on top, cheapest nearest the
        middle; <span className="bid-text">buyers (bids)</span> below, highest first. A trade happens only when a buyer
        will pay what a seller asks. Hover a row to see its queue.
      </p>
      <dl className="quote">
        <div title="The highest price any buyer is currently willing to pay.">
          <dt className="term">Best bid</dt><dd className="bid-text">{book.best_bid !== null ? formatTicks(book.best_bid, scale) : "—"}</dd>
        </div>
        <div title="The lowest price any seller is currently willing to accept.">
          <dt className="term">Best ask</dt><dd className="ask-text">{book.best_ask !== null ? formatTicks(book.best_ask, scale) : "—"}</dd>
        </div>
        <div title="Best ask minus best bid: the gap buyers and sellers haven't agreed to cross yet.">
          <dt className="term">Spread</dt><dd>{book.spread !== null ? formatTicks(book.spread, scale) : "—"}</dd>
        </div>
        <div title="Halfway between the best bid and best ask — a common estimate of the current price.">
          <dt className="term">Mid</dt><dd>{book.best_bid !== null && book.best_ask !== null ? formatMid(book.best_bid, book.best_ask, scale) : "—"}</dd>
        </div>
      </dl>

      <table className="ladder">
        <thead>
          <tr>
            <th className="num"><span className="term" title="Price per share, in dollars.">Price</span></th>
            <th className="num"><span className="term" title="Total shares waiting at this price. The bar compares sizes across levels.">Size</span></th>
            <th className="num"><span className="term" title="How many separate orders make up this level. They trade in arrival order.">Orders</span></th>
          </tr>
        </thead>
        <tbody>
          {asks.length === 0 && <tr><td colSpan={3} className="empty">no sellers</td></tr>}
          {/* Asks displayed highest→lowest so the best ask sits next to the spread. */}
          {[...asks].reverse().map((l) => row(l, "ask"))}
          <tr className="spread-row">
            <td colSpan={3}>
              {book.spread !== null ? `▲ sellers · spread $${formatTicks(book.spread, scale)} · buyers ▼` : "no two-sided market"}
            </td>
          </tr>
          {bids.map((l) => row(l, "bid"))}
          {bids.length === 0 && <tr><td colSpan={3} className="empty">no buyers</td></tr>}
        </tbody>
      </table>
      <p className="footnote">
        {book.bid_levels} bid / {book.ask_levels} ask price levels · {book.resting_orders} orders waiting
        {(book.bid_levels > LEVELS_SHOWN || book.ask_levels > LEVELS_SHOWN) && ` · best ${LEVELS_SHOWN} levels shown`}
      </p>
    </section>
  );
}
