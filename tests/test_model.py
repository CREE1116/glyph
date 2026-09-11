#!/usr/bin/env python3
"""Reference parity test for the native Qwen loader.

Checks three things against Hugging Face transformers in FP32: tokenizer IDs,
final-position logits, and greedy continuations decoded through the attention
cache. Also asserts that cached decoding stays close to a full recompute.
Skips when the reference stack or a model directory is absent, so it stays
optional in CI.

    GLYPH_BIN=build/glyph GLYPH_QWEN_DIR=/path/to/Qwen2.5-0.5B-Instruct \
        python3 tests/test_model.py
"""
import json
import os
import subprocess
import sys

TOLERANCE = 5e-3
DRIFT_TOLERANCE = 5e-3
CONTINUATIONS = 16

PROMPTS = [
    "The capital of France is",
    "Hello",
    "def add(a, b):\n    return",
    "안녕하세요, 오늘 날씨는",
    " ".join(str(n) for n in range(1, 34)),
    "<|im_start|>system\nYou are helpful.<|im_end|>\n<|im_start|>user\nHi<|im_end|>\n<|im_start|>assistant\n",
    "Emoji 🚀 and tabs\tand  spaces   end.",
]


def skip(reason):
    print("SKIP: " + reason)
    sys.exit(0)


def main():
    directory = os.environ.get("GLYPH_QWEN_DIR")
    if not directory or not os.path.isdir(directory):
        skip("set GLYPH_QWEN_DIR to a Qwen2/Qwen2.5 directory")
    binary = os.environ.get("GLYPH_BIN", "build/glyph")
    if not os.path.exists(binary):
        skip("build glyph first, or set GLYPH_BIN")
    try:
        import numpy as np
        import torch
        from transformers import AutoModelForCausalLM, AutoTokenizer
    except ImportError as error:
        skip("reference stack unavailable (%s); pip install torch transformers" % error)

    from transformers import GenerationConfig

    tokenizer = AutoTokenizer.from_pretrained(directory)
    model = AutoModelForCausalLM.from_pretrained(directory, dtype=torch.float32).eval()
    # Qwen ships sampling defaults including a repetition penalty. Glyph decodes
    # plain greedy, so the reference has to be asked for plain greedy too.
    greedy = GenerationConfig(do_sample=False, repetition_penalty=1.0, top_p=1.0,
                              num_beams=1, max_new_tokens=CONTINUATIONS,
                              eos_token_id=model.generation_config.eos_token_id,
                              pad_token_id=model.generation_config.pad_token_id)

    def run(mode, text):
        done = subprocess.run([binary, "model", directory, mode, text],
                              capture_output=True, text=True)
        if done.returncode:
            raise SystemExit("glyph %s failed: %s" % (mode, done.stderr.strip()))
        return done.stdout

    def glyph(mode, text):
        return json.loads(run(mode, text))

    failures = 0
    for prompt in PROMPTS:
        expected_ids = tokenizer(prompt, add_special_tokens=False)["input_ids"]
        ids = glyph("--tokens", prompt)
        with torch.no_grad():
            reference = model(torch.tensor([expected_ids])).logits[0, -1].double().numpy()
        logits = np.array(glyph("--logits", prompt))
        difference = float(np.abs(logits - reference).max()) if logits.shape == reference.shape else float("inf")
        ok = (ids == expected_ids and logits.shape == reference.shape
              and int(logits.argmax()) == int(reference.argmax()) and difference <= TOLERANCE)
        failures += not ok
        print("%s tokens=%-5s maxdiff=%.2e argmax=%d/%d :: %r"
              % ("PASS" if ok else "FAIL", ids == expected_ids, difference,
                 int(logits.argmax()), int(reference.argmax()), prompt[:40]))
    for prompt in PROMPTS[:4]:
        ids = tokenizer(prompt, add_special_tokens=False)["input_ids"]
        with torch.no_grad():
            expected = model.generate(torch.tensor([ids]), generation_config=greedy)[0].tolist()[len(ids):]
        produced = glyph("--generate", prompt)
        ok = produced == expected
        failures += not ok
        print("%s greedy %d tokens :: %r" % ("PASS" if ok else "FAIL", len(expected), prompt[:40]))
        if not ok:
            print("  reference %s\n  glyph     %s" % (expected, produced))

    # The cache must not change the arithmetic: compare each cached step against
    # a full recompute of the same prefix.
    for prompt in PROMPTS[:3]:
        drift = [float(line.rsplit(" ", 1)[1]) for line in run("--cache-check", prompt).splitlines()]
        worst = max(drift)
        ok = bool(drift) and worst <= DRIFT_TOLERANCE
        failures += not ok
        print("%s cache drift max=%.2e over %d steps :: %r"
              % ("PASS" if ok else "FAIL", worst if drift else float("nan"), len(drift), prompt[:40]))

    if failures:
        raise SystemExit("%d check(s) diverged from the reference" % failures)
    print("logits, greedy decoding and cache reuse all match the FP32 reference")


if __name__ == "__main__":
    main()
