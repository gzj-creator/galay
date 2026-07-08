#!/usr/bin/env python3
"""Compatibility entrypoint for the Linux executable matrix verifier."""

from __future__ import annotations

import importlib.util
import sys
from pathlib import Path


_IMPL_PATH = Path(__file__).resolve().parent / "common" / "105_verify_linux_exec_matrix.py"


def _load_impl():
    spec = importlib.util.spec_from_file_location("_galay_verify_linux_exec_matrix_impl", _IMPL_PATH)
    if spec is None or spec.loader is None:
        raise ImportError(f"unable to load {_IMPL_PATH}")
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


_IMPL = _load_impl()

for _name in dir(_IMPL):
    if _name.startswith("__"):
        continue
    globals()[_name] = getattr(_IMPL, _name)


if __name__ == "__main__":
    raise SystemExit(_IMPL.main(sys.argv[1:]))
