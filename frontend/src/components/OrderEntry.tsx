import { useEffect, useState, type FormEvent } from "react";
import type { Book, CancelResult, OrderResult, OrderType, Side } from "../api";
import { api } from "../api";
import { explainCancel, explainOrder, type Explanation } from "../explain";
import { dollarsToTicks, formatTicks } from "../price";
import type { OrderTemplate } from "./Guide";

interface Props {
  book: Book | null;
  mine: Set<number>;
  prefill: (OrderTemplate & { nonce: number }) | null;
  cancelId: string;
  setCancelId: (id: string) => void;
  onOrder: (r: OrderResult) => void;
  onCancel: (r: CancelResult) => void;
}

type Message = { ok: boolean; exp?: Explanation; error?: string };

function Result({ msg }: { msg: Message }) {
  return (
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
  );
}

export function OrderEntry({ book, mine, prefill, cancelId, setCancelId, onOrder, onCancel }: Props) {
  const scale = book?.price_scale ?? 100;
  const [side, setSide] = useState<Side>("buy");
  const [type, setType] = useState<OrderType>("limit");
  const [price, setPrice] = useState("");
  const [qty, setQty] = useState("");
  const [busy, setBusy] = useState(false);
  const [orderMsg, setOrderMsg] = useState<Message | null>(null);
  const [cancelMsg, setCancelMsg] = useState<Message | null>(null);

  // The walkthrough fills the form; the visitor still presses submit.
  useEffect(() => {
    if (!prefill) return;
    setSide(prefill.side);
    setType(prefill.type);
    setPrice(prefill.price !== undefined ? formatTicks(prefill.price, scale).replace(/,/g, "") : "");
    setQty(String(prefill.quantity));
    setOrderMsg(null);
    document.getElementById("order-entry")?.scrollIntoView({ block: "nearest", behavior: "smooth" });
  }, [prefill, scale]);

  const resting = book
    ? [
        ...book.asks.flatMap((l) => l.orders.map((o) => ({ ...o, side: "sell" as const, price: l.price }))),
        ...book.bids.flatMap((l) => l.orders.map((o) => ({ ...o, side: "buy" as const, price: l.price }))),
      ].sort((a, b) => Number(mine.has(b.id)) - Number(mine.has(a.id)) || a.id - b.id)
    : [];

  async function submit(e: FormEvent) {
    e.preventDefault();
    const quantity = Number(qty);
    if (!/^\d+$/.test(qty.trim()) || quantity <= 0) {
      setOrderMsg({ ok: false, error: "Quantity must be a positive whole number of shares." });
      return;
    }
    let ticks: number | undefined;
    if (type === "limit") {
      const t = dollarsToTicks(price.replace(/,/g, ""), scale);
      if (t === null || t <= 0) {
        setOrderMsg({ ok: false, error: `Enter a price in dollars, in steps of $${formatTicks(1, scale)} (e.g. 100.25).` });
        return;
      }
      ticks = t;
    }
    setBusy(true);
    try {
      const result = await api.submit({ side, type, price: ticks, quantity });
      setOrderMsg({ ok: true, exp: explainOrder(result, scale) });
      onOrder(result);
    } catch (err) {
      setOrderMsg({ ok: false, error: (err as Error).message });
    } finally {
      setBusy(false);
    }
  }

  async function cancel(e: FormEvent) {
    e.preventDefault();
    if (!/^\d+$/.test(cancelId.trim())) {
      setCancelMsg({ ok: false, error: "Enter an order number, or pick one from the list." });
      return;
    }
    setBusy(true);
    try {
      const result = await api.cancel(Number(cancelId));
      setCancelMsg({ ok: result.cancelled, exp: explainCancel(result) });
      onCancel(result);
    } catch (err) {
      setCancelMsg({ ok: false, error: (err as Error).message });
    } finally {
      setBusy(false);
    }
  }

  const bestOpposite = side === "buy" ? book?.best_ask : book?.best_bid;

  return (
    <section className="panel" id="order-entry">
      <h2>Order Entry</h2>
      <p className="panel-desc">Send an order to the C++ engine. The result below explains exactly what the engine did with it.</p>

      <form className="form" onSubmit={submit}>
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
              ? "Limit buy: pay at most your price. If no one is selling that cheaply, your order waits in the book."
              : "Limit sell: receive at least your price. If no one is buying that high, your order waits in the book."
            : side === "buy"
              ? "Market buy: buy right now from the cheapest sellers, whatever they ask. Never waits in the book."
              : "Market sell: sell right now to the highest bidders, whatever they pay. Never waits in the book."}
        </p>
        <label>
          {side === "buy" ? "Highest price you'll pay ($)" : "Lowest price you'll accept ($)"}
          <input
            inputMode="decimal"
            value={type === "market" ? "" : price}
            disabled={type === "market"}
            placeholder={type === "market" ? "not needed for market" : bestOpposite != null ? formatTicks(bestOpposite, scale) : "100.00"}
            onChange={(e) => setPrice(e.target.value)}
          />
        </label>
        <label>
          Quantity (shares)
          <input inputMode="numeric" value={qty} placeholder="10" onChange={(e) => setQty(e.target.value)} />
        </label>
        {type === "limit" && bestOpposite != null && (
          <p className="field-help span">
            {side === "buy"
              ? `Cheapest seller right now: $${formatTicks(bestOpposite, scale)}. At or above this, your buy trades immediately; below it, it waits.`
              : `Highest buyer right now: $${formatTicks(bestOpposite, scale)}. At or below this, your sell trades immediately; above it, it waits.`}
          </p>
        )}
        <button type="submit" className={`primary ${side}`} disabled={busy}>
          {side === "buy" ? "Buy" : "Sell"} {type}
        </button>
      </form>

      {orderMsg && <Result msg={orderMsg} />}

      <h3>Cancel an order</h3>
      <p className="panel-desc">
        Only orders still waiting in the book can be cancelled. Pick one from the list, or click a price level in the book.
      </p>
      <form className="form cancel" onSubmit={cancel}>
        <label>
          Order #
          <input inputMode="numeric" value={cancelId} placeholder="e.g. 25" onChange={(e) => setCancelId(e.target.value)} />
        </label>
        <label>
          Orders in the book
          <select value={resting.some((o) => String(o.id) === cancelId) ? cancelId : ""} onChange={(e) => setCancelId(e.target.value)}>
            <option value="">{resting.length ? "select…" : "none resting"}</option>
            {resting.map((o) => (
              <option key={o.id} value={o.id}>
                #{o.id} {o.side.toUpperCase()} {o.leaves} @ {formatTicks(o.price, scale)}
                {mine.has(o.id) ? "  (yours)" : ""}
              </option>
            ))}
          </select>
        </label>
        <button type="submit" disabled={busy}>Cancel</button>
      </form>
      {cancelMsg && <Result msg={cancelMsg} />}
    </section>
  );
}
