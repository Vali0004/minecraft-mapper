#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""Choose integer recipe runs for a complete crafting job.

Input and output are JSON on standard input and output. The caller selects a
finite candidate recipe graph and supplies its held stock, item values, and
machine requirements. A successful result is optimal for that graph when
HiGHS reports a zero gap.
"""

import json
import sys

import numpy as np
from scipy.optimize import Bounds, LinearConstraint, milp
from scipy.sparse import coo_matrix


def optimize(problem):
    recipes = problem["recipes"]
    held = problem.get("held", {})
    value = problem.get("value", {})
    demand = problem["demand"]
    free = set(problem.get("free", []))
    gather = problem.get("gather", {})
    items = sorted(set(held) | set(value) | set(demand) |
                   {k for r in recipes for k in (*r["in"], *r["out"], *r.get("tools", []))} |
                   {r["machine"] for r in recipes if r.get("machine")})
    ix = {k: i for i, k in enumerate(items)}
    nr, ni = len(recipes), len(items)
    run = list(range(nr))
    active = list(range(nr, 2 * nr))
    stock = {k: 2 * nr + i for i, k in enumerate(items)}
    missing = {k: 2 * nr + ni + i for i, k in enumerate(items)}
    nv = 2 * nr + 2 * ni
    rows, cols, data = [], [], []
    lower = np.zeros(nv)
    upper = np.full(nv, np.inf)
    cost = np.zeros(nv)
    integer = np.zeros(nv)
    row_lo = [float(demand.get(k, 0)) for k in items]
    row_hi = [np.inf] * ni

    def put(row, col, amount):
        if amount:
            rows.append(row)
            cols.append(col)
            data.append(amount)

    def constraint(coefficients, lo=-np.inf, hi=np.inf):
        row = len(row_lo)
        row_lo.append(lo)
        row_hi.append(hi)
        for col, amount in coefficients:
            put(row, col, amount)

    for i, recipe in enumerate(recipes):
        maximum = int(recipe.get("max_runs", 10000))
        upper[run[i]] = maximum
        upper[active[i]] = 1
        integer[run[i]] = integer[active[i]] = 1
        cost[run[i]] = float(recipe.get("run_cost", 0))
        cost[active[i]] = float(recipe.get("setup_cost", 1))
        constraint([(run[i], 1), (active[i], -maximum)], hi=0)
        constraint([(run[i], 1), (active[i], -1)], lo=0)
        for k, amount in recipe["out"].items():
            put(ix[k], run[i], float(amount))
        for k, amount in recipe["in"].items():
            put(ix[k], run[i], -float(amount))
        for k in recipe.get("tools", []):
            terms = [(active[i], 1)]
            for j, producer in enumerate(recipes):
                if k in producer["out"]:
                    terms.append((run[j], -min(1, float(producer["out"][k]))))
            constraint(terms, hi=float(held.get(k, 0)))
        machine = recipe.get("machine")
        if machine:
            terms = [(active[i], 1)]
            for j, producer in enumerate(recipes):
                if machine in producer["out"]:
                    terms.append((run[j], -min(1, float(producer["out"][machine]))))
            constraint(terms, hi=float(held.get(machine, 0)))

    for k in items:
        put(ix[k], stock[k], 1)
        put(ix[k], missing[k], 1)
        upper[stock[k]] = 1000000000 if k in free else float(held.get(k, 0))
        cost[stock[k]] = 0 if k in free else float(value.get(k, 1))
        cost[missing[k]] = float(gather.get(k, problem.get("missing_cost", 1000000)))
        if not problem.get("allow_missing", True) and k not in gather:
            upper[missing[k]] = 0

    matrix = coo_matrix((data, (rows, cols)), shape=(len(row_lo), nv)).tocsr()
    result = milp(cost, integrality=integer, bounds=Bounds(lower, upper),
                  constraints=LinearConstraint(matrix, row_lo, row_hi),
                  options={"time_limit": float(problem.get("time_limit", 10)), "mip_rel_gap": 0.0})
    if result.x is None:
        return {"status": "no_solution", "message": result.message}
    chosen = []
    for i, recipe in enumerate(recipes):
        amount = int(round(result.x[run[i]]))
        if amount:
            chosen.append({"id": recipe["id"], "runs": amount})
    return {
        "status": "optimal" if result.status == 0 else "bounded",
        "gap": getattr(result, "mip_gap", None),
        "cost": result.fun,
        "recipes": chosen,
        "stock": {k: round(result.x[stock[k]], 6) for k in items if result.x[stock[k]] > 1e-6 and k not in free},
        "missing": {k: round(result.x[missing[k]], 6) for k in items if result.x[missing[k]] > 1e-6},
    }


def order_runs(problem, solution):
    """Return an executable order or None if aggregate flows hide a cycle."""
    recipes = {r["id"]: r for r in problem["recipes"]}
    remaining = {entry["id"]: entry["runs"] for entry in solution["recipes"]}
    stock = dict(solution["stock"])
    for k, n in solution.get("missing", {}).items():
        stock[k] = stock.get(k, 0) + n
    for k in problem.get("free", []):
        stock[k] = float("inf")
    held_tools = problem.get("held", {})
    order = []
    previous = None

    def can_run(rid):
        recipe = recipes[rid]
        if any(stock.get(k, 0) + 1e-6 < n for k, n in recipe["in"].items()):
            return False
        if any(stock.get(k, 0) < 1 and held_tools.get(k, 0) < 1 for k in recipe.get("tools", [])):
            return False
        machine = recipe.get("machine")
        return not machine or stock.get(machine, 0) >= 1 or held_tools.get(machine, 0) >= 1

    while remaining:
        ready = previous if previous in remaining and can_run(previous) else None
        if ready is None:
            ready = next((rid for rid in remaining if can_run(rid)), None)
        if ready is None:
            return None
        recipe = recipes[ready]
        for k, n in recipe["in"].items():
            stock[k] -= n
        for k, n in recipe["out"].items():
            stock[k] = stock.get(k, 0) + n
        order.append(ready)
        remaining[ready] -= 1
        if not remaining[ready]:
            del remaining[ready]
        previous = ready
    if any(stock.get(k, 0) + 1e-6 < n for k, n in problem["demand"].items()):
        return None
    return order


if __name__ == "__main__":
    problem = json.load(sys.stdin)
    result = optimize(problem)
    if result["status"] == "optimal":
        order = order_runs(problem, result)
        if order is None:
            result["status"] = "unschedulable"
        else:
            result["order"] = []
            for rid in order:
                if result["order"] and result["order"][-1]["id"] == rid:
                    result["order"][-1]["runs"] += 1
                else:
                    result["order"].append({"id": rid, "runs": 1})
    json.dump(result, sys.stdout, allow_nan=False)
    sys.stdout.write("\n")
