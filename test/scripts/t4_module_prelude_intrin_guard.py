#!/usr/bin/env python3
"""Ensure compiler intrinsic headers are guarded by their ABI/architecture."""

from __future__ import annotations

import importlib.util
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
GENERATOR = ROOT / "scripts" / "gen_module_prelude.py"
EXPECTED_GUARD = "#if defined(_MSC_VER) && __has_include(<intrin.h>)\n#include <intrin.h>\n#endif"
EXPECTED_EMMINTRIN_GUARD = (
    "#if (defined(__x86_64__) || defined(__i386__)) && "
    "__has_include(<emmintrin.h>)\n"
    "#include <emmintrin.h>\n"
    "#endif"
)
EXPECTED_MODULES = {
    "galay-http2",
    "galay-kernel",
    "galay-mcp",
    "galay-rpc",
}


def load_generator():
    sys.dont_write_bytecode = True
    spec = importlib.util.spec_from_file_location("gen_module_prelude", GENERATOR)
    if spec is None or spec.loader is None:
        raise AssertionError("unable to load gen_module_prelude.py")
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


def main() -> int:
    generator = load_generator()

    kernel = generator.Module(ROOT / "src/cpp/galay-kernel")
    kernel.collect()
    rendered = generator.render(kernel)
    if EXPECTED_GUARD not in rendered:
        raise AssertionError("generator must guard intrin.h with _MSC_VER")
    if EXPECTED_EMMINTRIN_GUARD not in rendered:
        raise AssertionError("generator must guard emmintrin.h to x86 targets")

    prelude_paths = sorted(ROOT.glob("src/cpp/*/module/module_prelude.hpp"))
    intrin_paths = {
        path.parent.parent.name
        for path in prelude_paths
        if "<intrin.h>" in path.read_text(encoding="utf-8")
    }
    if not EXPECTED_MODULES <= intrin_paths:
        raise AssertionError(
            f"missing expected modules containing intrin.h: "
            f"{sorted(EXPECTED_MODULES - intrin_paths)}"
        )

    for path in prelude_paths:
        text = path.read_text(encoding="utf-8")
        if "<intrin.h>" in text and EXPECTED_GUARD not in text:
            raise AssertionError(
                f"{path.relative_to(ROOT)} must use the MSVC-only intrin.h guard"
            )
        if "<emmintrin.h>" in text and EXPECTED_EMMINTRIN_GUARD not in text:
            raise AssertionError(
                f"{path.relative_to(ROOT)} must use the x86-only emmintrin.h guard"
            )
        if "#if __has_include(<intrin.h>)\n#include <intrin.h>" in text:
            raise AssertionError(
                f"{path.relative_to(ROOT)} still has an unconditional intrin.h guard"
            )

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
