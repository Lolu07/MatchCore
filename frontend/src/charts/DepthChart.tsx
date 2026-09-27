import { useState, type PointerEvent } from "react";
import type { Book } from "../api";
import { formatTicks } from "../price";
import { useWidth } from "./useWidth";

const H = 230;
const PAD = { top: 16, right: 12, bottom: 26, left: 44 };

interface Hover {
  x: number;
  price: number;
  side: "bid" | "ask" | null;
  cum: number;
}

// Cumulative depth: at each price, how many shares could trade if an order
// swept the book up to that price. Bids accumulate leftward, asks rightward.
export function DepthChart({ book }: { book: Book }) {
  const [ref, width] = useWidth<HTMLDivElement>();
  const [hover, setHover] = useState<Hover | null>(null);
  const scale = book.price_scale;

  const bids = book.bids;
  const asks = book.asks;
  if (!bids.length && !asks.length) {
    return <div ref={ref} className="chart-empty">The book is empty — nothing to chart.</div>;
  }

  let acc = 0;
  const bidCum = bids.map((l) => ({ price: l.price, cum: (acc += l.quantity) }));
  acc = 0;
  const askCum = asks.map((l) => ({ price: l.price, cum: (acc += l.quantity) }));

  const lo = bids.length ? bids[bids.length - 1].price : asks[0].price;
  const hi = asks.length ? asks[asks.length - 1].price : bids[0].price;
  const span = Math.max(hi - lo, 10);
  const x0 = lo - span * 0.04;
  const x1 = hi + span * 0.04;
  const maxCum = Math.max(bidCum.at(-1)?.cum ?? 0, askCum.at(-1)?.cum ?? 0, 1);
  const yMax = niceCeil(maxCum);

  const iw = width - PAD.left - PAD.right;
  const ih = H - PAD.top - PAD.bottom;
  const sx = (p: number) => PAD.left + ((p - x0) / (x1 - x0)) * iw;
  const sy = (q: number) => PAD.top + ih - (q / yMax) * ih;
  const invX = (px: number) => x0 + ((px - PAD.left) / iw) * (x1 - x0);

  // Step paths from the inside of the book outward.
  const stepPath = (pts: { price: number; cum: number }[], edge: number) => {
    if (!pts.length) return { line: "", area: "" };
    let d = `M${sx(pts[0].price)},${sy(0)}`;
    pts.forEach((p, i) => {
      d += ` L${sx(p.price)},${sy(p.cum)}`;
      const next = i + 1 < pts.length ? pts[i + 1].price : edge;
      d += ` L${sx(next)},${sy(p.cum)}`;
    });
    return { line: d, area: `${d} L${sx(edge)},${sy(0)} Z` };
  };
  const bidPath = stepPath(bidCum, x0);
  const askPath = stepPath(askCum, x1);

  const yTicks = [0, yMax / 2, yMax];
  const best = { bid: book.best_bid, ask: book.best_ask };

  function onMove(e: PointerEvent<SVGSVGElement>) {
    const rect = e.currentTarget.getBoundingClientRect();
    const px = Math.min(Math.max(e.clientX - rect.left, PAD.left), PAD.left + iw);
    const price = invX(px);
    // Snap to the nearest level price on the side under the pointer.
    if (best.bid !== null && price <= best.bid + 0.5) {
      const lvl = bidCum.reduce((a, b) => (Math.abs(b.price - price) < Math.abs(a.price - price) ? b : a));
      setHover({ x: sx(lvl.price), price: lvl.price, side: "bid", cum: lvl.cum });
    } else if (best.ask !== null && price >= best.ask - 0.5) {
      const lvl = askCum.reduce((a, b) => (Math.abs(b.price - price) < Math.abs(a.price - price) ? b : a));
      setHover({ x: sx(lvl.price), price: lvl.price, side: "ask", cum: lvl.cum });
    } else {
      setHover({ x: px, price, side: null, cum: 0 });
    }
  }

  return (
    <div ref={ref} className="chart">
      <div className="legend">
        <span><i className="swatch bid" /> Bids — shares buyers would absorb down to each price</span>
        <span><i className="swatch ask" /> Asks — shares sellers offer up to each price</span>
      </div>
      <svg width={width} height={H} onPointerMove={onMove} onPointerLeave={() => setHover(null)} role="img"
        aria-label="Cumulative depth chart of bids and asks">
        {yTicks.map((t) => (
          <g key={t}>
            <line className="gridline" x1={PAD.left} x2={PAD.left + iw} y1={sy(t)} y2={sy(t)} />
            <text className="axis" x={PAD.left - 6} y={sy(t) + 4} textAnchor="end">{Math.round(t).toLocaleString()}</text>
          </g>
        ))}
        <path d={bidPath.area} className="area bid" />
        <path d={askPath.area} className="area ask" />
        <path d={bidPath.line} className="series bid" />
        <path d={askPath.line} className="series ask" />
        {bidCum.length > 0 && (
          <text className="direct-label" x={sx(bidCum.at(-1)!.price) + 4} y={sy(bidCum.at(-1)!.cum) - 6}>Bids</text>
        )}
        {askCum.length > 0 && (
          <text className="direct-label" x={sx(askCum.at(-1)!.price) - 4} y={sy(askCum.at(-1)!.cum) - 6} textAnchor="end">Asks</text>
        )}
        {[lo, hi].map((p, i) => (
          <text key={i} className="axis" x={sx(p)} y={H - 8} textAnchor={i ? "end" : "start"}>{formatTicks(p, scale)}</text>
        ))}
        {best.bid !== null && best.ask !== null && (
          <text className="axis" x={(sx(best.bid) + sx(best.ask)) / 2} y={H - 8} textAnchor="middle">
            spread {formatTicks(best.ask - best.bid, scale)}
          </text>
        )}
        {hover && (
          <g pointerEvents="none">
            <line className="crosshair" x1={hover.x} x2={hover.x} y1={PAD.top} y2={PAD.top + ih} />
            {hover.side && <circle className={`dot ${hover.side}`} cx={hover.x} cy={sy(hover.cum)} r={4} />}
          </g>
        )}
      </svg>
      {hover && (
        <div className="tooltip" style={{ left: Math.min(hover.x + 10, width - 190), top: 30 }}>
          {hover.side ? (
            <>
              <strong>{hover.cum.toLocaleString()} shares</strong>
              <span>
                <i className={`key ${hover.side}`} />
                {hover.side === "bid" ? "buyers at" : "sellers at"} ${formatTicks(hover.price, scale)}
                {hover.side === "bid" ? " or higher" : " or lower"}
              </span>
            </>
          ) : (
            <span>Inside the spread — no orders here</span>
          )}
        </div>
      )}
    </div>
  );
}

function niceCeil(v: number): number {
  const mag = 10 ** Math.floor(Math.log10(v));
  for (const m of [1, 2, 2.5, 5, 10]) if (m * mag >= v) return m * mag;
  return 10 * mag;
}
