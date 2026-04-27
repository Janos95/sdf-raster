#!/usr/bin/env python3
import json
import os
from pathlib import Path
import subprocess
import sys

import typeset_expression_vm

PIPELINE_DIR = Path(__file__).resolve().parent
REPO_ROOT = PIPELINE_DIR.parent
GENERATED_JSON = PIPELINE_DIR / "generated_prospero.json"
GENERATED_VM = PIPELINE_DIR / "generated_prospero.vm"
GENERATED_RENDER = PIPELINE_DIR / "generated_render.ppm"
LETTERS_JSON = PIPELINE_DIR / "letters.json"
UNIQUE_SHEET = PIPELINE_DIR / "letters_unique_sheet.png"

EXPECTED_WORDS = [
    "But",
    "rough",
    "magic",
    "abjure",
    "required",
    "heavenly",
    "music",
    "senses",
    "staff",
    "bury",
    "fathoms",
    "earth",
    "deeper",
    "plummet",
    "drown",
    "book",
]


def fail(message):
    print(f"FAIL: {message}")
    sys.exit(1)


def read_ppm(path):
    with open(path, "rb") as f:
        if f.readline().strip() != b"P6":
            fail(f"{path} is not a P6 PPM")
        tokens = []
        while len(tokens) < 3:
            line = f.readline()
            if not line:
                fail(f"{path} has a truncated header")
            if not line.startswith(b"#"):
                tokens.extend(line.split())
        w, h, maxv = map(int, tokens[:3])
        if maxv != 255:
            fail(f"{path} has unsupported max value {maxv}")
        data = f.read(w * h * 3)
    if len(data) != w * h * 3:
        fail(f"{path} has truncated pixel data")
    return w, h, data


def count_lit_pixels(path):
    w, h, data = read_ppm(path)
    lit = sum(1 for i in range(0, len(data), 3) if data[i] > 127)
    return w, h, lit


def check_metadata():
    with open(GENERATED_JSON) as f:
        meta = json.load(f)
    if meta.get("pipeline") != "expression-subgraph":
        fail("generated metadata is not from the expression-subgraph pipeline")
    if meta["text"] != typeset_expression_vm.DEFAULT_TEXT:
        fail("metadata text does not match typeset_expression_vm.DEFAULT_TEXT")
    if not meta["text"].startswith("But this rough magic"):
        fail("metadata text does not start with 'But this rough magic'")
    if "and, when" in meta["text"]:
        fail("unexpected comma present in 'and when'")
    if "Bury" in meta["text"]:
        fail("unexpected capitalization present in 'bury'")
    for word in EXPECTED_WORDS:
        if word not in meta["text"]:
            fail(f"expected word missing from metadata text: {word}")
    for typo in ["abrure", "heanIy", "heanly", "plurnmet"]:
        if typo in meta["text"]:
            fail(f"known typo present in metadata text: {typo}")
    reps = meta.get("representatives", {})
    for ch in sorted(set(meta["text"].replace(" ", ""))):
        if ch not in reps:
            fail(f"missing expression representative for {ch!r}")


def check_extraction():
    with open(LETTERS_JSON) as f:
        data = json.load(f)
    if len(data["letters"]) != 227:
        fail("unexpected extracted visual-unit count")
    if len(data["unique_letters"]) != 32:
        fail("unexpected unique visual-unit count")
    if not data["ascii_labels"]:
        fail("ASCII labels were not assigned")
    if not os.path.exists(UNIQUE_SHEET):
        fail("letters_unique_sheet.png is missing")


def check_vm_and_images():
    if not os.path.exists(GENERATED_VM):
        fail("generated_prospero.vm is missing")
    instructions = 0
    with open(GENERATED_VM) as f:
        for line in f:
            if line.strip() and not line.startswith("#"):
                instructions += 1
    if not (1000 < instructions < 50000):
        fail(f"unexpected generated instruction count: {instructions}")
    w, h, lit = count_lit_pixels(GENERATED_RENDER)
    if (w, h) != (1024, 1024):
        fail(f"generated_render.ppm has unexpected size {w}x{h}")
    if lit < 10000:
        fail(f"generated_render.ppm has too few lit pixels: {lit}")


def rerender():
    result = subprocess.run(
        ["make", "render", f"VM={GENERATED_VM.relative_to(REPO_ROOT)}", "PPM=/tmp/prospero_verify.ppm"],
        cwd=REPO_ROOT,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        check=False,
    )
    print(result.stdout, end="")
    if result.returncode != 0:
        fail("generated VM render failed")
    w, h, lit = count_lit_pixels("/tmp/prospero_verify.ppm")
    if (w, h) != (1024, 1024) or lit < 10000:
        fail("verification render image is invalid")


def main():
    check_metadata()
    check_extraction()
    check_vm_and_images()
    rerender()
    print("PASS: expression glyph bytecode and render verified")


if __name__ == "__main__":
    main()
