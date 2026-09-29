#!/usr/bin/env python3
"""Keep the style audit strict while validating the actual cpp layout."""

import importlib.util
import os
from pathlib import Path
import subprocess
from tempfile import TemporaryDirectory
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location(
    "audit", ROOT / "scripts/common/100_audit_examples_tests_benchmarks_scripts_style.py")
audit = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(audit)


class StyleAuditTest(unittest.TestCase):
    def setUp(self):
        directory = TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        self.root = Path(directory.name)
        root = patch.object(audit, "ROOT", self.root)
        root.start()
        self.addCleanup(root.stop)
        audit.issues.clear()
        audit.advisories.clear()

    def create(self, name, content=""):
        path = self.root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(content)
        return path

    def test_exact_existing_test_exceptions_use_cpp_layout(self):
        for name in ("config/t_module_config_surface.cc", "utils/t_import_smoke.cc",
                     "config/t1_assertions_enabled.cc", "config/t_unapproved.cc"):
            self.create("test/cpp/" + name)
        audit.audit_tests()
        self.assertEqual(len(audit.issues), 1, audit.issues)
        self.assertIn("t_unapproved.cc", audit.issues[0])

    def test_scheduler_example_pair_is_required(self):
        self.create("examples/cpp/kernel/include/e13_scheduler_config.cc")
        audit.audit_examples()
        self.assertEqual(len(audit.issues), 1)
        self.create("examples/cpp/kernel/mcpp/e13_scheduler_config.cc")
        audit.issues.clear()
        audit.audit_examples()
        self.assertEqual(audit.issues, [])

    def test_script_header_remains_required(self):
        path = self.create("scripts/tencent/300_full_test.sh", "#!/bin/bash\n")
        path.chmod(0o755)
        audit.audit_scripts()
        self.assertEqual(len(audit.issues), 2)
        path.write_text("#!/usr/bin/env bash\nset -euo pipefail\n")
        audit.issues.clear()
        audit.audit_scripts()
        self.assertEqual(audit.issues, [])

    def test_repository_has_no_blocking_style_findings(self):
        with patch.object(audit, "ROOT", ROOT):
            audit.audit_examples()
            audit.audit_tests()
            audit.audit_benchmarks()
            audit.audit_scripts()
            audit.audit_text_patterns()
        self.assertEqual(audit.issues, [])

    def test_relocated_shell_scripts_resolve_the_repository_root(self):
        for name in ("300_full_test.sh", "301_numa_test.sh", "302_perf_analysis.sh"):
            source = (ROOT / "scripts/tencent" / name).read_text()
            # Run the actual setup and definitions from a mirrored category
            # layout, without invoking multi-minute benchmarks or perf.
            setup, separator, _ = source.partition("\nmain() {")
            self.assertTrue(separator)
            path = self.create("scripts/tencent/" + name, setup +
                               '\necho "ROOT=$PROJECT_ROOT"\n')
            result = subprocess.run(["bash", str(path)], cwd="/tmp",
                                    env={**os.environ, "RESULT_DIR": str(self.root / "results")},
                                    capture_output=True, text=True, check=True)
            self.assertEqual(result.stdout.strip(), f"ROOT={self.root}")


if __name__ == "__main__":
    unittest.main()
