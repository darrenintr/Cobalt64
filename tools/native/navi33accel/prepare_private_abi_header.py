#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Make a temporary Navi33-namespaced view of the private KC model header.

The source header and generated result are Apple-derived and must stay outside
the repository. The rewrite changes only the layout tool's N48-prefixed
identifiers to N33-prefixed identifiers; it does not import Navi48 source or
GPU constants.
"""
from pathlib import Path
import sys


def main() -> int:
    if len(sys.argv) != 3:
        print("usage: prepare_private_abi_header.py EVIDENCE_DIR OUTPUT_DIR",
              file=sys.stderr)
        return 2
    evidence = Path(sys.argv[1]).expanduser().resolve()
    output_dir = Path(sys.argv[2]).expanduser().resolve()
    repo = Path(__file__).resolve().parents[3]
    source = evidence / "layout-out" / "IOAccelFamily2_decl.h"
    output = output_dir / "IOAccelFamily2_decl.h"
    if not source.is_file():
        print(f"missing target-derived layout header: {source}", file=sys.stderr)
        return 2
    if output == repo or repo in output.parents:
        print("refusing to write Apple-derived output inside the repository",
              file=sys.stderr)
        return 2

    content = source.read_text()
    if "N48_IOGraphicsAccelerator2_SIZE" not in content:
        print("source does not contain the expected target class model",
              file=sys.stderr)
        return 2
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(content.replace("N48", "N33"))
    print(output)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
