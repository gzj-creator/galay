#!/usr/bin/env python3
"""Audit examples, tests, benchmarks, and scripts style drift."""

from __future__ import annotations

import os
import re
import stat
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
EXAMPLE_RE = re.compile(r"^e[0-9]+_[a-z0-9]+(?:_[a-z0-9]+)*\.cc$")
TEST_RE = re.compile(r"^t[0-9]+_[a-z0-9]+(?:_[a-z0-9]+)*\.cc$")
BENCHMARK_RE = re.compile(r"^b[0-9]+_[a-z0-9]+(?:_[a-z0-9]+)*\.cc$")
BENCHMARK_SUPPORT_RE = re.compile(r"^(?:bench_support|ssl_stats)\.cc$")
SCRIPT_RE = re.compile(r"^[0-9]{3}_[a-z0-9]+(?:_[a-z0-9]+)*\.(?:sh|py|cmake|in|yml|yaml|json|md|txt|conf|crt|key|srl)$")
GROUPED_UTIL_TEST_RE = re.compile(r"^[a-z0-9]+(?:_[a-z0-9]+)*_test\.cc$")
LEGACY_SCRIPT_RE = re.compile(r"S[0-9]+-Run|RunIntegrationTest|RunHttpIntegrationTest")
TARGET_BENCH_RE = re.compile(r"add_executable\s*\(\s*([A-Za-z0-9_.:-]+)")


issues: list[str] = []
advisories: list[str] = []


def rel(path: Path) -> str:
    return path.relative_to(ROOT).as_posix()


def issue(path: Path, line: int, rule: str, message: str) -> None:
    issues.append(f"{rel(path)}:{line}: {rule}: {message}")


def advisory(path: Path, line: int, rule: str, message: str) -> None:
    advisories.append(f"{rel(path)}:{line}: {rule}: {message}")


def iter_files(*roots: str) -> list[Path]:
    files: list[Path] = []
    for root in roots:
        base = ROOT / root
        if base.exists():
            files.extend(path for path in base.rglob("*") if path.is_file())
    return sorted(files)


def text_lines(path: Path) -> list[str]:
    try:
        return path.read_text(encoding="utf-8").splitlines()
    except UnicodeDecodeError:
        return []


def audit_examples() -> None:
    base = ROOT / "examples"
    if not base.exists():
        return

    for module_dir in sorted(path for path in base.iterdir() if path.is_dir()):
        include_names = {
            path.name for path in (module_dir / "include").glob("*") if path.suffix in {".cc", ".cpp"}
        }
        import_names = {
            path.name for path in (module_dir / "import").glob("*") if path.suffix in {".cc", ".cpp"}
        }

        for side in ("include", "import"):
            side_dir = module_dir / side
            if not side_dir.exists():
                continue
            for path in sorted(side_dir.iterdir()):
                if not path.is_file() or path.suffix not in {".cc", ".cpp"}:
                    continue
                if path.suffix == ".cpp":
                    issue(path, 1, "cpp-extension", "example should use .cc")
                if not EXAMPLE_RE.match(path.name):
                    issue(path, 1, "example-name", "example should match eNN_<scenario>.cc")

        include_stems = {Path(name).with_suffix(".cc").stem for name in include_names}
        import_stems = {Path(name).with_suffix(".cc").stem for name in import_names}
        optional_include_pairs = {"e6_mpsc", "e7_unsafe", "e8_async", "e9_sleep"}
        for stem in sorted(include_stems ^ import_stems):
            if module_dir.name == "kernel" and stem in optional_include_pairs:
                continue
            side = "include" if stem in include_stems else "import"
            issue(module_dir / side, 1, "example-pair", f"missing paired example for {stem}")


def audit_tests() -> None:
    base = ROOT / "test"
    if not base.exists():
        return

    for path in sorted(base.rglob("*")):
        if not path.is_file() or path.suffix not in {".cc", ".cpp"}:
            continue
        if path.suffix == ".cpp":
            issue(path, 1, "cpp-extension", "test should use .cc")
        parent = path.parent
        relative_parts = path.relative_to(base).parts
        if "package" in relative_parts:
            continue
        grouped_utils = (
            len(path.relative_to(base).parts) == 3
            and path.relative_to(base).parts[0] == "utils"
            and GROUPED_UTIL_TEST_RE.match(path.name)
        )
        approved_single_tests = {
            "test/config/t_module_config_surface.cc",
            "test/utils/t_import_smoke.cc",
        }
        if not grouped_utils and rel(path) not in approved_single_tests and not TEST_RE.match(path.name):
            issue(path, 1, "test-name", "test should match tNN_<scenario>.cc")
        if grouped_utils and path.suffix != ".cc":
            issue(path, 1, "test-name", "grouped utility test should use .cc")

    for cmake in sorted(base.rglob("CMakeLists.txt")):
        lines = text_lines(cmake)
        for idx, line in enumerate(lines, 1):
            if re.search(r"\bproject\s*\(\s*test\s*\)", line):
                issue(cmake, idx, "cmake-project-test", "subdirectory CMake should not call project(test)")
        content = "\n".join(lines)
        if "package" not in cmake.relative_to(base).parts and "add_executable" in content and "add_test" not in content:
            issue(cmake, 1, "cmake-add-test", "test CMake creates executables without add_test")


def audit_benchmarks() -> None:
    base = ROOT / "benchmark"
    if not base.exists():
        return

    for path in sorted(base.rglob("*")):
        if path.is_dir() and path.name == "compare":
            if path.relative_to(ROOT).as_posix() == "benchmark/tracing/compare":
                continue
            issue(path, 1, "benchmark-compare", "benchmark compare directory should be deleted unless maintained")
            continue
        if not path.is_file() or path.suffix not in {".cc", ".cpp"}:
            continue
        if BENCHMARK_SUPPORT_RE.match(path.name):
            continue
        if path.suffix == ".cpp":
            issue(path, 1, "cpp-extension", "benchmark should use .cc")
        if path.name.endswith("_benchmark.cpp") or not BENCHMARK_RE.match(path.name):
            issue(path, 1, "benchmark-name", "benchmark should match bNN_<clear_scenario>.cc")

    for cmake in sorted(base.rglob("CMakeLists.txt")):
        module = cmake.parent.name
        for idx, line in enumerate(text_lines(cmake), 1):
            match = TARGET_BENCH_RE.search(line)
            if match:
                target = match.group(1).strip('"${}')
                if not target.startswith(f"benchmark_{module}_"):
                    issue(cmake, idx, "benchmark-target", f"benchmark target should start with benchmark_{module}_")


def audit_scripts() -> None:
    base = ROOT / "scripts"
    if not base.exists():
        return

    for path in sorted(base.rglob("*")):
        if not path.is_file():
            continue
        if "__pycache__" in path.parts or path.suffix == ".pyc":
            continue
        if path.parent == base:
            issue(path, 1, "script-location", "script files should live in a category subdirectory")
        if not SCRIPT_RE.match(path.name):
            issue(path, 1, "script-name", "script name should start with a unique NNN_ prefix and use lower snake_case")
        if LEGACY_SCRIPT_RE.search(path.name):
            issue(path, 1, "legacy-script-name", "legacy stage-coded script name should be renamed")
        if path.suffix == ".sh":
            mode = path.stat().st_mode
            if not mode & stat.S_IXUSR:
                issue(path, 1, "script-executable", "shell script should be executable")
        if path.suffix in {".sh", ".py"}:
            lines = text_lines(path)
            if path.suffix == ".sh":
                if not lines or lines[0] != "#!/usr/bin/env bash":
                    issue(path, 1, "script-shebang", "shell script should use /usr/bin/env bash")
                if len(lines) < 2 or lines[1] != "set -euo pipefail":
                    issue(path, 2, "script-strict-mode", "shell script should set -euo pipefail")
            if path.suffix == ".py" and (not lines or lines[0] != "#!/usr/bin/env python3"):
                issue(path, 1, "script-shebang", "python script should use /usr/bin/env python3")


def audit_text_patterns() -> None:
    scanned_roots = ["examples", "test", "benchmark", "scripts", "cmake"]
    for path in iter_files(*scanned_roots):
        if path.suffix not in {".cc", ".cpp", ".h", ".hpp", ".cmake", ".txt", ".md", ".sh", ".py"} and path.name != "CMakeLists.txt":
            continue
        for idx, line in enumerate(text_lines(path), 1):
            if path.parts and any(part in {"examples", "test", "benchmark"} for part in path.relative_to(ROOT).parts):
                if re.search(r"^\s*using\s+namespace\s+", line):
                    advisory(path, idx, "global-using-namespace", "avoid global using namespace in examples/tests/benchmarks")
                if "@brief" in line:
                    advisory(path, idx, "doxygen-brief", "avoid @brief in examples/tests/benchmarks")
            if path != Path(__file__).resolve() and LEGACY_SCRIPT_RE.search(line):
                issue(path, idx, "legacy-script-reference", "legacy script reference should be updated")


def main() -> int:
    audit_examples()
    audit_tests()
    audit_benchmarks()
    audit_scripts()
    audit_text_patterns()
    for item in sorted(set(issues)):
        print(item)
    if advisories:
        print(f"advisory-summary: {len(set(advisories))} non-blocking comment/namespace style items remain")
    return 1 if issues else 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except BrokenPipeError:
        sys.exit(1)
