"""Per-node GPU cost cases (`run_bench.py nodes`) and their ratio budgets.

A node bench is a native executable that builds each case as a real document, compiles, prepares
and runs it on the device through the production executor, and writes per-case timings (see
benchmarks.md, "Node cost"): `coldMs` (fresh caches), `warmMs` (the same frame again, a cache
hit) and `ms` (the median fresh-cache rerun once programs are built: the frame's GPU cost).

Each case names a cheap `reference` (a Grade at the same resolution) and, for a new node, its
closest existing `comparable`. The ratios of `ms` to theirs are held to the budgets in
bench_spec.NODE_COST_BUDGETS; `compare` flags a case over budget, and a case whose `ms` or ratio
grew by more than the threshold against a baseline.
"""
import bench_spec as spec

SCHEMA = "bloom-node-cost/1"


def keyed(cases):
    """Cases by (bench, tier, case): each bench names its own reference and comparables."""
    return {(c.get("bench", ""), c["tier"], c["case"]): c for c in cases}


def with_ratios(cases):
    """The cases with `referenceRatio` and `comparableRatio` (None when not applicable)."""
    by_key = keyed(cases)
    result = []
    for case in cases:
        row = dict(case)
        for field in ("reference", "comparable"):
            other = by_key.get((case.get("bench", ""), case["tier"], case.get(field) or ""))
            ratio = None
            if (case.get("ok") and other and other.get("ok") and other.get("ms")
                    and other["case"] != case["case"]):
                ratio = round(case["ms"] / other["ms"], 3)
            row[field + "Ratio"] = ratio
        result.append(row)
    return result


def budget_for(case):
    budget = dict(spec.NODE_COST_BUDGETS["default"])
    budget.update(spec.NODE_COST_BUDGETS.get(case["case"], {}))
    return budget


def violations(cases):
    """Cases over their ratio budget, and cases that did not run."""
    found = []
    for case in with_ratios(cases):
        label = f"{case['tier']} {case['case']}"
        if not case.get("ok"):
            if (case["tier"], case["case"]) not in spec.NODE_COST_KNOWN_FAILURES:
                found.append({"case": label, "reason": "did not run: " + case.get("error", "")})
            continue
        if not case.get("comparable"):
            continue  # a reference or comparable itself
        budget = budget_for(case)
        for field in ("reference", "comparable"):
            ratio = case.get(field + "Ratio")
            if ratio is not None and ratio > budget[field]:
                found.append({"case": label, "reason": f"{ratio}x the {field} "
                              f"{case[field]} (budget {budget[field]}x)"})
    return found


def compare(current, baseline, threshold):
    """Budget violations of `current`, and its cases whose ms or ratios grew by more than
    `threshold` percent over `baseline`."""
    old = keyed(with_ratios(baseline["cases"]))
    regressions, improvements = [], []
    for case in with_ratios(current["cases"]):
        before = old.get((case.get("bench", ""), case["tier"], case["case"]))
        if not before or not case.get("ok") or not before.get("ok"):
            continue
        for metric in ("ms", "referenceRatio", "comparableRatio"):
            new_value, old_value = case.get(metric), before.get(metric)
            if not new_value or not old_value:
                continue
            change = (new_value - old_value) / old_value * 100
            row = {"key": f"{case.get('bench', '')} {case['tier']} {case['case']}".strip(),
                   "metric": metric,
                   "baseline": old_value, "current": new_value, "changePercent": round(change, 1)}
            if change > threshold:
                regressions.append(row)
            elif -change > threshold:
                improvements.append(row)
    return {"threshold": threshold, "regressions": regressions, "improvements": improvements,
            "budgetViolations": violations(current["cases"]), "cpuFallbackMeasurements": []}


def table(cases):
    lines = ["| Bench | Tier | Case | Node | Param | Cold ms | Warm ms | ms/frame | Dispatches | "
             "vs reference | vs comparable |",
             "|---|---|---|---|---|---|---|---|---|---|---|"]
    for case in with_ratios(cases):
        if not case.get("ok"):
            lines.append(f"| {case.get('bench', '')} | {case['tier']} | {case['case']} | "
                         f"{case['node']} | | did not run: {case.get('error', '')} | | | | | |")
            continue
        param = f"{case['parameter']} {case['value']}" if case.get("parameter") else ""
        ref = (f"{case['referenceRatio']:.2f}x {case['reference']}"
               if case.get("referenceRatio") else "")
        cmp_ = (f"{case['comparableRatio']:.2f}x {case['comparable']}"
                if case.get("comparableRatio") else "")
        lines.append(f"| {case.get('bench', '')} | {case['tier']} | {case['case']} | "
                     f"{case['node']} | {param} | "
                     f"{case['coldMs']:.2f} | {case['warmMs']:.3f} | {case['ms']:.2f} | "
                     f"{case['dispatches']} | {ref} | {cmp_} |")
    return "\n".join(lines)
