import { useCallback, useEffect, useMemo, useState } from "react";
import { api, type Book, type Metrics as MetricsData, type OrderResult, type Side, type Trade } from "./api";
import { Guide, type OrderTemplate, type Progress } from "./components/Guide";
import { HowItWorks } from "./components/HowItWorks";
import { MarketBar } from "./components/MarketBar";
import { Metrics } from "./components/Metrics";
import { MyOrders } from "./components/MyOrders";
import { OrderBook } from "./components/OrderBook";
import { OrderEntry } from "./components/OrderEntry";
import { Trades } from "./components/Trades";
import { explainCancel, type Explanation } from "./explain";
import { fromResult, reconcile, type MyOrder } from "./orders";

// State changes via this page's own actions (whose responses carry the new
// book), other visitors, or simulated traders; polling keeps it all in view.
const POLL_MS = 1000;

export function App() {
  const [book, setBook] = useState<Book | null>(null);
  const [trades, setTrades] = useState<Trade[]>([]);
  const [metrics, setMetrics] = useState<MetricsData | null>(null);
  const [online, setOnline] = useState<boolean | null>(null);
  // Orders placed from this tab, newest first.
  const [myOrders, setMyOrders] = useState<MyOrder[]>([]);
  const [highlightId, setHighlightId] = useState<number | null>(null);
  const [cancelMsg, setCancelMsg] = useState<{ ok: boolean; exp?: Explanation; error?: string } | null>(null);
  const [progress, setProgress] = useState<Progress>({ rest: false, cross: false, market: false, cancel: false });
  const [prefill, setPrefill] = useState<(OrderTemplate & { nonce: number }) | null>(null);

  const mine = useMemo(() => new Set(myOrders.map((o) => o.id)), [myOrders]);

  // Every new book snapshot also refreshes the status of this visitor's orders.
  const applyBook = useCallback((b: Book) => {
    setBook(b);
    setMyOrders((os) => reconcile(os, b));
  }, []);

  const refresh = useCallback(async () => {
    try {
      const [b, t, m] = await Promise.all([api.book(), api.trades(), api.metrics()]);
      applyBook(b);
      setTrades(t);
      setMetrics(m);
      setOnline(true);
    } catch {
      setOnline(false);
    }
  }, [applyBook]);

  useEffect(() => {
    refresh();
    const id = setInterval(refresh, POLL_MS);
    return () => clearInterval(id);
  }, [refresh]);

  const onOrder = useCallback(
    (r: OrderResult) => {
      setMyOrders((os) => [fromResult(r), ...os]);
      applyBook(r.book);
      setProgress((p) => ({
        ...p,
        rest: p.rest || r.status === "resting" || r.status === "partially_filled_resting",
        cross: p.cross || (r.type === "limit" && r.filled > 0),
        market: p.market || r.type === "market",
      }));
      refresh();
    },
    [applyBook, refresh],
  );

  // Used by My Orders (single, "cancel all", by number) and the book's × buttons,
  // so every cancel path reports its result in the same place.
  const cancel = useCallback(
    async (ids: number[]) => {
      try {
        let ok = 0;
        let last = null;
        for (const id of ids) {
          last = await api.cancel(id);
          applyBook(last.book);
          if (last.cancelled) {
            ok++;
            setMyOrders((os) => os.map((o) => (o.id === id ? { ...o, status: "cancelled", leaves: 0, queuePos: null } : o)));
            setProgress((p) => ({ ...p, cancel: true }));
          }
        }
        if (last) {
          const exp = explainCancel(last);
          if (ids.length > 1) exp.headline = `${ok} of ${ids.length} orders cancelled`;
          setCancelMsg({ ok: last.cancelled, exp });
        }
      } catch (err) {
        setCancelMsg({ ok: false, error: (err as Error).message });
      }
      refresh();
    },
    [applyBook, refresh],
  );

  const simulate = useCallback(async () => {
    const r = await api.simulate(8);
    applyBook(r.book);
    refresh();
    return r;
  }, [applyBook, refresh]);

  const showMyOrder = (id: number) => {
    setHighlightId(id);
    document.getElementById("my-orders")?.scrollIntoView({ block: "nearest", behavior: "smooth" });
  };

  const tradeAt = (side: Side, price: number) =>
    setPrefill({ side, type: "limit", price, quantity: 0, nonce: Date.now() });

  return (
    <div className="app">
      <header className="topbar">
        <span className="brand" aria-hidden="true"><i className="bar ask" /><i className="bar bid" /></span>
        <h1>MatchCore</h1>
        <span className="sub">Live demo of a C++20 stock-exchange matching engine</span>
        <a href="https://github.com/Lolu07/MatchCore" target="_blank" rel="noreferrer">source on GitHub</a>
        <span className={`conn ${online ? "ok" : online === false ? "down" : ""}`}>
          {online === null ? "connecting… (the free server can take ~30 s to wake up)" : online ? "engine connected · live" : "engine unreachable — retrying"}
        </span>
      </header>

      <Guide
        book={book}
        progress={progress}
        mine={mine}
        onPrefill={(o) => setPrefill({ ...o, nonce: Date.now() })}
        onSelectCancel={showMyOrder}
      />

      <MarketBar onBurst={simulate} />

      <main className="layout">
        <OrderBook book={book} mine={mine} onTradeAt={tradeAt} onCancel={(id) => {
          void cancel([id]);
          document.getElementById("my-orders")?.scrollIntoView({ block: "nearest", behavior: "smooth" });
        }} />
        <div className="stack">
          <OrderEntry book={book} prefill={prefill} onOrder={onOrder} />
          <MyOrders orders={myOrders} scale={book?.price_scale ?? 100} highlightId={highlightId} message={cancelMsg} onCancel={cancel} />
        </div>
        <Trades trades={trades} scale={book?.price_scale ?? 100} mine={mine} />
        <Metrics metrics={metrics} />
      </main>

      <HowItWorks />
    </div>
  );
}
