import { useState, type PointerEvent } from "react";
import type { Trade } from "../api";
import { formatTicks } from "../price";
import { useWidth } from "./useWidth";

const H = 150;
const PAD = { top: 14, right: 64, bottom: 10, left: 52 };

// Trade prices in execution order (x = trade sequence, not wall time: many
// trades share a millisecond). Single series, so no legend box.
export function PriceChart({ trades, scale }: { trades: Trade[]; scale: number }) {
  const [ref, width] = useWidth<HTMLDivElement>();
  const [hover, setHover] = useState<number | null>(null);
  const pts = [...trades].reverse(); // API is newest-first

  if (pts.length < 2) {
    return (
      <div ref={ref} className="chart-empty">
        The price line appears after two trades — cross the spread or press “Simulate traders”.
      </div>
    );
  }

  const prices = pts.map((t) => t.price);
  let lo = Math.min(...prices);
  let hi = Math.max(...prices);
  if (hi - lo < 10) { lo -= 5; hi += 5; }
  const iw = width - PAD.left - PAD.right;
  const ih = H - PAD.top - PAD.bottom;
  const sx = (i: number) => PAD.left + (i / (pts.length - 1)) * iw;
  const sy = (p: number) => PAD.top + ih - ((p - lo) / (hi - lo)) * ih;

  // Step-after: a price holds until the next trade prints.
  let d = `M${sx(0)},${sy(pts[0].price)}`;
  for (let i = 1; i < pts.length; i++) d += ` H${sx(i)} V${sy(pts[i].price)}`;

  const last = pts[pts.length - 1];
  const h = hover !== null ? pts[hover] : null;

  function onMove(e: PointerEvent<SVGSVGElement>) {
    const rect = e.currentTarget.getBoundingClientRect();
    const i = Math.round(((e.clientX - rect.left - PAD.left) / iw) * (pts.length - 1));
    setHover(Math.min(Math.max(i, 0), pts.length - 1));
  }

  return (
    <div ref={ref} className="chart">
      <svg width={width} height={H} onPointerMove={onMove} onPointerLeave={() => setHover(null)} role="img"
        aria-label={`Price of the last ${pts.length} trades`}>
        {[lo, (lo + hi) / 2, hi].map((t, i) => (
          <g key={i}>
            <line className="gridline" x1={PAD.left} x2={PAD.left + iw} y1={sy(t)} y2={sy(t)} />
            <text className="axis" x={PAD.left - 6} y={sy(t) + 4} textAnchor="end">{formatTicks(Math.round(t), scale)}</text>
          </g>
        ))}
        <path d={d} className="series price" />
        <circle className="dot price" cx={sx(pts.length - 1)} cy={sy(last.price)} r={4} />
        <text className="direct-label" x={sx(pts.length - 1) + 10} y={sy(last.price) + 4}>
          {formatTicks(last.price, scale)}
        </text>
        {hover !== null && (
          <g pointerEvents="none">
            <line className="crosshair" x1={sx(hover)} x2={sx(hover)} y1={PAD.top} y2={PAD.top + ih} />
            <circle className="dot price" cx={sx(hover)} cy={sy(pts[hover].price)} r={4} />
          </g>
        )}
      </svg>
      {h && hover !== null && (
        <div className="tooltip" style={{ left: Math.min(sx(hover) + 10, width - 200), top: 8 }}>
          <strong>${formatTicks(h.price, scale)}</strong>
          <span>trade #{h.seq} · {h.quantity} shares{h.aggressor ? ` · ${h.aggressor} aggressor` : ""}</span>
        </div>
      )}
    </div>
  );
}
