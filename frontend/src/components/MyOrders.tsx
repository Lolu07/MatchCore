import { useState, type FormEvent } from "react";
import type { Explanation } from "../explain";
import type { MyOrder } from "../orders";
import { formatTicks } from "../price";

interface Props {
  orders: MyOrder[]; // newest first
  scale: number;
  highlightId: number | null;
  message: { ok: boolean; exp?: Explanation; error?: string } | null;
  onCancel: (ids: number[]) => Promise<void>;
}

const STATUS: Record<MyOrder["status"], string> = {
  resting: "waiting",
  filled: "traded",
  cancelled: "cancelled",
  dropped: "market remainder dropped",
};

function ordinal(n: number): string {
  const s = ["th", "st", "nd", "rd"], v = n % 100;
  return n + (s[(v - 20) % 10] || s[v] || s[0]);
}

export function MyOrders({ orders, scale, highlightId, message, onCancel }: Props) {
  const [busy, setBusy] = useState(false);
  const [inputError, setInputError] = useState<string | null>(null);
  const [manualId, setManualId] = useState("");
  const msg = inputError ? { ok: false, error: inputError } : message;

  const resting = orders.filter((o) => o.status === "resting");

  async function cancel(ids: number[]) {
    setInputError(null);
    setBusy(true);
    await onCancel(ids);
    setBusy(false);
  }

  function cancelManual(e: FormEvent) {
    e.preventDefault();
    if (!/^\d+$/.test(manualId.trim())) {
      setInputError("Enter an order number.");
      return;
    }
    cancel([Number(manualId)]);
  }

  return (
    <section className="panel" id="my-orders">
      <div className="panel-head">
        <h2>My Orders</h2>
        {resting.length > 1 && (
          <button type="button" className="small" disabled={busy} onClick={() => cancel(resting.map((o) => o.id))}>
            Cancel all waiting ({resting.length})
          </button>
        )}
      </div>
      <p className="panel-desc">
        Orders you placed from this tab. Status updates live: a waiting order can be traded by any visitor or simulated
        trader at any moment.
      </p>

      {orders.length === 0 ? (
        <p className="empty">You haven't placed any orders yet.</p>
      ) : (
        <div className="scroll short">
          <table className="grid">
            <thead>
              <tr>
                <th className="num">#</th>
                <th>Order</th>
                <th className="num"><span className="term" title="Shares traded so far / shares ordered.">Filled</span></th>
                <th>Status</th>
                <th />
              </tr>
            </thead>
            <tbody>
              {orders.map((o) => (
                <tr key={o.id} className={o.id === highlightId ? "highlight" : ""}>
                  <td className="num muted">{o.id}</td>
                  <td>
                    <span className={o.side === "buy" ? "bid-text" : "ask-text"}>{o.side.toUpperCase()}</span>{" "}
                    {o.type === "limit" && o.price !== null ? `@ ${formatTicks(o.price, scale)}` : "market"}
                  </td>
                  <td className="num">
                    {o.filled}/{o.quantity}
                  </td>
                  <td>
                    <span className={`pill ${o.status}`}>{STATUS[o.status]}</span>
                    {o.status === "resting" && o.queuePos !== null && (
                      <span className="muted small-text"> · {ordinal(o.queuePos + 1)} in line</span>
                    )}
                  </td>
                  <td className="num">
                    {o.status === "resting" && (
                      <button type="button" className="small" disabled={busy} onClick={() => cancel([o.id])}>
                        Cancel
                      </button>
                    )}
                  </td>
                </tr>
              ))}
            </tbody>
          </table>
        </div>
      )}

      {msg && (
        <div className={`result ${msg.ok ? "" : "error"}`} role="status">
          {msg.exp ? (
            <>
              <strong>{msg.exp.headline}</strong>
              <p>{msg.exp.text}</p>
            </>
          ) : (
            msg.error
          )}
        </div>
      )}

      <details className="manual-cancel">
        <summary>Cancel any order by number</summary>
        <form className="form cancel" onSubmit={cancelManual}>
          <label>
            Order #
            <input inputMode="numeric" value={manualId} placeholder="e.g. 12" onChange={(e) => setManualId(e.target.value)} />
          </label>
          <button type="submit" disabled={busy}>Cancel</button>
        </form>
        <p className="footnote">
          It's a shared demo, so you can cancel anyone's resting order — try one of the sample orders from the book.
        </p>
      </details>
    </section>
  );
}
