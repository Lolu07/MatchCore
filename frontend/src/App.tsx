import { useCallback, useEffect, useState } from "react";
import { api, type Book, type Metrics as MetricsData, type Trade } from "./api";
import { Metrics } from "./components/Metrics";
import { OrderBook } from "./components/OrderBook";
import { OrderEntry } from "./components/OrderEntry";
import { Trades } from "./components/Trades";

// State changes only via this UI's own actions (whose responses carry the new
// book), so polling just keeps other tabs / curl sessions in sync.
const POLL_MS = 1000;

export function App() {
  const [book, setBook] = useState<Book | null>(null);
  const [trades, setTrades] = useState<Trade[]>([]);
  const [metrics, setMetrics] = useState<MetricsData | null>(null);
  const [online, setOnline] = useState<boolean | null>(null);
  const [cancelId, setCancelId] = useState("");

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

  // After an order/cancel: show the engine's post-trade book immediately,
  // then pull the trades and counters it produced.
  const onEngineUpdate = useCallback(
    (b: Book) => {
      setBook(b);
      refresh();
    },
    [refresh],
  );

  return (
    <div className="app">
      <header className="topbar">
        <h1>MatchCore</h1>
        <span className="sub">C++20 limit order book · price-time priority</span>
        <a href="https://github.com/Lolu07/MatchCore" target="_blank" rel="noreferrer">
          source on GitHub
        </a>
        <span className={`conn ${online ? "ok" : online === false ? "down" : ""}`}>
          {online === null ? "connecting…" : online ? "engine connected" : "engine unreachable — retrying"}
        </span>
      </header>
      <main className="layout">
        <OrderBook book={book} onSelectOrder={(id) => setCancelId(String(id))} />
        <OrderEntry book={book} cancelId={cancelId} setCancelId={setCancelId} onEngineUpdate={onEngineUpdate} />
        <Trades trades={trades} scale={book?.price_scale ?? 100} />
        <Metrics metrics={metrics} />
      </main>
    </div>
  );
}
