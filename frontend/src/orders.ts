// Tracks the orders this visitor placed and keeps their status in sync with
// the engine's book snapshots. Status is *read* from the book (is the order
// still resting, with how many shares?) — never predicted.

import type { Book, OrderResult, OrderType, Side } from "./api";

export type MyStatus = "resting" | "filled" | "cancelled" | "dropped";

export interface MyOrder {
  id: number;
  side: Side;
  type: OrderType;
  price: number | null;
  quantity: number;
  leaves: number;      // shares still resting (0 once filled/cancelled/dropped)
  filled: number;      // shares traded so far
  status: MyStatus;
  queuePos: number | null; // 0 = front of its price level
}

export function fromResult(r: OrderResult): MyOrder {
  const resting = r.status === "resting" || r.status === "partially_filled_resting";
  return {
    id: r.order_id,
    side: r.side,
    type: r.type,
    price: r.price,
    quantity: r.quantity,
    leaves: resting ? r.leaves : 0,
    filled: r.filled,
    status: resting ? "resting" : r.status === "filled" ? "filled" : "dropped",
    queuePos: null,
  };
}

/** Update resting orders from a snapshot: new leaves, queue position, or gone (traded). */
export function reconcile(orders: MyOrder[], book: Book): MyOrder[] {
  let changed = false;
  const next = orders.map((o) => {
    if (o.status !== "resting" || o.price === null) return o;
    const levels = o.side === "buy" ? book.bids : book.asks;
    const level = levels.find((l) => l.price === o.price);
    const idx = level ? level.orders.findIndex((x) => x.id === o.id) : -1;
    if (level && idx >= 0) {
      const leaves = level.orders[idx].leaves;
      if (leaves === o.leaves && idx === o.queuePos) return o;
      changed = true;
      return { ...o, leaves, filled: o.quantity - leaves, queuePos: idx };
    }
    // Not visible. Only conclude "traded" if its price is inside the depth we
    // were sent and its level's queue wasn't truncated.
    const totalLevels = o.side === "buy" ? book.bid_levels : book.ask_levels;
    const deepest = levels.at(-1)?.price;
    const beyondView =
      levels.length < totalLevels && deepest !== undefined && (o.side === "buy" ? o.price < deepest : o.price > deepest);
    const truncatedLevel = level !== undefined && level.order_count > level.orders.length;
    if (beyondView || truncatedLevel) return o;
    changed = true;
    return { ...o, leaves: 0, filled: o.quantity, status: "filled" as const, queuePos: null };
  });
  return changed ? next : orders;
}
