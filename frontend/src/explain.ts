// Turns engine responses into plain-English explanations for visitors.
// This only *describes* results the C++ engine returned — it never decides them.

import type { CancelResult, OrderResult } from "./api";
import { formatTicks } from "./price";

export interface Explanation {
  headline: string;
  text: string;
  fills: string[];
}

export function explainOrder(r: OrderResult, scale: number): Explanation {
  const px = (t: number) => `$${formatTicks(t, scale)}`;
  const buy = r.side === "buy";
  const counterparty = buy ? "seller" : "buyer";
  const n = r.fills.length;
  const fills = r.fills.map(
    (f) => `${f.quantity} @ ${px(f.price)} with order #${f.maker_id}`,
  );
  const trades = `${n} trade${n === 1 ? "" : "s"}`;

  switch (r.status) {
    case "resting": {
      const levels = buy ? r.book.bids : r.book.asks;
      const level = levels.find((l) => l.price === r.price);
      const ahead = level ? level.orders.findIndex((o) => o.id === r.order_id) : -1;
      const best = (buy ? r.book.best_bid : r.book.best_ask) === r.price;
      return {
        headline: `Order #${r.order_id} is waiting in the book`,
        text:
          `No ${counterparty} was asking ${px(r.price!)} or ${buy ? "less" : "more"}, so nothing traded. ` +
          `Your ${r.side} of ${r.quantity} now rests at ${px(r.price!)}` +
          (best ? ` — the new best ${buy ? "bid" : "ask"}` : "") +
          "." +
          (ahead > 0
            ? ` ${ahead} order${ahead === 1 ? " is" : "s are"} ahead of you at this price because ${ahead === 1 ? "it" : "they"} arrived first.`
            : "") +
          ` It trades automatically when a ${buy ? "sell" : "buy"} order arrives at your price or better — or cancel it below.`,
        fills,
      };
    }
    case "filled":
      return {
        headline: `Order #${r.order_id} filled completely — ${trades}`,
        text:
          `Your ${r.type} ${r.side} ${r.type === "limit" ? "crossed the spread and " : ""}traded with ${counterparty}s already in the book: ` +
          `best price first, and at each price the oldest order first. Each trade happened at the resting order's price.`,
        fills,
      };
    case "partially_filled_resting":
      return {
        headline: `Order #${r.order_id} partly filled — ${r.leaves} still waiting`,
        text:
          `${r.filled} of ${r.quantity} traded immediately (${trades}). There weren't enough ${counterparty}s at ${px(r.price!)} or better, ` +
          `so the remaining ${r.leaves} now rest in the book at ${px(r.price!)}.`,
        fills,
      };
    case "partially_filled_remainder_discarded":
      return {
        headline: `Market order #${r.order_id} filled ${r.filled} of ${r.quantity}`,
        text:
          `It traded with every ${counterparty} in the book (${trades}) and then ran out. ` +
          `Market orders never wait in the book, so the unfilled ${r.leaves} were dropped.`,
        fills,
      };
    case "unfilled_discarded":
      return {
        headline: `Market order #${r.order_id} did not trade`,
        text: `There were no ${counterparty}s in the book. Market orders never wait, so it was dropped.`,
        fills,
      };
    default:
      return { headline: `Order #${r.order_id}: ${r.status}`, text: "", fills };
  }
}

export function explainCancel(r: CancelResult): Explanation {
  return r.cancelled
    ? {
        headline: `Order #${r.order_id} cancelled`,
        text: "It has been removed from the book. The engine finds it directly through its order-ID index, without scanning the book.",
        fills: [],
      }
    : {
        headline: `Order #${r.order_id} was not cancelled`,
        text: "It isn't resting in the book — it may have already traded in full, been cancelled, or never existed.",
        fills: [],
      };
}
