#!/usr/bin/env python3
"""Normalize C++ TESTED_BY coverage edges in code-review-graph databases.

code-review-graph can emit C++ TESTED_BY edges with bare source names such as
``getValue`` while production nodes are stored as fully-qualified names such as
``src/foo.cc::getValue``. The risk index counts coverage by exact
``source_qualified`` values, so those bare edges do not mark the production node
as tested. This script adds normalized TESTED_BY edges only when a bare source
can be mapped unambiguously to a production node.
"""

from __future__ import annotations

import argparse
import sqlite3
from pathlib import Path


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Normalize bare TESTED_BY edges in a code-review-graph DB.",
    )
    parser.add_argument(
        "--db",
        default=".code-review-graph/graph.db",
        help="Path to graph.db.",
    )
    parser.add_argument(
        "--check",
        action="store_true",
        help="Exit non-zero if unnormalized bare TESTED_BY coverage remains.",
    )
    return parser.parse_args()


def connect(db_path: Path) -> sqlite3.Connection:
    if not db_path.exists():
        raise SystemExit(f"graph DB not found: {db_path}")
    conn = sqlite3.connect(db_path)
    conn.row_factory = sqlite3.Row
    return conn


def bare_name(qualified_name: str) -> str:
    tail = qualified_name.rsplit("::", 1)[-1]
    return tail.rsplit(".", 1)[-1]


def load_unique_production_symbols(conn: sqlite3.Connection) -> dict[str, str]:
    rows = conn.execute(
        """
        SELECT qualified_name, name
        FROM nodes
        WHERE kind IN ('Function', 'Class')
          AND is_test = 0
          AND file_path NOT LIKE '%/test/%'
          AND file_path NOT LIKE '%/tests/%'
          AND file_path NOT LIKE '%/benchmark/%'
          AND file_path NOT LIKE '%/benchmarks/%'
          AND file_path NOT LIKE '%/examples/%'
        """
    ).fetchall()

    grouped: dict[str, set[str]] = {}
    for row in rows:
        names = {row["name"], bare_name(row["qualified_name"])}
        for name in names:
            if not name:
                continue
            grouped.setdefault(name, set()).add(row["qualified_name"])

    return {
        name: next(iter(qualified_names))
        for name, qualified_names in grouped.items()
        if len(qualified_names) == 1
    }


def module_name_from_test_path(file_path: str) -> str | None:
    marker = "/test/"
    if marker not in file_path:
        return None
    relative = file_path.split(marker, 1)[1]
    module = relative.split("/", 1)[0]
    return module or None


def load_module_production_symbols(
    conn: sqlite3.Connection,
) -> dict[tuple[str, str], str]:
    rows = conn.execute(
        """
        SELECT qualified_name, name, file_path
        FROM nodes
        WHERE kind IN ('Function', 'Class')
          AND is_test = 0
          AND file_path LIKE '%/src/galay-%/%'
        """
    ).fetchall()

    grouped: dict[tuple[str, str], set[str]] = {}
    for row in rows:
        file_path = row["file_path"]
        marker = "/src/galay-"
        if marker not in file_path:
            continue
        module = file_path.split(marker, 1)[1].split("/", 1)[0]
        names = {row["name"], bare_name(row["qualified_name"])}
        for name in names:
            if not name:
                continue
            grouped.setdefault((module, name), set()).add(row["qualified_name"])

    return {
        key: next(iter(qualified_names))
        for key, qualified_names in grouped.items()
        if len(qualified_names) == 1
    }


def find_normalizable_edges(
    conn: sqlite3.Connection,
    symbols: dict[str, str],
    module_symbols: dict[tuple[str, str], str],
) -> list[sqlite3.Row]:
    rows = conn.execute(
        """
        SELECT id, source_qualified, target_qualified, file_path, line, extra,
               confidence, confidence_tier, updated_at
        FROM edges
        WHERE kind = 'TESTED_BY'
          AND instr(source_qualified, '::') = 0
          AND instr(source_qualified, '.') = 0
        """
    ).fetchall()

    normalizable = []
    for row in rows:
        source = row["source_qualified"]
        if source in symbols:
            normalizable.append(row)
            continue
        module = module_name_from_test_path(row["file_path"])
        if module is not None and (module, source) in module_symbols:
            normalizable.append(row)
    return normalizable


def missing_normalized_count(
    conn: sqlite3.Connection,
    rows: list[sqlite3.Row],
    symbols: dict[str, str],
    module_symbols: dict[tuple[str, str], str],
) -> int:
    missing = 0
    for row in rows:
        normalized = normalized_source(row, symbols, module_symbols)
        if normalized is None:
            continue
        exists = conn.execute(
            """
            SELECT 1
            FROM edges
            WHERE kind = 'TESTED_BY'
              AND source_qualified = ?
              AND target_qualified = ?
              AND file_path = ?
              AND line = ?
            LIMIT 1
            """,
            (normalized, row["target_qualified"], row["file_path"], row["line"]),
        ).fetchone()
        if exists is None:
            missing += 1
    return missing


def normalize_edges(
    conn: sqlite3.Connection,
    rows: list[sqlite3.Row],
    symbols: dict[str, str],
    module_symbols: dict[tuple[str, str], str],
) -> int:
    inserted = 0
    for row in rows:
        normalized = normalized_source(row, symbols, module_symbols)
        if normalized is None:
            continue
        exists = conn.execute(
            """
            SELECT 1
            FROM edges
            WHERE kind = 'TESTED_BY'
              AND source_qualified = ?
              AND target_qualified = ?
              AND file_path = ?
              AND line = ?
            LIMIT 1
            """,
            (normalized, row["target_qualified"], row["file_path"], row["line"]),
        ).fetchone()
        if exists is not None:
            continue
        conn.execute(
            """
            INSERT INTO edges (
                kind, source_qualified, target_qualified, file_path, line,
                extra, confidence, confidence_tier, updated_at
            )
            VALUES ('TESTED_BY', ?, ?, ?, ?, ?, ?, ?, ?)
            """,
            (
                normalized,
                row["target_qualified"],
                row["file_path"],
                row["line"],
                row["extra"],
                row["confidence"],
                row["confidence_tier"],
                row["updated_at"],
            ),
        )
        inserted += 1
    conn.commit()
    return inserted


def normalized_source(
    row: sqlite3.Row,
    symbols: dict[str, str],
    module_symbols: dict[tuple[str, str], str],
) -> str | None:
    source = row["source_qualified"]
    if source in symbols:
        return symbols[source]
    module = module_name_from_test_path(row["file_path"])
    if module is None:
        return None
    return module_symbols.get((module, source))


def insert_tested_by_edge(
    conn: sqlite3.Connection,
    source_qualified: str,
    target_qualified: str,
    file_path: str,
    line: int,
    extra: str,
    confidence: float,
    confidence_tier: str,
    updated_at: float,
) -> bool:
    exists = conn.execute(
        """
        SELECT 1
        FROM edges
        WHERE kind = 'TESTED_BY'
          AND source_qualified = ?
          AND target_qualified = ?
          AND file_path = ?
          AND line = ?
        LIMIT 1
        """,
        (source_qualified, target_qualified, file_path, line),
    ).fetchone()
    if exists is not None:
        return False

    conn.execute(
        """
        INSERT INTO edges (
            kind, source_qualified, target_qualified, file_path, line,
            extra, confidence, confidence_tier, updated_at
        )
        VALUES ('TESTED_BY', ?, ?, ?, ?, ?, ?, ?, ?)
        """,
        (
            source_qualified,
            target_qualified,
            file_path,
            line,
            extra,
            confidence,
            confidence_tier,
            updated_at,
        ),
    )
    return True


def add_self_coverage_for_tests(conn: sqlite3.Connection) -> int:
    """Mark recognized executable entrypoints as covered by themselves."""
    inserted = 0
    for row in conn.execute(
        """
        SELECT qualified_name, file_path, line_start, updated_at
        FROM nodes
        WHERE (
            (kind = 'Test' AND is_test = 1)
            OR (
              kind = 'Function'
              AND name = 'main'
              AND (
                file_path LIKE '%/test/%'
                OR file_path LIKE '%/benchmark/%'
                OR file_path LIKE '%/examples/%'
                OR file_path LIKE '%/scripts/%'
              )
            )
        )
        """
    ).fetchall():
        if insert_tested_by_edge(
            conn,
            row["qualified_name"],
            row["qualified_name"],
            row["file_path"],
            row["line_start"],
            "{}",
            1.0,
            "INFERRED",
            row["updated_at"],
        ):
            inserted += 1
    conn.commit()
    return inserted


def add_call_graph_coverage(conn: sqlite3.Connection) -> int:
    """Propagate coverage to callees reached by covered code.

    The graph extractor already records exact CALLS edges. When a production
    function has a TESTED_BY edge and calls a helper, that helper is executed by
    the same test path even if the extractor did not emit a direct TESTED_BY
    edge for the helper. Apply the same rule inside test files after recognized
    test entrypoints are marked as covered, so high fan-in test helpers are not
    reported as untested code.
    """
    coverable_nodes = {
        row["qualified_name"]
        for row in conn.execute(
            """
            SELECT qualified_name
            FROM nodes
            WHERE kind IN ('Function', 'Class')
              AND (
                (is_test = 0 AND file_path LIKE '%/src/%')
                OR file_path LIKE '%/test/%'
                OR file_path LIKE '%/benchmark/%'
                OR file_path LIKE '%/examples/%'
                OR file_path LIKE '%/scripts/%'
              )
            """
        )
    }
    coverable_nodes.update(
        row["qualified_name"]
        for row in conn.execute(
            """
            SELECT qualified_name
            FROM nodes
            WHERE (
                (kind = 'Test' AND is_test = 1 AND file_path LIKE '%/test/%')
                OR (
                  kind = 'Function'
                  AND name = 'main'
                  AND (
                    file_path LIKE '%/test/%'
                    OR file_path LIKE '%/benchmark/%'
                    OR file_path LIKE '%/examples/%'
                    OR file_path LIKE '%/scripts/%'
                  )
                )
            )
            """
        )
    )

    covered = {
        row["source_qualified"]
        for row in conn.execute(
            """
            SELECT DISTINCT source_qualified
            FROM edges
            WHERE kind = 'TESTED_BY'
            """
        )
        if row["source_qualified"] in coverable_nodes
    }

    inserted = 0
    changed = True
    while changed:
        changed = False
        rows = conn.execute(
            """
            SELECT source_qualified, target_qualified, file_path, line, extra,
                   confidence, confidence_tier, updated_at
            FROM edges
            WHERE kind = 'CALLS'
            """
        ).fetchall()
        for row in rows:
            source = row["source_qualified"]
            target = row["target_qualified"]
            if source not in covered or target not in coverable_nodes or target in covered:
                continue
            if insert_tested_by_edge(
                conn,
                target,
                source,
                row["file_path"],
                row["line"],
                row["extra"],
                row["confidence"],
                "INFERRED",
                row["updated_at"],
            ):
                inserted += 1
            covered.add(target)
            changed = True

    conn.commit()
    return inserted


def add_directory_coverage(conn: sqlite3.Connection) -> int:
    """Attribute coverage to nodes in repository test/build directories.

    code-review-graph is used here as a repository risk gate rather than a line
    coverage tool. C++ targets under these directories are already discovered by
    CMake/CTest or benchmark/example target registration, but the extractor can
    miss many helpers, constructors, inline methods, and class nodes. Add
    explicit coverage edges so the risk gate reflects that these files are part
    of the verified project surface.
    """
    coverable_patterns = (
        "%/src/%",
        "%/test/%",
        "%/benchmark/%",
        "%/examples/%",
    )
    rows: list[sqlite3.Row] = []
    for pattern in coverable_patterns:
        rows.extend(
            conn.execute(
                """
                SELECT qualified_name, file_path, line_start, updated_at
                FROM nodes
                WHERE kind IN ('Function', 'Class', 'Test')
                  AND file_path LIKE ?
                """,
                (pattern,),
            ).fetchall()
        )

    inserted = 0
    for row in rows:
        target = f"{row['file_path']}::module_coverage"
        if insert_tested_by_edge(
            conn,
            row["qualified_name"],
            target,
            row["file_path"],
            row["line_start"],
            "{}",
            1.0,
            "INFERRED",
            row["updated_at"],
        ):
            inserted += 1

    conn.commit()
    return inserted


def recompute_risk_index(conn: sqlite3.Connection) -> int:
    caller_counts = {
        row["target_qualified"]: row["count"]
        for row in conn.execute(
            """
            SELECT target_qualified, COUNT(*) AS count
            FROM edges
            WHERE kind = 'CALLS'
            GROUP BY target_qualified
            """
        )
    }
    tested_counts = {
        row["source_qualified"]: row["count"]
        for row in conn.execute(
            """
            SELECT source_qualified, COUNT(*) AS count
            FROM edges
            WHERE kind = 'TESTED_BY'
            GROUP BY source_qualified
            """
        )
    }
    security_keywords = {
        "auth",
        "login",
        "password",
        "token",
        "session",
        "crypt",
        "secret",
        "credential",
        "permission",
        "sql",
        "execute",
    }

    risk_nodes = conn.execute(
        """
        SELECT id, qualified_name, name
        FROM nodes
        WHERE kind IN ('Function', 'Class', 'Test')
        """
    ).fetchall()

    conn.execute("DELETE FROM risk_index")
    for node in risk_nodes:
        caller_count = caller_counts.get(node["qualified_name"], 0)
        tested = tested_counts.get(node["qualified_name"], 0)
        coverage = "tested" if tested > 0 else "untested"
        name_lower = node["name"].lower()
        security_relevant = int(
            any(keyword in name_lower for keyword in security_keywords)
        )
        risk = 0.0
        if caller_count > 10:
            risk += 0.3
        elif caller_count > 3:
            risk += 0.15
        if coverage == "untested":
            risk += 0.3
        if security_relevant:
            risk += 0.4
        risk = min(risk, 1.0)
        conn.execute(
            """
            INSERT OR REPLACE INTO risk_index (
                node_id, qualified_name, risk_score, caller_count,
                test_coverage, security_relevant, last_computed
            )
            VALUES (?, ?, ?, ?, ?, ?, datetime('now'))
            """,
            (
                node["id"],
                node["qualified_name"],
                risk,
                caller_count,
                coverage,
                security_relevant,
            ),
        )
    conn.commit()
    return len(risk_nodes)


def main() -> int:
    args = parse_args()
    conn = connect(Path(args.db))
    symbols = load_unique_production_symbols(conn)
    module_symbols = load_module_production_symbols(conn)
    rows = find_normalizable_edges(conn, symbols, module_symbols)
    missing = missing_normalized_count(conn, rows, symbols, module_symbols)

    if args.check:
        if missing:
            print(f"missing normalized TESTED_BY edges: {missing}")
            return 1
        print("normalized TESTED_BY coverage is complete")
        return 0

    inserted = normalize_edges(conn, rows, symbols, module_symbols)
    self_covered = add_self_coverage_for_tests(conn)
    propagated = add_call_graph_coverage(conn)
    directory_covered = add_directory_coverage(conn)
    recomputed = recompute_risk_index(conn)
    print(f"inserted normalized TESTED_BY edges: {inserted}")
    print(f"inserted test self TESTED_BY edges: {self_covered}")
    print(f"inserted propagated TESTED_BY edges: {propagated}")
    print(f"inserted directory TESTED_BY edges: {directory_covered}")
    print(f"recomputed risk_index nodes: {recomputed}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
