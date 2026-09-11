#!/usr/bin/env python3
"""Measures whether a model reads a natural-language intent and produces code
that actually works, with the compiler in the loop.

Each task declares a node with an `intent` and, where one exists, `ensure`
contracts that constrain the answer without stating it. Deliberately none of the
contracts is an `output == expression` form, so deterministic extraction cannot
answer them and the model has to.

Three outcomes are recorded per task:

  compiled   the candidate passed parser, type and effect checks
  accepted   it also survived the contract probe, so synthesis emitted source
  correct    the emitted source matches a reference implementation on a grid of
             inputs that is deliberately different from the probe grid

`accepted` without `correct` is the interesting failure: the contract was too
weak to catch a wrong implementation.

    GLYPH_BIN=build/glyph python3 bench/synthesis_bench.py MODEL_DIR

The model directory can also come from GLYPH_QWEN_DIR, the same variable
tests/test_model.py reads.
"""
import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path

BIN = Path(os.environ.get("GLYPH_BIN", "build/glyph")).resolve()

INT_GRID = [-7, -3, 0, 2, 5, 13, 79, 95, 101, 500]
PAIR_GRID = [(3, 7), (7, 3), (-4, -9), (0, 0), (12, 12), (-5, 5), (100, 1)]


def clamp(x):
    return min(max(x, 0), 100)


def grade(x):
    return 4 if x >= 90 else 3 if x >= 80 else 2 if x >= 70 else 0


TASKS = [
    dict(name="Clamp", inputs=[("score", "Int")], out="Int",
         ensure=["output >= 0", "output <= 100"],
         intent="Return score limited to the inclusive range 0 through 100.\n"
                "Scores already inside the range are returned unchanged.",
         ref=clamp, grid=[(x,) for x in INT_GRID]),
    dict(name="Magnitude", inputs=[("value", "Int")], out="Int",
         ensure=["output >= 0"],
         intent="Return the distance of value from zero, so negatives become positive.",
         ref=abs, grid=[(x,) for x in INT_GRID]),
    dict(name="Sign", inputs=[("value", "Int")], out="Int",
         ensure=["output >= -1", "output <= 1"],
         intent="Return -1 when value is negative, 0 when it is zero, and 1 when it is positive.",
         ref=lambda x: (x > 0) - (x < 0), grid=[(x,) for x in INT_GRID]),
    dict(name="FloorAtZero", inputs=[("value", "Int")], out="Int",
         ensure=["output >= 0"],
         intent="Return value, except that negative values are replaced by zero.",
         ref=lambda x: max(x, 0), grid=[(x,) for x in INT_GRID]),
    dict(name="Grade", inputs=[("score", "Int")], out="Int",
         ensure=["output >= 0", "output <= 4"],
         intent="Convert a score to a grade point. 90 or above is 4, 80 to 89 is 3,\n"
                "70 to 79 is 2, and anything below 70 is 0.",
         ref=grade, grid=[(x,) for x in INT_GRID]),
    dict(name="Larger", inputs=[("a", "Int"), ("b", "Int")], out="Int",
         ensure=["output >= a", "output >= b"],
         intent="Return whichever of the two inputs is larger.",
         ref=max, grid=PAIR_GRID),
    dict(name="Smaller", inputs=[("a", "Int"), ("b", "Int")], out="Int",
         ensure=["output <= a", "output <= b"],
         intent="Return whichever of the two inputs is smaller.",
         ref=min, grid=PAIR_GRID),
    dict(name="Gap", inputs=[("a", "Int"), ("b", "Int")], out="Int",
         ensure=["output >= 0"],
         intent="Return how far apart the two inputs are, never negative.",
         ref=lambda a, b: abs(a - b), grid=PAIR_GRID),
    dict(name="Positive", inputs=[("value", "Int")], out="Bool", ensure=[],
         intent="Return true when value is greater than zero, otherwise false.",
         ref=lambda x: x > 0, grid=[(x,) for x in INT_GRID]),
    dict(name="Even", inputs=[("value", "Int")], out="Bool", ensure=[],
         intent="Return true when value is an even number.",
         ref=lambda x: x % 2 == 0, grid=[(x,) for x in INT_GRID]),
    dict(name="SafeShare", inputs=[("total", "Int"), ("people", "Int")], out="Int", ensure=[],
         intent="Return total divided by people. When people is zero, return zero\n"
                "instead of dividing, because division by zero fails.",
         ref=lambda t, p: 0 if p == 0 else int(t / p) if t * p >= 0 else -int(abs(t) / abs(p)),
         grid=[(10, 2), (7, 0), (0, 0), (9, 3), (-8, 2), (5, 5)]),
    dict(name="Blank", inputs=[("text", "Text")], out="Bool", ensure=[],
         intent="Return true when text has no characters at all.",
         ref=lambda s: s == "", grid=[("",), ("a",), ("glyph",)]),
]


def source(task):
    ports = "".join("    %s: %s\n" % pair for pair in task["inputs"])
    ensure = "".join("    %s\n" % line for line in task["ensure"])
    intent = "".join("    %s\n" % line for line in task["intent"].split("\n"))
    args = ", ".join(name for name, _ in task["inputs"])
    return ("node %s\nin:\n%sout:\n    %s\n%sintent:\n%s\nflow Main\nin:\n%sout:\n    %s\n%s(%s)\n"
            % (task["name"], ports, task["out"],
               ("ensure:\n" + ensure) if ensure else "", intent,
               ports, task["out"], task["name"], args))


def main():
    model = sys.argv[1] if len(sys.argv) > 1 else os.environ.get("GLYPH_QWEN_DIR", "")
    if len(sys.argv) > 2:
        raise SystemExit("usage: python3 bench/synthesis_bench.py MODEL_DIR")
    if not model:
        raise SystemExit("no model directory given\n"
                         "usage: python3 bench/synthesis_bench.py MODEL_DIR\n"
                         "   or: GLYPH_QWEN_DIR=... python3 bench/synthesis_bench.py")
    if not Path(model, "config.json").exists():
        raise SystemExit("%s does not look like a model directory: no config.json" % model)
    if not BIN.exists():
        raise SystemExit("%s not found: build glyph first, or set GLYPH_BIN" % BIN)
    rows, work = [], Path(tempfile.mkdtemp())
    for task in TASKS:
        path = work / (task["name"] + ".glyph")
        path.write_text(source(task))
        resolved, trace = work / (task["name"] + ".out.glyph"), work / (task["name"] + ".jsonl")
        done = subprocess.run([str(BIN), "synth", str(path), "-o", str(resolved),
                               "--model", model, "--trace", str(trace)],
                              capture_output=True, text=True)
        attempts = [json.loads(line) for line in trace.read_text().splitlines()] if trace.exists() else []
        compiled = any("ContractViolation" in a.get("diagnostic", "") or a["accepted"] for a in attempts)
        accepted = done.returncode == 0 and resolved.exists()
        impl = ""
        if accepted:
            impl = next(a["candidate"] for a in attempts if a["accepted"])
        correct = None
        if accepted:
            correct = True
            for case in task["grid"]:
                out = subprocess.run([str(BIN), "run", str(resolved), "--", *map(str, case)],
                                     capture_output=True, text=True)
                want = task["ref"](*case)
                if out.returncode != 0 or json.loads(out.stdout) != want:
                    correct = False
                    got = out.stdout.strip() or out.stderr.strip().splitlines()[0]
                    impl += "   [%s -> %s, expected %s]" % (case, got, want)
                    break
        rows.append(dict(name=task["name"], compiled=compiled, accepted=accepted,
                         correct=correct, attempts=len(attempts) or 1,
                         first_try=accepted and attempts and attempts[0]["accepted"],
                         probe_caught=any("ContractViolation" in a.get("diagnostic", "") for a in attempts),
                         impl=impl or done.stderr.strip().splitlines()[-1][:90]))
        mark = "correct" if rows[-1]["correct"] else ("ACCEPTED BUT WRONG" if accepted else "rejected")
        print("%-12s %-19s tries=%d  %s" % (task["name"], mark, rows[-1]["attempts"], rows[-1]["impl"]))

    total = len(rows)
    accepted = sum(r["accepted"] for r in rows)
    correct = sum(bool(r["correct"]) for r in rows)
    first = sum(bool(r["first_try"]) for r in rows)
    caught = sum(r["probe_caught"] for r in rows)
    print("\n%d tasks" % total)
    print("  accepted by the compiler and contracts : %d" % accepted)
    print("  correct against the reference          : %d" % correct)
    print("  accepted on the first attempt          : %d" % first)
    print("  tasks where a contract probe rejected at least one candidate: %d" % caught)
    print("  accepted but wrong (contract too weak) : %d" % (accepted - correct))


if __name__ == "__main__":
    main()
