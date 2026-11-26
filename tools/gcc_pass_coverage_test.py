"""Unit tests for `tools/gcc_pass_coverage.py` gcov JSON parsing & diffing."""

import json
import unittest

from tools import gcc_pass_coverage as cov


def _make_gcov_json(
    file_path: str,
    functions: list[dict],
    lines: list[dict],
    extra_files: list[dict] | None = None,
) -> str:
  files = [{"file": file_path, "functions": functions, "lines": lines}]
  if extra_files:
    files.extend(extra_files)
  return json.dumps({
      "format_version": "1",
      "gcc_version": "13.3.0",
      "current_working_directory": "/tmp",
      "data_file": "final.gcda",
      "files": files,
  })


class GccPassCoverageTest(unittest.TestCase):

  def test_parse_and_merge_across_shards_and_ignore_header_and_throw_branches(self):
    shard0_json = _make_gcov_json(
        file_path="/repo/toolchain/gcc-2.7.2-src/final.c",
        functions=[
            {"name": "final_scan_insn", "start_line": 100, "end_line": 120, "execution_count": 3},
        ],
        lines=[
            {"line_number": 102, "function_name": "final_scan_insn", "count": 5, "branches": []},
            {
                "line_number": 105,
                "function_name": "final_scan_insn",
                "count": 3,
                "branches": [
                    {"count": 3, "throw": False, "fallthrough": True},
                    {"count": 0, "throw": False, "fallthrough": False},
                    {"count": 99, "throw": True, "fallthrough": False},
                ],
            },
            {"line_number": 110, "function_name": "final_scan_insn", "count": 0, "branches": []},
        ],
        extra_files=[
            {
                "file": "/repo/toolchain/gcc-2.7.2-src/rtl.h",
                "functions": [{"name": "hdr_fn", "start_line": 1, "end_line": 5, "execution_count": 10}],
                "lines": [{"line_number": 2, "function_name": "hdr_fn", "count": 10, "branches": []}],
            }
        ],
    )
    shard1_json = _make_gcov_json(
        file_path="/repo/toolchain/gcc-2.7.2-src/final.c",
        functions=[
            {"name": "final_scan_insn", "start_line": 100, "end_line": 120, "execution_count": 7},
        ],
        lines=[
            {"line_number": 102, "function_name": "final_scan_insn", "count": 4, "branches": []},
            {
                "line_number": 105,
                "function_name": "final_scan_insn",
                "count": 2,
                "branches": [
                    {"count": 0, "throw": False, "fallthrough": True},
                    {"count": 2, "throw": False, "fallthrough": False},
                    {"count": 0, "throw": True, "fallthrough": False},
                ],
            },
            {"line_number": 110, "function_name": "final_scan_insn", "count": 0, "branches": []},
        ],
    )

    agg = cov.FileCoverage(src_rel="final.c")
    cov.merge_file_coverage(agg, cov.parse_gcov_json_text(shard0_json, "final.c"))
    cov.merge_file_coverage(agg, cov.parse_gcov_json_text(shard1_json, "final.c"))

    # Header `rtl.h` ignored; counts summed across shards; throw branch ignored.
    self.assertEqual(agg.line_counts, {102: 9, 105: 5, 110: 0})
    self.assertEqual(agg.total_lines(), 3)
    self.assertEqual(agg.executed_lines(), 2)
    self.assertEqual(agg.branch_counts, {(105, 0): 3, (105, 1): 2})
    self.assertEqual(agg.total_branches(), 2)
    self.assertEqual(agg.executed_branches(), 2)
    self.assertEqual(agg.functions["final_scan_insn"].execution_count, 10)

  def test_forward_vs_replay_diff_and_markdown_rendering(self):
    spec = cov.PassSpec("Stage 0 (MIPS Backend)", "config/mips/mips.c", "mips", True)
    fwd = cov.FileCoverage(
        src_rel="config/mips/mips.c",
        line_counts={10: 4, 11: 2, 12: 0, 20: 0, 21: 0},
        line_functions={10: "fn_a", 11: "fn_a", 12: "fn_a", 20: "fn_b", 21: "fn_b"},
        branch_counts={(10, 0): 4, (10, 1): 0, (11, 0): 2, (11, 1): 0},
        functions={
            "fn_a": cov.FunctionRecord("fn_a", 10, 15, 4),
            "fn_b": cov.FunctionRecord("fn_b", 20, 25, 0),
        },
    )
    rep = cov.FileCoverage(
        src_rel="config/mips/mips.c",
        line_counts={10: 3, 11: 0, 12: 1, 20: 0, 21: 0},
        line_functions={10: "fn_a", 11: "fn_a", 12: "fn_a", 20: "fn_b", 21: "fn_b"},
        branch_counts={(10, 0): 2, (10, 1): 1, (11, 0): 0, (11, 1): 0},
        functions={
            "fn_a": cov.FunctionRecord("fn_a", 10, 15, 3),
            "fn_b": cov.FunctionRecord("fn_b", 20, 25, 0),
        },
    )

    diff = cov.compute_forward_vs_replay_diff(spec, fwd, rep)
    self.assertEqual(diff.total_lines, 5)
    self.assertEqual(diff.forward_lines, 2)
    self.assertEqual(diff.replay_lines, 2)
    self.assertEqual(diff.both_lines, 1)
    self.assertEqual(diff.forward_only_lines, [11])
    self.assertEqual(diff.replay_only_lines, [12])
    self.assertEqual(diff.total_branches, 4)
    self.assertEqual(diff.forward_branches, 2)
    self.assertEqual(diff.replay_branches, 2)
    self.assertEqual(diff.both_branches, 1)
    self.assertEqual(diff.forward_only_branches, 1)
    self.assertEqual(diff.replay_only_branches, 1)

    md = cov.render_markdown_report(
        [diff], "python3 tools/gcc_pass_coverage.py", corpus_case_count=4382, num_workers=2
    )
    self.assertIn("`config/mips/mips.c`", md)
    self.assertIn("`fn_a` (`config/mips/mips.c:10-15`)", md)
    self.assertIn("`fn_b` (`config/mips/mips.c:20-25`)", md)
    self.assertIn("Top Forward-Only Functions", md)
    self.assertIn("Top Replay-Only Functions", md)

  def test_format_line_ranges_edge_cases(self):
    self.assertEqual(cov.format_line_ranges([]), "—")
    self.assertEqual(cov.format_line_ranges([5]), "5")
    self.assertEqual(cov.format_line_ranges([1, 2, 3, 7, 9, 10]), "1-3, 7, 9-10")
    self.assertEqual(
        cov.format_line_ranges([1, 3, 5, 7, 9], max_ranges=3),
        "1, 3, 5, ... (+2 more)",
    )

  def test_empty_coverage_has_zero_div_safety(self):
    spec = cov.PassSpec("Stage 0", "final.c", "final", True)
    empty = cov.FileCoverage(src_rel="final.c")
    diff = cov.compute_forward_vs_replay_diff(spec, empty, empty)
    self.assertEqual(diff.total_lines, 0)
    self.assertEqual(diff.total_branches, 0)
    md = cov.render_markdown_report([diff], "cmd", corpus_case_count=0, num_workers=1)
    self.assertIn("0.0%", md)

  def test_parse_shard_indices(self):
    self.assertEqual(cov.parse_shard_indices(None, 4), [0, 1, 2, 3])
    self.assertEqual(cov.parse_shard_indices("0,2", 4), [0, 2])
    self.assertEqual(cov.parse_shard_indices("1-3,1", 4), [1, 2, 3])
    with self.assertRaises(ValueError):
      cov.parse_shard_indices("4", 4)
    with self.assertRaises(ValueError):
      cov.parse_shard_indices("3-1", 4)
    with self.assertRaises(ValueError):
      cov.parse_shard_indices(", ,", 4)


if __name__ == "__main__":
  unittest.main()
