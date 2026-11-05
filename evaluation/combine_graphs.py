#!/usr/bin/env python3
"""Combine evaluation SVGs into three columns: all, large, and small flows."""

import argparse
from copy import deepcopy
import math
from pathlib import Path
import re
import sys
import xml.etree.ElementTree as ET

SVG = "http://www.w3.org/2000/svg"
XLINK = "http://www.w3.org/1999/xlink"
WORKLOADS = ("uniform", "pareto", "enterprise", "datamining")
CATEGORIES = ("all", "large", "small")
ET.register_namespace("", SVG)
ET.register_namespace("xlink", XLINK)


def dimensions(root):
    if root.tag != f"{{{SVG}}}svg":
        raise ValueError("Input must be an SVG document")
    viewbox = root.get("viewBox")
    if viewbox:
        bounds = [float(value) for value in viewbox.replace(",", " ").split()]
        if len(bounds) != 4 or not all(math.isfinite(value) for value in bounds):
            raise ValueError("Invalid SVG viewBox")
        width, height = bounds[2:]
    else:
        width = float(root.get("width", "").removesuffix("px"))
        height = float(root.get("height", "").removesuffix("px"))
        root.set("viewBox", f"0 0 {width:g} {height:g}")
    if not math.isfinite(width) or not math.isfinite(height) or min(width, height) <= 0:
        raise ValueError("SVG dimensions must be finite and positive")
    return width, height


def isolate_ids(root, prefix):
    """Keep clip paths, markers, and other local references distinct per panel."""
    ids = {element.get("id"): prefix + element.get("id")
           for element in root.iter() if element.get("id")}
    urls = re.compile(r"url\(\s*(['\"]?)#([^'\")\s]+)\1\s*\)")

    def rewrite(value):
        return urls.sub(lambda match: f"url(#{ids.get(match[2], match[2])})", value)

    for element in root.iter():
        for key, value in list(element.attrib.items()):
            if key == "id":
                element.set(key, ids[value])
            elif key in ("href", f"{{{XLINK}}}href") and value.startswith("#"):
                element.set(key, "#" + ids.get(value[1:], value[1:]))
            else:
                element.set(key, rewrite(value))
        if element.tag == f"{{{SVG}}}style" and element.text:
            text = rewrite(element.text)
            for old, new in ids.items():
                # ID selectors occur before a rule; leave ordinary color values alone.
                text = re.sub(r"#" + re.escape(old) + r"(?=[\s,.>:\[\{])", "#" + new, text)
            element.text = text


def combine(directory, output, workloads=WORKLOADS, gap=16):
    directory, output = Path(directory), Path(output)
    if not math.isfinite(gap) or gap < 0:
        raise ValueError("Gap must be finite and nonnegative")
    panels = []
    for workload in workloads:
        for category in CATEGORIES:
            source = directory / f"{workload}-{category}.svg"
            if source.resolve() == output.resolve():
                raise ValueError("Output must not replace an input graph")
            root = ET.parse(source).getroot()
            panels.append((root, dimensions(root)))
    panel_width = max(size[0] for _, size in panels)
    panel_height = max(size[1] for _, size in panels)
    width = 3 * panel_width + 4 * gap
    height = len(workloads) * panel_height + (len(workloads) + 1) * gap
    result = ET.Element(f"{{{SVG}}}svg", {
        "width": f"{width:g}", "height": f"{height:g}", "viewBox": f"0 0 {width:g} {height:g}"})
    ET.SubElement(result, f"{{{SVG}}}title").text = "ECMP versus CONGA: all, large, and small flows"
    ET.SubElement(result, f"{{{SVG}}}desc").text = (
        "Columns: all, large, small. Rows: " + ", ".join(workloads) + ". Original graph axes and data are preserved.")
    ET.SubElement(result, f"{{{SVG}}}rect", {"width": "100%", "height": "100%", "fill": "white"})
    for index, (original, _) in enumerate(panels):
        panel = deepcopy(original)
        isolate_ids(panel, f"panel-{index}-")
        panel.set("x", f"{gap + (index % 3) * (panel_width + gap):g}")
        panel.set("y", f"{gap + (index // 3) * (panel_height + gap):g}")
        panel.set("width", f"{panel_width:g}")
        panel.set("height", f"{panel_height:g}")
        panel.set("preserveAspectRatio", "xMidYMid meet")
        result.append(panel)
    output.parent.mkdir(parents=True, exist_ok=True)
    ET.ElementTree(result).write(output, encoding="utf-8", xml_declaration=True)
    return len(panels)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", type=Path, required=True, help="directory containing workload-category.svg files")
    parser.add_argument("--output", type=Path,
                        help="output SVG, or filename base for separate rows (default: INPUT/combined.svg)")
    parser.add_argument("--separate-rows", action="store_true",
                        help="write one three-panel SVG per workload, named OUTPUTSTEM-WORKLOAD.svg")
    parser.add_argument("--workload", choices=("all", *WORKLOADS), default="all",
                        help="all workloads in four rows, or one workload in a single row")
    parser.add_argument("--gap", type=float, default=16, help="panel spacing and outside margin (default: 16)")
    args = parser.parse_args(argv)
    workloads = WORKLOADS if args.workload == "all" else (args.workload,)
    output = args.output or args.input / "combined.svg"
    try:
        outputs = [(output, workloads)]
        if args.separate_rows:
            outputs = [(output.with_name(f"{output.stem}-{workload}{output.suffix or '.svg'}"), (workload,))
                       for workload in workloads]
        for destination, selected in outputs:
            count = combine(args.input, destination, selected, args.gap)
            print(f"Wrote {destination}: {count} graphs, 3 columns × {len(selected)} rows")
    except (OSError, ValueError, ET.ParseError) as error:
        print(f"Cannot combine graphs: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
