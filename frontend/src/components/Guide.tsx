import type { Book, OrderType, Side } from "../api";
import { formatTicks } from "../price";

export interface Progress {
  rest: boolean;
  cross: boolean;
  market: boolean;
  cancel: boolean;
}

export interface OrderTemplate {
  side: Side;
  type: OrderType;
  price?: number; // ticks
  quantity: number;
}

interface Props {
  book: Book | null;
  progress: Progress;
  mine: Set<number>;
  onPrefill: (o: OrderTemplate) => void;
  onSelectCancel: (id: number) => void;
}

interface Step {
  key: keyof Progress;
  title: string;
  body: string;
  action?: { label: string; run: () => void };
  unavailable?: string;
}

// Suggested orders are derived from the live book, so the walkthrough stays
// meaningful however other visitors have moved it.
export function Guide({ book, progress, mine, onPrefill, onSelectCancel }: Props) {
  const scale = book?.price_scale ?? 100;
  const px = (t: number) => `$${formatTicks(t, scale)}`;
  const bid = book?.best_bid ?? null;
  const ask = book?.best_ask ?? null;

  // Step 1: a buy at the second-best bid level. It rests (below the best ask),
  // queues behind orders already there (time priority), and is out of reach
  // of step 3's market sell, which only takes from the best bid level.
  const bidLevels = book?.bids ?? [];
  const restPrice: number | null =
    bidLevels.length > 1 ? bidLevels[1].price : bid !== null ? bid - 10 : ask !== null ? ask - 20 : null;

  // Step 2: a buy priced at the second-best ask, sized to clear the best
  // level and dip into the next — shows best-price-first, then FIFO.
  const asks = book?.asks ?? [];
  const crossPrice = asks.length > 1 ? asks[1].price : asks[0]?.price;
  const crossQty = asks.length > 1 ? asks[0].quantity + Math.min(asks[1].quantity, 15) : asks[0]?.quantity;

  // Step 3: a market sell sized to stay within the best bid level.
  const bids = bidLevels;
  const marketQty = bids.length ? Math.min(bids[0].quantity, 30) : 0;

  // Step 4: the most recent of this visitor's orders still resting.
  const restingMine = [...(book?.bids ?? []), ...(book?.asks ?? [])]
    .flatMap((l) => l.orders.map((o) => o.id))
    .filter((id) => mine.has(id));
  const latestMine = restingMine.length ? Math.max(...restingMine) : null;

  const steps: Step[] = [
    {
      key: "rest",
      title: "Place an order that waits",
      body:
        restPrice !== null
          ? `Buy 25 at ${px(restPrice)}. That's below the cheapest seller (${ask !== null ? px(ask) : "none"}), so nothing trades. Your order joins the book and queues behind buyers who were already waiting at that price.`
          : "Place any limit order; with an empty book it will rest and wait.",
      action: { label: "Fill in form", run: () => onPrefill({ side: "buy", type: "limit", price: restPrice ?? 10_000, quantity: 25 }) },
    },
    {
      key: "cross",
      title: "Cross the spread",
      body:
        crossPrice !== undefined
          ? `Buy ${crossQty} at ${px(crossPrice)}. You're willing to pay at least what sellers ask, so the engine trades immediately — first with the cheapest sellers, and among equal prices, whoever arrived first.`
          : "",
      action: crossPrice !== undefined ? { label: "Fill in form", run: () => onPrefill({ side: "buy", type: "limit", price: crossPrice, quantity: crossQty! }) } : undefined,
      unavailable: crossPrice === undefined ? "No sellers in the book right now — place a sell limit order first." : undefined,
    },
    {
      key: "market",
      title: "Send a market order",
      body: bids.length
        ? `Sell ${marketQty} at market. No price needed: it trades right away with the highest bidders (${px(bids[0].price)} first). Market orders never wait in the book.`
        : "",
      action: bids.length ? { label: "Fill in form", run: () => onPrefill({ side: "sell", type: "market", quantity: marketQty }) } : undefined,
      unavailable: bids.length ? undefined : "No buyers in the book right now — place a buy limit order first.",
    },
    {
      key: "cancel",
      title: "Cancel your order",
      body: "Find your waiting order in My Orders (it's also marked “yours” in the book) and press Cancel — it disappears from the book instantly.",
      action: latestMine !== null ? { label: `Show order #${latestMine}`, run: () => onSelectCancel(latestMine) } : undefined,
      unavailable: latestMine === null ? "You have no resting orders yet — do step 1 first." : undefined,
    },
  ];

  const done = steps.filter((s) => progress[s.key]).length;

  return (
    <details className="intro" open>
      <summary>
        <span className="intro-title">What is this, and how do I use it?</span>
        <span className="muted">{done === 4 ? "walkthrough complete ✓" : `walkthrough ${done}/4`}</span>
      </summary>

      <p className="lead">
        <strong>MatchCore is the core of a stock exchange, written in C++.</strong> This page is a live trading screen
        connected to it. Every order you place is sent to the matching engine on the server, which pairs buyers with
        sellers the way real exchanges do: <strong>best price first, then first come, first served</strong>. The book
        started with sample orders so there's something to trade against, and it's shared — other visitors see the
        same book.
      </p>
      <p className="lead ways">
        <span><strong>Follow the 4 steps below</strong>,</span>
        <span><strong>click any price</strong> in the book to trade at it,</span>
        <span>or press <strong>Simulate traders</strong> and watch the market move.</span>
      </p>

      <ol className="steps">
        {steps.map((s, i) => (
          <li key={s.key} className={progress[s.key] ? "step done" : "step"}>
            <div className="step-head">
              <span className="step-num">{progress[s.key] ? "✓" : i + 1}</span>
              <span className="step-title">{s.title}</span>
            </div>
            {/* A finished step keeps its explanation even if the book has since moved. */}
            <p>{progress[s.key] ? s.body : (s.unavailable ?? s.body)}</p>
            {s.action && !s.unavailable && (
              <button type="button" onClick={s.action.run}>
                {s.action.label} →
              </button>
            )}
          </li>
        ))}
      </ol>
      <p className="footnote">
        “Fill in form” only prepares the order — you still press the Buy/Sell button yourself. Hover over any label with a
        dotted underline for a definition. Simulated traders can trade against your orders too, so the book may move
        between steps.
      </p>
    </details>
  );
}
