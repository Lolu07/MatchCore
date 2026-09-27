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
      <p className="footnote">{metrics?.engine ?? "—"}</p>
      <dl className="stats">
        {live.map(([label, v]) => (
          <div key={label}>
            <dt>{label}</dt>
            <dd>{v}</dd>
          </div>
        ))}
      </dl>

      <h3>
        Benchmark results <span className="badge">static · not live</span>
      </h3>
      <p className="footnote">
        {bench.source}. {bench.workload}. {bench.hardware}.
      </p>
      <div className="bench">
        <table className="grid">
          <caption>MatchingEngine (MPSC)</caption>
          <thead><tr><th className="num">Thr</th><th className="num">ops/s</th></tr></thead>
          <tbody>
            {bench.matching_engine.map((r) => (
              <tr key={r.threads}><td className="num">{r.threads}</td><td className="num">{mops(r.ops_per_s)}</td></tr>
            ))}
          </tbody>
        </table>
        <table className="grid">
          <caption>ConcurrentOrderBook (shared_mutex)</caption>
          <thead><tr><th className="num">Thr</th><th className="num">ops/s</th><th className="num">p50</th><th className="num">p99</th></tr></thead>
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
