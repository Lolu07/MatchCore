import { useCallback, useEffect, useState } from "react";
import { api, type Book, type CancelResult, type Metrics as MetricsData, type OrderResult, type Trade } from "./api";
import { Guide, type OrderTemplate, type Progress } from "./components/Guide";
import { HowItWorks } from "./components/HowItWorks";
import { Metrics } from "./components/Metrics";
import { OrderBook } from "./components/OrderBook";
import { OrderEntry } from "./components/OrderEntry";
import { Trades } from "./components/Trades";

// State changes via this page's own actions (whose responses carry the new
// book) or via other visitors; polling keeps the latter in view.
const POLL_MS = 1000;

export function App() {
  const [book, setBook] = useState<Book | null>(null);
  const [trades, setTrades] = useState<Trade[]>([]);
  const [metrics, setMetrics] = useState<MetricsData | null>(null);
  const [online, setOnline] = useState<boolean | null>(null);
  const [cancelId, setCancelId] = useState("");
  // Orders placed from this tab, so the UI can point out "yours".
  const [mine, setMine] = useState<Set<number>>(new Set());
  const [progress, setProgress] = useState<Progress>({ rest: false, cross: false, market: false, cancel: false });
  const [prefill, setPrefill] = useState<(OrderTemplate & { nonce: number }) | null>(null);

  const refresh = useCallback(async () => {
    try {
      const [b, t, m] = await Promise.all([api.book(), api.trades(), api.metrics()]);
      setBook(b);
      setTrades(t);
      setMetrics(m);
      setOnline(true);
    } catch {
      setOnline(false);
    }
  }, []);

  useEffect(() => {
    refresh();
    const id = setInterval(refresh, POLL_MS);
    return () => clearInterval(id);
  }, [refresh]);

  // Show the engine's post-trade book immediately, then pull the trades and
  // counters it produced.
  const onOrder = useCallback(
    (r: OrderResult) => {
      setBook(r.book);
      setMine((prev) => new Set(prev).add(r.order_id));
      setProgress((p) => ({
        ...p,
        rest: p.rest || r.status === "resting" || r.status === "partially_filled_resting",
        cross: p.cross || (r.type === "limit" && r.filled > 0),
        market: p.market || r.type === "market",
      }));
      refresh();
    },
    [refresh],
  );

  const onCancel = useCallback(
    (r: CancelResult) => {
      setBook(r.book);
      if (r.cancelled) setProgress((p) => ({ ...p, cancel: true }));
      refresh();
    },
    [refresh],
  );

  const selectForCancel = (id: number) => {
    setCancelId(String(id));
    document.getElementById("order-entry")?.scrollIntoView({ block: "nearest", behavior: "smooth" });
  };

  return (
    <div className="app">
      <header className="topbar">
        <h1>MatchCore</h1>
        <span className="sub">Live demo of a C++20 stock-exchange matching engine</span>
        <a href="https://github.com/Lolu07/MatchCore" target="_blank" rel="noreferrer">
          source on GitHub
        </a>
        <span className={`conn ${online ? "ok" : online === false ? "down" : ""}`}>
          {online === null ? "connecting… (the free server can take ~30 s to wake up)" : online ? "engine connected" : "engine unreachable — retrying"}
        </span>
      </header>

      <Guide
        book={book}
        progress={progress}
        mine={mine}
        onPrefill={(o) => setPrefill({ ...o, nonce: Date.now() })}
        onSelectCancel={selectForCancel}
      />

      <main className="layout">
        <OrderBook book={book} mine={mine} onSelectOrder={selectForCancel} />
        <OrderEntry
          book={book}
          mine={mine}
          prefill={prefill}
          cancelId={cancelId}
          setCancelId={setCancelId}
          onOrder={onOrder}
          onCancel={onCancel}
        />
        <Trades trades={trades} scale={book?.price_scale ?? 100} mine={mine} />
        <Metrics metrics={metrics} />
      </main>

      <HowItWorks />
    </div>
  );
}
