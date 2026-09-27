import { useState } from "react";
import { api, type Metrics as MetricsData, type SpeedTestResult } from "../api";
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
    ["Visitor orders", m ? m.orders_submitted.toLocaleString() : "—"],
    ["Simulated orders", m ? m.simulated_orders.toLocaleString() : "—"],
    ["Fills", m ? m.fills.toLocaleString() : "—"],
    ["Resting orders", m ? m.resting_orders.toLocaleString() : "—"],
    ["Volume traded", m ? m.volume.toLocaleString() : "—"],
    ["Cancels ok / requested", m ? `${m.cancels_succeeded} / ${m.cancels_requested}` : "—"],
  ];
  const [test, setTest] = useState<SpeedTestResult | null>(null);
  const [testing, setTesting] = useState(false);
  const [testError, setTestError] = useState<string | null>(null);

  async function runTest() {
    setTesting(true);
    setTestError(null);
    try {
      setTest(await api.speedTest());
    } catch (err) {
      setTestError((err as Error).message);
    } finally {
      setTesting(false);
    }
  }

  return (
    <section className="panel">
      <h2>Engine Metrics</h2>
      <p className="panel-desc">
        Live counters since the server last started, across all visitors.
        {!!metrics?.seeded_orders && ` Excludes the ${metrics.seeded_orders} sample orders placed at startup.`}
      </p>
      <dl className="stats">
        {live.map(([label, v]) => (
          <div key={label}>
            <dt>{label}</dt>
            <dd>{v}</dd>
          </div>
        ))}
      </dl>

      <div className="speedtest">
        <div className="panel-head">
          <h3>Live speed test</h3>
          <button type="button" className="small" disabled={testing} onClick={runTest}>
            {testing ? "Running 100,000 orders…" : test ? "Run again" : "Run speed test"}
          </button>
        </div>
        <p className="panel-desc">
          Runs 100,000 orders (the benchmark's 80% limit / 10% market / 10% cancel mix) through a fresh, private copy of
          the engine on this server right now — the shared book isn't touched.
        </p>
        {testError && <div className="result error">{testError}</div>}
        {test && (
          <>
            <dl className="stats two">
              <div title="Orders per second through MatchingEngine: one producer thread enqueues, the matching thread processes. Timed until the queue is fully drained.">
                <dt className="term">Engine throughput</dt>
                <dd>{mops(test.engine_ops_per_s)} <span className="unit">orders/s</span></dd>
              </div>
              <div title="Time for a single OrderBook call on one thread, no queue or lock. p50: half are faster; p99: 99% are faster.">
                <dt className="term">Per-order latency p50 / p99</dt>
                <dd>{ns(test.book_p50_ns)} / {ns(test.book_p99_ns)}</dd>
              </div>
            </dl>
            <p className="footnote">
              Measured just now on the demo server ({test.cpus} CPU{test.cpus === 1 ? "" : "s"}, shared free-tier
              instance) · {test.engine_trades.toLocaleString()} trades generated. Expect lower numbers than the dedicated
              run below; hosted CPUs are slower and shared.
            </p>
          </>
        )}
      </div>

      <h3>
        Recorded benchmark <span className="badge">offline run · not live</span>
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
