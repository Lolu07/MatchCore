// Typed client for the C++ matchcore_server. All prices are integer ticks,
// exactly as the engine stores them; see price.ts for display conversion.

export type Side = "buy" | "sell";
export type OrderType = "limit" | "market";

export interface RestingOrder {
  id: number;
  leaves: number;
}

export interface Level {
  price: number;
  quantity: number;
  order_count: number;
  orders: RestingOrder[]; // FIFO front of the queue (may be truncated); first matches next
}

export interface Book {
  price_scale: number;
  best_bid: number | null;
  best_ask: number | null;
  spread: number | null;
  bid_levels: number;
  ask_levels: number;
  resting_orders: number;
  bids: Level[]; // best first
  asks: Level[]; // best first
}

export interface Trade {
  seq: number;
  price: number;
  quantity: number;
  maker_id: number;
  taker_id: number;
  aggressor: Side | null;
  engine_ts_ns: number;
  time_ms: number;
}

export interface Metrics {
  engine: string;
  orders_submitted: number;
  limit_orders: number;
  market_orders: number;
  cancels_requested: number;
  cancels_succeeded: number;
  fills: number;
  volume: number;
  resting_orders: number;
  bid_levels: number;
  ask_levels: number;
  seeded_orders: number;
  uptime_s: number;
}

export interface OrderRequest {
  side: Side;
  type: OrderType;
  price?: number; // ticks; limit orders only
  quantity: number;
}

export interface OrderResult {
  order_id: number;
  side: Side;
  type: OrderType;
  price: number | null;
  quantity: number;
  filled: number;
  leaves: number;
  status: string;
  fills: Trade[];
  book: Book;
}

export interface CancelResult {
  order_id: number;
  cancelled: boolean;
  reason: string | null;
  book: Book;
}

async function call<T>(method: string, path: string, body?: unknown): Promise<T> {
  const res = await fetch(`/api${path}`, {
    method,
    headers: body ? { "Content-Type": "application/json" } : undefined,
    body: body ? JSON.stringify(body) : undefined,
  });
  const text = await res.text();
  let data: unknown;
  try {
    data = JSON.parse(text);
  } catch {
    throw new Error(`API unreachable (HTTP ${res.status})`);
  }
  if (!res.ok) {
    const msg = (data as { error?: string }).error ?? `HTTP ${res.status}`;
    throw new Error(msg);
  }
  return data as T;
}

export const api = {
  book: (depth = 20) => call<Book>("GET", `/book?depth=${depth}`),
  trades: (limit = 50) => call<Trade[]>("GET", `/trades?limit=${limit}`),
  metrics: () => call<Metrics>("GET", "/metrics"),
  submit: (req: OrderRequest) => call<OrderResult>("POST", "/orders", req),
  cancel: (id: number) => call<CancelResult>("DELETE", `/orders/${id}`),
};
