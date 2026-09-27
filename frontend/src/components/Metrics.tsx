import type { Metrics as MetricsData } from "../api";
import bench from "../data/benchmarks.json";

function ns(v: number): string {
  if (v >= 1_000_000) return `${(v / 1_000_000).toFixed(1)} ms`;
  if (v >= 1_000) return `${(v / 1_000).toFixed(1)} µs`;
  return `${v} ns`;
}

function mops(v: number): string {
  return `${(v / 1_000_000).toFixed(2)}M`;
}

export function Metrics({ metrics }: { metrics: MetricsData | null }) {
  const m = metrics;
  const live: [string, string][] = [
    ["Orders submitted", m ? m.orders_submitted.toLocaleString() : "—"],
    ["Fills", m ? m.fills.toLocaleString() : "—"],
    ["Resting orders", m ? m.resting_orders.toLocaleString() : "—"],
    ["Volume traded", m ? m.volume.toLocaleString() : "—"],
    ["Cancels ok / requested", m ? `${m.cancels_succeeded} / ${m.cancels_requested}` : "—"],
    ["Price levels bid / ask", m ? `${m.bid_levels} / ${m.ask_levels}` : "—"],
  ];

  return (
    <section className="panel">
      <h2>Engine Metrics</h2>
      <p className="panel-desc">
        Live counters from the engine since the server last started (all visitors combined).
        {!!metrics?.seeded_orders &&
          ` The ${metrics.seeded_orders} sample orders placed at startup aren't counted as submitted.`}
      </p>
      <dl className="stats">
        {live.map(([label, v]) => (
          <div key={label}>
            <dt>{label}</dt>
            <dd>{v}</dd>
          </div>
        ))}
      </dl>

      <h3>
        Benchmark results <span className="badge">recorded offline · not live</span>
      </h3>
      <p className="panel-desc">
        How fast the engine itself is, measured by a C++ benchmark with no network or browser involved.{" "}
        <span className="term" title="Operations per second: orders, cancels and market orders processed.">ops/s</span> is
        throughput;{" "}
        <span className="term" title="Latency of a single operation. p50: half of operations are faster. p99: 99% are faster — the slow tail.">p50 / p99</span>{" "}
        are per-order latency. The left design (one thread owns the book, others queue work for it) stays fast as threads
        are added; the right (threads share the book behind a lock) slows down and its slowest operations get dramatically
        slower as threads compete for the lock.
      </p>
      <p className="footnote">
        {bench.source}. {bench.workload}. {bench.hardware}.
      </p>
      <div className="bench">
        <table className="grid">
          <caption title="MatchingEngine: MPSC request queue, dedicated matching thread">Queue + 1 matching thread</caption>
          <thead><tr><th className="num">Threads</th><th className="num">ops/s</th></tr></thead>
          <tbody>
            {bench.matching_engine.map((r) => (
              <tr key={r.threads}><td className="num">{r.threads}</td><td className="num">{mops(r.ops_per_s)}</td></tr>
            ))}
          </tbody>
        </table>
        <table className="grid">
          <caption title="ConcurrentOrderBook: std::shared_mutex around the OrderBook">Shared book behind a lock</caption>
          <thead><tr><th className="num">Threads</th><th className="num">ops/s</th><th className="num">p50</th><th className="num">p99</th></tr></thead>
          <tbody>
            {bench.concurrent_order_book.map((r) => (
              <tr key={r.threads}>
                <td className="num">{r.threads}</td>
                <td className="num">{mops(r.ops_per_s)}</td>
                <td className="num">{ns(r.p50_ns)}</td>
                <td className="num">{ns(r.p99_ns)}</td>
              </tr>
            ))}
          </tbody>
        </table>
      </div>
    </section>
  );
}
