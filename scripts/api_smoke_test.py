#!/usr/bin/env python3
"""
End-to-end check of matchcore_server against a *fresh* server instance.

    ./build/matchcore_server &            # or: --port 8081
    python3 scripts/api_smoke_test.py     # or: python3 scripts/api_smoke_test.py http://127.0.0.1:8081

Flow: resting buy, resting sell, crossing sell that sweeps two bid levels,
check fills (maker price, price-time priority) and remaining quantities,
cancel a resting order, check it is gone, re-cancel fails. Standard library only.
"""

import json
import sys
import urllib.error
import urllib.request

BASE = (sys.argv[1] if len(sys.argv) > 1 else "http://127.0.0.1:8080").rstrip("/") + "/api"


def call(method, path, body=None):
    data = json.dumps(body).encode() if body is not None else None
    req = urllib.request.Request(BASE + path, data=data, method=method,
                                 headers={"Content-Type": "application/json"})
    try:
        with urllib.request.urlopen(req) as r:
            return r.status, json.load(r)
    except urllib.error.HTTPError as e:
        return e.code, json.load(e)


def check(cond, msg):
    print(("  ok    " if cond else "  FAIL  ") + msg)
    if not cond:
        sys.exit(1)


def levels(side):
    return [(l["price"], l["quantity"], [o["id"] for o in l["orders"]]) for l in side]


_, book = call("GET", "/book")
if book["resting_orders"] != 0:
    sys.exit("book is not empty — restart matchcore_server before running this test")

print("resting orders")
_, b1 = call("POST", "/orders", {"side": "buy", "type": "limit", "price": 9950, "quantity": 10})
_, b2 = call("POST", "/orders", {"side": "buy", "type": "limit", "price": 9900, "quantity": 5})
_, a1 = call("POST", "/orders", {"side": "sell", "type": "limit", "price": 10050, "quantity": 8})
check(b1["status"] == b2["status"] == a1["status"] == "resting", "three limit orders rest")
book = a1["book"]
check((book["best_bid"], book["best_ask"], book["spread"]) == (9950, 10050, 100),
      "best bid 99.50 / best ask 100.50 / spread 1.00")

print("crossing order")
code, x = call("POST", "/orders", {"side": "sell", "type": "limit", "price": 9900, "quantity": 12})
check(code == 201 and x["status"] == "filled" and x["filled"] == 12, "sell 12 @ 99.00 fully filled")
fills = [(f["maker_id"], f["price"], f["quantity"], f["aggressor"]) for f in x["fills"]]
check(fills == [(b1["order_id"], 9950, 10, "sell"), (b2["order_id"], 9900, 2, "sell")],
      "fills: best price first, at maker prices (10 @ 99.50, then 2 @ 99.00)")
check(levels(x["book"]["bids"]) == [(9900, 3, [b2["order_id"]])], "bid #2 reduced to 3")
check(levels(x["book"]["asks"]) == [(10050, 8, [a1["order_id"]])], "ask untouched")

print("cancellation")
_, c = call("DELETE", f"/orders/{b2['order_id']}")
check(c["cancelled"] and c["book"]["bids"] == [], "cancel resting #%d removes it" % b2["order_id"])
_, c = call("DELETE", f"/orders/{b2['order_id']}")
check(not c["cancelled"], "second cancel is rejected")
_, c = call("DELETE", f"/orders/{b1['order_id']}")
check(not c["cancelled"], "cancel of a filled order is rejected")

print("reads")
_, trades = call("GET", "/trades")
check([t["seq"] for t in trades] == [2, 1], "trade log newest first")
_, m = call("GET", "/metrics")
check((m["orders_submitted"], m["fills"], m["volume"], m["resting_orders"]) == (4, 2, 12, 1),
      "metrics: 4 orders, 2 fills, 12 volume, 1 resting")

print("validation")
check(call("POST", "/orders", {"side": "buy", "price": 99.5, "quantity": 1})[0] == 400, "fractional ticks rejected")
check(call("POST", "/orders", {"side": "buy", "price": 9950, "quantity": 0})[0] == 400, "zero quantity rejected")
check(call("POST", "/orders", {"side": "buy", "type": "market", "price": 1, "quantity": 1})[0] == 400,
      "market order with price rejected")

print("\nall checks passed")
