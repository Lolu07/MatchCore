import { useEffect, useRef, useState, type FormEvent } from "react";
import type { Book, OrderResult, OrderType, Side } from "../api";
import { api } from "../api";
import { explainOrder, type Explanation } from "../explain";
import { dollarsToTicks, formatTicks } from "../price";
import type { OrderTemplate } from "./Guide";

interface Props {
  book: Book | null;
  prefill: (OrderTemplate & { nonce: number }) | null;
  onOrder: (r: OrderResult) => void;
}

type Message = { ok: boolean; exp?: Explanation; error?: string };

const QTY_CHIPS = [10, 25, 50, 100];

export function OrderEntry({ book, prefill, onOrder }: Props) {
  const scale = book?.price_scale ?? 100;
  const [side, setSide] = useState<Side>("buy");
  const [type, setType] = useState<OrderType>("limit");
  const [price, setPrice] = useState("");
  const [qty, setQty] = useState("");
  const [busy, setBusy] = useState(false);
  const [msg, setMsg] = useState<Message | null>(null);
  const [pulse, setPulse] = useState(0);
  const priceRef = useRef<HTMLInputElement>(null);

  const toInput = (ticks: number) => formatTicks(ticks, scale).replace(/,/g, "");

  // The walkthrough and the book ladder fill the form; the visitor still submits.
  useEffect(() => {
    if (!prefill) return;
    setSide(prefill.side);
    setType(prefill.type);
    setPrice(prefill.price !== undefined ? toInput(prefill.price) : "");
    if (prefill.quantity) setQty(String(prefill.quantity));
    setMsg(null);
    setPulse((n) => n + 1);
    document.getElementById("order-entry")?.scrollIntoView({ block: "nearest", behavior: "smooth" });
  }, [prefill]);

  const ticks = type === "limit" ? dollarsToTicks(price.replace(/,/g, ""), scale) : null;
  const bestOpposite = side === "buy" ? book?.best_ask ?? null : book?.best_bid ?? null;

  function step(delta: number) {
    const base = ticks ?? (side === "buy" ? book?.best_bid : book?.best_ask) ?? 100 * scale;
    setPrice(toInput(Math.max(1, base + delta)));
    priceRef.current?.focus();
  }

  async function submit(e: FormEvent) {
    e.preventDefault();
    const quantity = Number(qty);
    if (!/^\d+$/.test(qty.trim()) || quantity <= 0) {
      setMsg({ ok: false, error: "Quantity must be a positive whole number of shares." });
      return;
    }
    if (type === "limit" && (ticks === null || ticks <= 0)) {
      setMsg({ ok: false, error: `Enter a price in dollars, in steps of $${formatTicks(1, scale)} (e.g. 100.25).` });
      return;
    }
    setBusy(true);
    try {
      const result = await api.submit({ side, type, price: ticks ?? undefined, quantity });
      setMsg({ ok: true, exp: explainOrder(result, scale) });
      onOrder(result);
    } catch (err) {
      setMsg({ ok: false, error: (err as Error).message });
    } finally {
      setBusy(false);
    }
  }

  // Price comparison only — the engine decides what actually fills.
  let crossNote: { text: string; crosses: boolean } | null = null;
  if (type === "limit" && ticks !== null && bestOpposite !== null) {
    const crosses = side === "buy" ? ticks >= bestOpposite : ticks <= bestOpposite;
    crossNote = crosses
      ? { crosses, text: `Crosses the spread: the best ${side === "buy" ? "seller asks" : "buyer bids"} $${formatTicks(bestOpposite, scale)}, so this trades immediately.` }
      : { crosses, text: `Doesn't reach the best ${side === "buy" ? "seller" : "buyer"} ($${formatTicks(bestOpposite, scale)}), so it will wait in the book.` };
  }

  return (
    <section className="panel" id="order-entry">
      <h2>Order Entry</h2>
      <p className="panel-desc">
        Send an order to the C++ engine. Tip: click any price in the order book to fill this in.
      </p>

      <form className={`form${pulse ? " prefilled" : ""}`} key={pulse} onSubmit={submit}>
        <div className="seg" role="group" aria-label="Side">
          <button type="button" className={side === "buy" ? "on buy" : ""} onClick={() => setSide("buy")}>Buy</button>
          <button type="button" className={side === "sell" ? "on sell" : ""} onClick={() => setSide("sell")}>Sell</button>
        </div>
        <div className="seg" role="group" aria-label="Order type">
          <button type="button" className={type === "limit" ? "on" : ""} onClick={() => setType("limit")}>Limit</button>
          <button type="button" className={type === "market" ? "on" : ""} onClick={() => setType("market")}>Market</button>
        </div>
        <p className="field-help span">
          {type === "limit"
            ? side === "buy"
              ? "Limit buy: pay at most your price. If no one sells that cheaply, the order waits in the book."
              : "Limit sell: receive at least your price. If no one buys that high, the order waits in the book."
            : side === "buy"
              ? "Market buy: buy right now from the cheapest sellers, whatever they ask. Never waits in the book."
              : "Market sell: sell right now to the highest bidders, whatever they pay. Never waits in the book."}
        </p>

        <label className="span">
          {side === "buy" ? "Highest price you'll pay ($)" : "Lowest price you'll accept ($)"}
          <div className="stepper">
            <button type="button" disabled={type === "market"} onClick={() => step(-1)} aria-label="Lower price by one tick">−</button>
            <input
              ref={priceRef}
              inputMode="decimal"
              value={type === "market" ? "" : price}
              disabled={type === "market"}
              placeholder={type === "market" ? "not needed — trades at the best available prices" : bestOpposite != null ? formatTicks(bestOpposite, scale) : "100.00"}
              onChange={(e) => setPrice(e.target.value)}
            />
            <button type="button" disabled={type === "market"} onClick={() => step(1)} aria-label="Raise price by one tick">+</button>
          </div>
        </label>
        {type === "limit" && book && (
          <div className="chips span" aria-label="Quick price">
            <span className="muted">Quick price:</span>
            {book.best_bid !== null && <button type="button" onClick={() => setPrice(toInput(book.best_bid!))}>Best bid {formatTicks(book.best_bid, scale)}</button>}
            {book.best_bid !== null && book.best_ask !== null && (
              <button type="button" onClick={() => setPrice(toInput(Math.round((book.best_bid! + book.best_ask!) / 2)))}>Mid</button>
            )}
            {book.best_ask !== null && <button type="button" onClick={() => setPrice(toInput(book.best_ask!))}>Best ask {formatTicks(book.best_ask, scale)}</button>}
          </div>
        )}
        {crossNote && (
          <p className={`field-help span cross-note ${crossNote.crosses ? "crosses" : ""}`}>
            {crossNote.crosses ? "⚡ " : "⏸ "}
            {crossNote.text}
          </p>
        )}

        <label className="span">
          Quantity (shares)
          <div className="qty-row">
            <input inputMode="numeric" value={qty} placeholder="10" onChange={(e) => setQty(e.target.value)} />
            <div className="chips">
              {QTY_CHIPS.map((q) => (
                <button key={q} type="button" className={qty === String(q) ? "on" : ""} onClick={() => setQty(String(q))}>{q}</button>
              ))}
            </div>
          </div>
        </label>

        <button type="submit" className={`primary ${side}`} disabled={busy}>
          {busy ? "Sending to engine…" : `${side === "buy" ? "Buy" : "Sell"} ${qty || "…"} ${type === "limit" && ticks ? `@ ${formatTicks(ticks, scale)}` : type === "market" ? "at market" : ""}`}
        </button>
      </form>

      {msg && (
        <div className={`result ${msg.ok ? "" : "error"}`} role="status">
          {msg.exp ? (
            <>
              <strong>{msg.exp.headline}</strong>
              {msg.exp.text && <p>{msg.exp.text}</p>}
              {msg.exp.fills.length > 0 && (
                <ul className="fills">
                  {msg.exp.fills.map((f, i) => (
                    <li key={i}>{f}</li>
                  ))}
                </ul>
              )}
            </>
          ) : (
            msg.error
          )}
        </div>
      )}
    </section>
  );
}
