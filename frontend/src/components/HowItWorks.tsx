const GLOSSARY: [string, string][] = [
  ["Bid", "An offer to buy: the most someone will pay. Shown in green."],
  ["Ask", "An offer to sell: the least someone will accept. Shown in red."],
  ["Spread", "Best ask minus best bid — the gap no one has agreed to trade across yet."],
  ["Mid", "Halfway between the best bid and best ask."],
  ["Limit order", "Trade only at your price or better. Whatever can't trade right away waits in the book."],
  ["Market order", "Trade immediately at the best prices available. Never waits; any unfilled part is dropped."],
  ["Maker", "The order that was already waiting in the book. Trades happen at the maker's price."],
  ["Taker / aggressor", "The incoming order that triggered the trade by crossing the spread."],
  ["Price-time priority", "Best price trades first; at the same price, the earliest order trades first."],
  ["Fill", "One trade between two orders. A large order can fill against several resting orders."],
  ["Tick", "The smallest price step: $0.01. The engine stores prices as whole ticks, never floating point."],
];

export function HowItWorks() {
  return (
    <details className="panel how" open>
      <summary>
        <h2>How it works</h2>
      </summary>
      <div className="how-grid">
        <div>
          <h3>What happens when you press Buy or Sell</h3>
          <ol className="pipeline">
            <li>
              Your browser sends the order to the server as JSON. Prices travel as whole cents (ticks), so no rounding
              can creep in.
            </li>
            <li>
              The C++ server places it on the engine's <strong>request queue</strong>. Many threads can add to this
              queue; one dedicated <strong>matching thread</strong> takes requests off it strictly in arrival order.
            </li>
            <li>
              The matching thread checks the opposite side of the book, best price first, and within a price the oldest
              order first. Each trade executes at the resting order's price.
            </li>
            <li>
              Any unfilled part of a limit order joins the back of the queue at its price; the unfilled part of a market
              order is dropped.
            </li>
            <li>
              The server replies with your fills and the updated book, and this page draws them. The page never matches
              anything itself — every number comes from the engine.
            </li>
          </ol>
          <p className="footnote">
            Under the hood, each side of the book is a sorted map from price to a first-in-first-out list of orders,
            plus a hash index from order ID to its place in that list — which is why cancelling is O(1). Only the
            matching thread ever touches the book, so it needs no lock.{" "}
            <a href="https://github.com/Lolu07/MatchCore#architecture" target="_blank" rel="noreferrer">
              Read the design notes →
            </a>
          </p>
        </div>
        <div>
          <h3>Glossary</h3>
          <dl className="glossary">
            {GLOSSARY.map(([term, def]) => (
              <div key={term}>
                <dt>{term}</dt>
                <dd>{def}</dd>
              </div>
            ))}
          </dl>
        </div>
      </div>
      <p className="footnote">
        This is a shared public demo: everyone trades on the same book, and it resets to the sample orders whenever the
        server restarts (the free host sleeps when idle).
      </p>
    </details>
  );
}
