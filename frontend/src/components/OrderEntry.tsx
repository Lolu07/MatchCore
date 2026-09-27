import { useState, type FormEvent } from "react";
import type { Book, CancelResult, OrderResult, OrderType, Side } from "../api";
import { api } from "../api";
import { dollarsToTicks, formatTicks } from "../price";

interface Props {
  book: Book | null;
  cancelId: string;
  setCancelId: (id: string) => void;
  onEngineUpdate: (book: Book) => void;
}

const STATUS_TEXT: Record<string, string> = {
  filled: "Filled",
  resting: "Resting in book",
  partially_filled_resting: "Partially filled — remainder resting",
  partially_filled_remainder_discarded: "Partially filled — market remainder discarded",
  unfilled_discarded: "No liquidity — market order discarded",
};

export function OrderEntry({ book, cancelId, setCancelId, onEngineUpdate }: Props) {
  const scale = book?.price_scale ?? 100;
  const [side, setSide] = useState<Side>("buy");
  const [type, setType] = useState<OrderType>("limit");
  const [price, setPrice] = useState("");
  const [qty, setQty] = useState("");
  const [busy, setBusy] = useState(false);
  const [orderMsg, setOrderMsg] = useState<{ ok: boolean; result?: OrderResult; text?: string } | null>(null);
  const [cancelMsg, setCancelMsg] = useState<{ ok: boolean; result?: CancelResult; text?: string } | null>(null);

  const resting = book
    ? [
        ...book.asks.flatMap((l) => l.orders.map((o) => ({ ...o, side: "sell" as const, price: l.price }))),
        ...book.bids.flatMap((l) => l.orders.map((o) => ({ ...o, side: "buy" as const, price: l.price }))),
      ].sort((a, b) => a.id - b.id)
    : [];

  async function submit(e: FormEvent) {
    e.preventDefault();
    const quantity = Number(qty);
    if (!/^\d+$/.test(qty.trim()) || quantity <= 0) {
      setOrderMsg({ ok: false, text: "Quantity must be a positive whole number" });
      return;
    }
    let ticks: number | undefined;
    if (type === "limit") {
      const t = dollarsToTicks(price, scale);
      if (t === null || t <= 0) {
        setOrderMsg({ ok: false, text: `Price must be a positive amount in ${formatTicks(1, scale)} increments` });
        return;
      }
      ticks = t;
    }
    setBusy(true);
    try {
      const result = await api.submit({ side, type, price: ticks, quantity });
      setOrderMsg({ ok: true, result });
      onEngineUpdate(result.book);
    } catch (err) {
      setOrderMsg({ ok: false, text: (err as Error).message });
    } finally {
      setBusy(false);
    }
  }

  async function cancel(e: FormEvent) {
    e.preventDefault();
    if (!/^\d+$/.test(cancelId.trim())) {
      setCancelMsg({ ok: false, text: "Enter a numeric order ID" });
      return;
    }
    setBusy(true);
    try {
      const result = await api.cancel(Number(cancelId));
      setCancelMsg({ ok: result.cancelled, result });
      onEngineUpdate(result.book);
    } catch (err) {
      setCancelMsg({ ok: false, text: (err as Error).message });
    } finally {
      setBusy(false);
    }
  }

  const r = orderMsg?.result;

  return (
    <section className="panel">
      <h2>Order Entry</h2>
      <p className="hint">
        Try a <strong>buy limit above the best ask</strong> (e.g. 50 @ 100.20) to cross the spread and sweep
        levels in price-time order. Click any book level to select its front order for cancel.
      </p>
      <form className="form" onSubmit={submit}>
        <div className="seg" role="group" aria-label="Side">
          <button type="button" className={side === "buy" ? "on buy" : ""} onClick={() => setSide("buy")}>Buy</button>
          <button type="button" className={side === "sell" ? "on sell" : ""} onClick={() => setSide("sell")}>Sell</button>
        </div>
        <div className="seg" role="group" aria-label="Order type">
          <button type="button" className={type === "limit" ? "on" : ""} onClick={() => setType("limit")}>Limit</button>
          <button type="button" className={type === "market" ? "on" : ""} onClick={() => setType("market")}>Market</button>
        </div>
        <label>
          Limit price ($)
          <input
            inputMode="decimal"
            value={type === "market" ? "" : price}
            disabled={type === "market"}
            placeholder={
              type === "market"
                ? "market — sweeps book"
                : (side === "buy" ? book?.best_ask : book?.best_bid) != null
                  ? formatTicks((side === "buy" ? book!.best_ask : book!.best_bid)!, scale)
                  : "100.00"
            }
            onChange={(e) => setPrice(e.target.value)}
          />
        </label>
        <label>
          Quantity
          <input inputMode="numeric" value={qty} placeholder="10" onChange={(e) => setQty(e.target.value)} />
        </label>
        <button type="submit" className={`primary ${side}`} disabled={busy}>
          {side === "buy" ? "Buy" : "Sell"} {type}
        </button>
      </form>

      {orderMsg && (
        <div className={`result ${orderMsg.ok ? "" : "error"}`} role="status">
          {r ? (
            <>
              <div>
                Order <strong>#{r.order_id}</strong> · {STATUS_TEXT[r.status] ?? r.status}
              </div>
              <div className="muted">
                filled {r.filled}/{r.quantity}
                {r.fills.length > 0 && ` in ${r.fills.length} fill${r.fills.length > 1 ? "s" : ""}: `}
                {r.fills.map((f) => `${f.quantity} @ ${formatTicks(f.price, scale)} vs #${f.maker_id}`).join(", ")}
              </div>
            </>
          ) : (
            orderMsg.text
          )}
        </div>
      )}

      <h3>Cancel order</h3>
      <form className="form cancel" onSubmit={cancel}>
        <label>
          Order ID
          <input inputMode="numeric" value={cancelId} placeholder="#" onChange={(e) => setCancelId(e.target.value)} />
        </label>
        <label>
          Resting orders
          <select value={resting.some((o) => String(o.id) === cancelId) ? cancelId : ""} onChange={(e) => setCancelId(e.target.value)}>
            <option value="">{resting.length ? "select…" : "none resting"}</option>
            {resting.map((o) => (
              <option key={o.id} value={o.id}>
                #{o.id} {o.side.toUpperCase()} {o.leaves} @ {formatTicks(o.price, scale)}
              </option>
            ))}
          </select>
        </label>
        <button type="submit" disabled={busy}>Cancel</button>
      </form>
      {cancelMsg && (
        <div className={`result ${cancelMsg.ok ? "" : "error"}`} role="status">
          {cancelMsg.result
            ? cancelMsg.result.cancelled
              ? `Order #${cancelMsg.result.order_id} cancelled`
              : `Order #${cancelMsg.result.order_id} not cancelled — ${cancelMsg.result.reason}`
            : cancelMsg.text}
        </div>
      )}
    </section>
  );
}
