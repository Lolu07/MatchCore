import { useEffect, useRef, useState } from "react";
import type { Book, Level, Side } from "../api";
import { DepthChart } from "../charts/DepthChart";
import { formatMid, formatTicks } from "../price";

const LEVELS_SHOWN = 10;

interface Props {
  book: Book | null;
  mine: Set<number>;
  onTradeAt: (side: Side, price: number) => void;
  onCancel: (id: number) => void;
}

export function OrderBook({ book, mine, onTradeAt, onCancel }: Props) {
  const [view, setView] = useState<"ladder" | "depth">("ladder");

  // Row keys include the quantity, so a level whose size changed re-mounts and
  // plays the flash. Keys seen in the previous render don't flash; nothing
  // flashes on first load.
  const prevKeys = useRef<Set<string> | null>(null);
  const keys = new Set<string>();

  useEffect(() => {
    if (book && view === "ladder") prevKeys.current = keys;
  });

  if (!book) {
    return (
      <section className="panel">
        <h2>Order Book</h2>
        <p className="muted">Connecting to the engine… (a sleeping free server can take ~30 s to wake up)</p>
      </section>
    );
  }

  const scale = book.price_scale;
  const asks = book.asks.slice(0, LEVELS_SHOWN);
  const bids = book.bids.slice(0, LEVELS_SHOWN);
  const maxQty = Math.max(1, ...asks.map((l) => l.quantity), ...bids.map((l) => l.quantity));

  const row = (lvl: Level, side: "bid" | "ask") => {
    const key = `${side}-${lvl.price}-${lvl.quantity}`;
    keys.add(key);
    const fresh = prevKeys.current !== null && !prevKeys.current.has(key);
    const own = lvl.orders.find((o) => mine.has(o.id));
    // Clicking a level prepares the order that would trade against it.
    const tradeSide: Side = side === "ask" ? "buy" : "sell";
    const act = () => onTradeAt(tradeSide, lvl.price);
    return (
      <tr
        key={key}
        className={`ladder-row ${side}${fresh ? " flash" : ""}`}
        tabIndex={0}
        role="button"
        aria-label={`${lvl.quantity} shares ${side === "ask" ? "offered" : "bid"} at ${formatTicks(lvl.price, scale)}. Press to prepare a ${tradeSide} at this price.`}
        title={
          `${lvl.order_count} ${side === "bid" ? "buy" : "sell"} order(s) at $${formatTicks(lvl.price, scale)}, first in line first:\n` +
          lvl.orders.map((o) => `#${o.id} × ${o.leaves}${mine.has(o.id) ? " (yours)" : ""}`).join(", ") +
          (lvl.order_count > lvl.orders.length ? ", …" : "") +
          `\nClick to prepare a ${tradeSide.toUpperCase()} at this price.`
        }
        onClick={act}
        onKeyDown={(e) => (e.key === "Enter" || e.key === " ") && (e.preventDefault(), act())}
      >
        <td className="num price">
          {own && <span className="yours">yours</span>}
          {formatTicks(lvl.price, scale)}
        </td>
        <td className="num depth-cell">
          <span className="depth-bar" style={{ width: `${(lvl.quantity / maxQty) * 100}%` }} />
          <span className="depth-val">{lvl.quantity.toLocaleString()}</span>
        </td>
        <td className="num muted">
          {lvl.order_count}
          {own && (
            <button
              type="button"
              className="row-x"
              title={`Cancel your order #${own.id}`}
              aria-label={`Cancel your order #${own.id}`}
              onClick={(e) => {
                e.stopPropagation();
                onCancel(own.id);
              }}
            >
              ×
            </button>
          )}
        </td>
      </tr>
    );
  };

  return (
    <section className="panel">
      <div className="panel-head">
        <h2>Order Book</h2>
        <div className="seg compact" role="tablist" aria-label="Book view">
          <button type="button" role="tab" aria-selected={view === "ladder"} className={view === "ladder" ? "on" : ""} onClick={() => setView("ladder")}>Ladder</button>
          <button type="button" role="tab" aria-selected={view === "depth"} className={view === "depth" ? "on" : ""} onClick={() => setView("depth")}>Depth chart</button>
        </div>
      </div>
      <p className="panel-desc">
        {view === "ladder" ? (
          <>
            Every order waiting to trade. <span className="ask-text">Sellers (asks)</span> on top, cheapest nearest the
            middle; <span className="bid-text">buyers (bids)</span> below, highest first.{" "}
            <strong>Click a price to trade at it</strong> — a sell level prepares a buy, a buy level prepares a sell.
          </>
        ) : (
          <>
            The same book as a curve: how many shares you could trade by sweeping up to each price. A steep wall means
            lots of liquidity close to the spread. Hover to read it.
          </>
        )}
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

      {view === "depth" ? (
        <DepthChart book={book} />
      ) : (
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
      )}
      <p className="footnote">
        {book.bid_levels} bid / {book.ask_levels} ask price levels · {book.resting_orders} orders waiting
        {view === "ladder" && (book.bid_levels > LEVELS_SHOWN || book.ask_levels > LEVELS_SHOWN) && ` · best ${LEVELS_SHOWN} levels shown`}
      </p>
    </section>
  );
}
