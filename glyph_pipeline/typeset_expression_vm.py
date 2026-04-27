#!/usr/bin/env python3
import argparse
import json
import sys

IMAGE_W = 1024
IMAGE_H = 1024

DEFAULT_TEXT = (
    "But this rough magic I here abjure, and when I have required Some heavenly music, "
    "which even now I do, To work mine end upon their senses that This airy charm is for, "
    "I'll break my staff, bury it certain fathoms in the earth, And deeper than did ever "
    "plummet sound I'll drown my book."
)

DESCENDERS = set("gjpqy")
NARROW = set("i'l,.")

# These are source letter indices whose expression subgraphs visibly correspond to
# the character. They correct the connected-component label shift in letters.json
# without synthesizing or bitmap-repairing glyphs.
SOURCE_REPRESENTATIVES = {
    "'": 1,
    ",": 201,
    ".": 15,
    "A": 38,
    "B": 209,
    "I": 226,
    "S": 187,
    "T": 141,
    "a": 222,
    "b": 196,
    "c": 225,
    "d": 204,
    "e": 192,
    "f": 82,
    "g": 219,
    "h": 213,
    "i": 214,
    "j": 197,
    "k": 14,
    "l": 2,
    "m": 221,
    "n": 203,
    "o": 217,
    "p": 20,
    "q": 181,
    "r": 216,
    "s": 215,
    "t": 211,
    "u": 210,
    "v": 177,
    "w": 205,
    "y": 158,
}


class SourceInstr:
    def __init__(self, ident, op, args, line):
        self.ident = ident
        self.op = op
        self.args = args
        self.line = line


class VmBuilder:
    def __init__(self):
        self.lines = []
        self.next_id = 0
        self.x = self.emit("var-x")
        self.y = self.emit("var-y")

    def emit(self, op, *args):
        ident = f"_{self.next_id}"
        self.next_id += 1
        self.lines.append(" ".join([ident, op, *map(str, args)]))
        return ident

    def const(self, value):
        return self.emit("const", f"{value:.9g}")

    def add(self, a, b):
        return self.emit("add", a, b)

    def sub(self, a, b):
        return self.emit("sub", a, b)

    def mul(self, a, b):
        return self.emit("mul", a, b)

    def union(self, roots):
        roots = [r for r in roots if r is not None]
        if not roots:
            return self.const(1.0)
        root = roots[0]
        for term in roots[1:]:
            root = self.emit("min", root, term)
        return root


def parse_source_lines(lines, source_name="<embedded glyph_program>"):
    instrs = []
    id_to_index = {}
    for line_no, line in enumerate(lines, 1):
        raw = line.strip()
        if not raw or raw.startswith("#"):
            continue
        parts = raw.split()
        if len(parts) < 2:
            continue
        ident, op, args = parts[0], parts[1], parts[2:]
        id_to_index[ident] = len(instrs)
        instrs.append(SourceInstr(ident, op, args, f"{source_name}:{line_no}"))
    return instrs, id_to_index


def load_embedded_program(data):
    program = data.get("glyph_program")
    if program and program.get("format") == "vm-lines":
        return parse_source_lines(program.get("instructions", []))
    raise ValueError(
        "letters JSON does not contain an embedded vm-lines glyph_program; rerun `make extract`"
    )


def px_to_render(px, size):
    return -1.0 + 2.0 * px / (size - 1)


def glyph_score(letter):
    ch = letter["char"]
    x0, y0, x1, y1 = letter["bbox_px"]
    w, h = x1 - x0 + 1, y1 - y0 + 1
    score = 0
    if ch in DESCENDERS:
        score += abs(h - 60) * 40
        score += abs(w - 34) * 12
    elif ch in NARROW:
        target_w = 8 if ch != "." else 9
        target_h = 55 if ch in "il" else 18 if ch in "'," else 9
        score += abs(w - target_w) * 80 + abs(h - target_h) * 20
    elif ch.isupper() or ch in "bdfhkt":
        score += abs(h - 62) * 35 + abs(w - 32) * 10
    else:
        score += abs(h - 34) * 35 + abs(w - 34) * 10
    score += letter["index"] * 0.01
    return score


def load_glyphs(letters_path):
    with open(letters_path) as f:
        data = json.load(f)
    by_index = {letter["index"]: letter for letter in data["letters"]}
    glyphs = {}
    for ch, index in SOURCE_REPRESENTATIVES.items():
        if index in by_index:
            letter = dict(by_index[index])
            letter["char"] = ch
            glyphs[ch] = letter
    for letter in data["letters"]:
        ch = letter.get("char")
        if not ch or ch in glyphs:
            continue
        if ch not in glyphs or glyph_score(letter) < glyph_score(glyphs[ch]):
            glyphs[ch] = letter
    return data, glyphs


def glyph_metrics(letter, scale):
    x0, y0, x1, y1 = letter["bbox_px"]
    w = max(1.0, (x1 - x0 + 1) * scale)
    h = max(1.0, (y1 - y0 + 1) * scale)
    ch = letter["char"]
    if ch in DESCENDERS:
        baseline = min(h, 35 * scale)
    elif ch == "'":
        baseline = 58 * scale
    elif ch in ".,'":
        baseline = h
    else:
        baseline = h
    return w, h, baseline


def text_width(text, glyphs, scale, tracking, space_width):
    width = 0.0
    for ch in text:
        if ch == " ":
            width += space_width
        elif ch in glyphs:
            w, _, _ = glyph_metrics(glyphs[ch], scale)
            width += w + tracking
    return width


def layout_text(text, glyphs, scale, margin_x, margin_y, max_width, tracking, space_width, line_gap):
    metrics = {ch: glyph_metrics(letter, scale) for ch, letter in glyphs.items()}
    max_ascent = max(baseline for _, _, baseline in metrics.values())
    max_descent = max(h - baseline for _, h, baseline in metrics.values())
    line_h = max_ascent + max_descent + line_gap
    x = float(margin_x)
    y = float(margin_y)
    placed = []
    for word in text.replace("\n", " \n ").split(" "):
        if not word:
            continue
        if word == "\n":
            x = float(margin_x)
            y += line_h
            continue
        word_w = text_width(word, glyphs, scale, tracking, space_width)
        if x > margin_x and x + word_w > margin_x + max_width:
            x = float(margin_x)
            y += line_h
        for ch in word:
            if ch not in glyphs:
                raise ValueError(f"missing glyph expression for {ch!r}")
            letter = glyphs[ch]
            w, h, baseline = metrics[ch]
            top = y + max_ascent - baseline
            placed.append((ch, letter, x, top, w, h))
            x += w + tracking
        x += space_width
    return placed


def transformed_axis(builder, out_var, src0, src1, dst0, dst1):
    dst_len = dst1 - dst0
    if abs(dst_len) < 1e-9:
        return builder.const(src0)
    scale = (src1 - src0) / dst_len
    return builder.add(builder.const(src0), builder.mul(builder.sub(out_var, builder.const(dst0)), builder.const(scale)))


def clone_expr(builder, instrs, id_to_index, source_id, x_expr, y_expr, memo):
    if source_id in memo:
        return memo[source_id]
    instr = instrs[id_to_index[source_id]]
    if instr.op == "var-x":
        memo[source_id] = x_expr
        return x_expr
    if instr.op == "var-y":
        memo[source_id] = y_expr
        return y_expr
    if instr.op == "const":
        out = builder.emit("const", instr.args[0])
        memo[source_id] = out
        return out
    args = []
    for arg in instr.args:
        if arg.startswith("_"):
            args.append(clone_expr(builder, instrs, id_to_index, arg, x_expr, y_expr, memo))
        else:
            args.append(arg)
    out = builder.emit(instr.op, *args)
    memo[source_id] = out
    return out


def emit_placed_glyph(builder, instrs, id_to_index, letter, x, y, w, h, flip_y):
    sx0, sy0, sx1, sy1 = letter["bbox_render"]
    dx0 = px_to_render(x, IMAGE_W)
    dx1 = px_to_render(x + w - 1, IMAGE_W)
    dy0 = px_to_render(y, IMAGE_H)
    dy1 = px_to_render(y + h - 1, IMAGE_H)
    src_x = transformed_axis(builder, builder.x, sx0, sx1, dx0, dx1)
    src_y = transformed_axis(builder, builder.y, sy1 if flip_y else sy0, sy0 if flip_y else sy1, dy0, dy1)
    memo = {}
    roots = [clone_expr(builder, instrs, id_to_index, root, src_x, src_y, memo) for root in letter["terms"]]
    return builder.union(roots)


def write_glyph_table(path, instrs, glyphs):
    with open(path, "w") as f:
        f.write("# Generated by typeset_expression_vm.py. C-readable glyph table.\n")
        f.write(f"program vm-lines {len(instrs)}\n")
        for instr in instrs:
            f.write(" ".join(["instr", instr.ident, instr.op, *instr.args]) + "\n")
        f.write(f"count {len(glyphs)}\n")
        for ch, letter in sorted(glyphs.items()):
            x0, y0, x1, y1 = letter["bbox_px"]
            sx0, sy0, sx1, sy1 = letter["bbox_render"]
            terms = letter["terms"]
            fields = [
                "glyph",
                str(ord(ch)),
                str(letter["index"]),
                str(x0),
                str(y0),
                str(x1),
                str(y1),
                f"{sx0:.9g}",
                f"{sy0:.9g}",
                f"{sx1:.9g}",
                f"{sy1:.9g}",
                str(len(terms)),
                *terms,
            ]
            f.write(" ".join(fields) + "\n")


def write_vm(
    path,
    letters_path,
    text,
    scale,
    margin_x,
    margin_y,
    max_width,
    tracking,
    space_width,
    line_gap,
    flip_y,
    metadata_path,
    glyph_table_path,
):
    data, glyphs = load_glyphs(letters_path)
    instrs, id_to_index = load_embedded_program(data)
    placed = layout_text(text, glyphs, scale, margin_x, margin_y, max_width, tracking, space_width, line_gap)
    builder = VmBuilder()
    roots = []
    for _, letter, x, y, w, h in placed:
        roots.append(emit_placed_glyph(builder, instrs, id_to_index, letter, x, y, w, h, flip_y))
    builder.union(roots)
    with open(path, "w") as f:
        f.write("# Generated by typeset_expression_vm.py from embedded glyph expression subgraphs.\n")
        f.write(f"# letters={letters_path} glyphs={len(placed)} instructions={builder.next_id}\n")
        for line in builder.lines:
            f.write(line + "\n")
    with open(metadata_path, "w") as f:
        json.dump(
            {
                "pipeline": "expression-subgraph",
                "letters": letters_path,
                "glyph_program_source": data.get("source"),
                "glyph_program_instructions": len(instrs),
                "text": text,
                "scale": scale,
                "flip_y": flip_y,
                "glyph_count": len(placed),
                "instructions": builder.next_id,
                "representatives": {ch: letter["index"] for ch, letter in sorted(glyphs.items())},
                "reference_contact_sheet": data.get("unique_contact_sheet"),
            },
            f,
            indent=2,
        )
        f.write("\n")
    if glyph_table_path:
        write_glyph_table(glyph_table_path, instrs, glyphs)
    return builder.next_id, len(placed)


def main(argv):
    parser = argparse.ArgumentParser(description="Typeset text by reusing extracted Prospero glyph expression subgraphs.")
    parser.add_argument("--letters", default="letters.json")
    parser.add_argument("--out", default="generated_prospero.vm")
    parser.add_argument("--metadata", default="generated_prospero.json")
    parser.add_argument("--glyph-table", default=None)
    parser.add_argument("--text", default=DEFAULT_TEXT)
    parser.add_argument("--scale", type=float, default=0.76)
    parser.add_argument("--margin-x", type=float, default=36)
    parser.add_argument("--margin-y", type=float, default=54)
    parser.add_argument("--max-width", type=float, default=955)
    parser.add_argument("--tracking", type=float, default=4)
    parser.add_argument("--space-width", type=float, default=18)
    parser.add_argument("--line-gap", type=float, default=9)
    parser.add_argument("--no-flip-y", action="store_true")
    args = parser.parse_args(argv)
    instructions, glyphs = write_vm(
        args.out,
        args.letters,
        args.text,
        args.scale,
        args.margin_x,
        args.margin_y,
        args.max_width,
        args.tracking,
        args.space_width,
        args.line_gap,
        not args.no_flip_y,
        args.metadata,
        args.glyph_table,
    )
    print(f"wrote {args.out}: glyphs={glyphs} instructions={instructions}")
    print(f"wrote {args.metadata}")
    if args.glyph_table:
        print(f"wrote {args.glyph_table}")


if __name__ == "__main__":
    main(sys.argv[1:])
