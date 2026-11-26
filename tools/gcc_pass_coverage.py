#!/usr/bin/env python3
"""On-demand GCC 2.7.2 per-pass line & branch coverage tool (forward vs. replay).

Orchestrates:
  1. Building the isolated gcov-instrumented archive
     `toolchain/libgcc272_harness_cov.a` (leaving `toolchain/gcc-2.7.2-src/`
     read-only and `toolchain/libgcc272_harness.a` untouched).
  2. Building uninstrumented `//gcc_harness:shared_corpus_traces` and
     instrumented `--//gcc_harness:coverage=true //lifter:gcc_pass_coverage_runner`.
  3. Running parallel sharded `--mode=forward` and `--mode=replay` workers with
     isolated `GCOV_PREFIX` directories (asserting exit code 0 on every worker).
  4. Pairing `.gcno` symlinks with each shard's `.gcda` files, running
     `gcov -b -c -j -t`, summing line/branch/function counts across shards, and
     emitting a Markdown coverage & forward-vs-replay diff report (plus optional
     JSON output).
"""

from __future__ import annotations

import argparse
from concurrent.futures import ThreadPoolExecutor
from dataclasses import dataclass, field
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
from typing import Any


@dataclass(frozen=True)
class PassSpec:
  stage_label: str
  src_rel: str
  obj_stem: str
  is_stage_0_6: bool


PASS_SPECS: tuple[PassSpec, ...] = (
    PassSpec("Stage 0 (Final)", "final.c", "final", True),
    PassSpec("Stage 0 (MIPS Backend)", "config/mips/mips.c", "mips", True),
    PassSpec("Stage 1 (DBR Schedule)", "reorg.c", "reorg", True),
    PassSpec("Stage 2 (Jump2)", "jump.c", "jump", True),
    PassSpec("Stages 3 & 6 (Sched)", "sched.c", "sched", True),
    PassSpec("Stage 4 (Global Alloc)", "global.c", "global", True),
    PassSpec("Stage 4 (Reload)", "reload.c", "reload", True),
    PassSpec("Stage 4 (Reload1)", "reload1.c", "reload1", True),
    PassSpec("Stage 4 (Caller-Save)", "caller-save.c", "caller-save", True),
    PassSpec("Stage 5 (Local Alloc)", "local-alloc.c", "local-alloc", True),
    PassSpec("Stage 7 (Combine — baseline)", "combine.c", "combine", False),
    PassSpec("Stage 8 (Flow — baseline)", "flow.c", "flow", False),
    PassSpec("Stages 9 & 11 (CSE — baseline)", "cse.c", "cse", False),
    PassSpec("Stage 10 (Loop — baseline)", "loop.c", "loop", False),
)


@dataclass
class FunctionRecord:
  name: str
  start_line: int
  end_line: int
  execution_count: int = 0


@dataclass
class FileCoverage:
  src_rel: str
  # line_number -> total execution count across shards
  line_counts: dict[int, int] = field(default_factory=dict)
  # line_number -> function_name (from gcov JSON)
  line_functions: dict[int, str] = field(default_factory=dict)
  # (line_number, branch_idx) -> total branch edge count across shards
  branch_counts: dict[tuple[int, int], int] = field(default_factory=dict)
  # function_name -> FunctionRecord
  functions: dict[str, FunctionRecord] = field(default_factory=dict)

  def total_lines(self) -> int:
    return len(self.line_counts)

  def executed_lines(self) -> int:
    return sum(1 for c in self.line_counts.values() if c > 0)

  def total_branches(self) -> int:
    return len(self.branch_counts)

  def executed_branches(self) -> int:
    return sum(1 for c in self.branch_counts.values() if c > 0)


@dataclass
class FunctionDiffSummary:
  name: str
  start_line: int
  end_line: int
  total_lines: int
  forward_covered: int
  replay_covered: int
  uncovered_lines: list[int]
  forward_only_lines: list[int]
  replay_only_lines: list[int]


@dataclass
class FileDiffSummary:
  spec: PassSpec
  total_lines: int
  forward_lines: int
  replay_lines: int
  both_lines: int
  forward_only_lines: list[int]
  replay_only_lines: list[int]
  total_branches: int
  forward_branches: int
  replay_branches: int
  both_branches: int
  forward_only_branches: int
  replay_only_branches: int
  functions: list[FunctionDiffSummary]


def parse_gcov_json_text(json_text: str, target_src_rel: str) -> FileCoverage:
  """Parses GCC `gcov -b -c -j -t` JSON stdout for `target_src_rel`."""
  data = json.loads(json_text)
  result = FileCoverage(src_rel=target_src_rel)
  norm_target = target_src_rel.replace("\\", "/")

  for file_obj in data.get("files", []):
    file_path = str(file_obj.get("file", "")).replace("\\", "/")
    if not (file_path == norm_target or file_path.endswith("/" + norm_target)):
      continue

    for fn in file_obj.get("functions", []):
      name = str(fn.get("name", ""))
      start_line = int(fn.get("start_line", 0))
      end_line = int(fn.get("end_line", start_line))
      exec_count = int(fn.get("execution_count", 0))
      if name in result.functions:
        result.functions[name].execution_count += exec_count
      else:
        result.functions[name] = FunctionRecord(
            name=name,
            start_line=start_line,
            end_line=end_line,
            execution_count=exec_count,
        )

    for ln in file_obj.get("lines", []):
      line_no = int(ln.get("line_number", 0))
      count = int(ln.get("count", 0))
      fn_name = str(ln.get("function_name", ""))
      result.line_counts[line_no] = result.line_counts.get(line_no, 0) + count
      if fn_name and line_no not in result.line_functions:
        result.line_functions[line_no] = fn_name

      branches = ln.get("branches", [])
      non_throw_idx = 0
      for br in branches:
        if br.get("throw", False):
          continue
        key = (line_no, non_throw_idx)
        result.branch_counts[key] = (
            result.branch_counts.get(key, 0) + int(br.get("count", 0))
        )
        non_throw_idx += 1

  return result


def merge_file_coverage(dst: FileCoverage, src: FileCoverage) -> None:
  """Accumulates `src` shard coverage counts into `dst` in-place."""
  for line_no, count in src.line_counts.items():
    dst.line_counts[line_no] = dst.line_counts.get(line_no, 0) + count
  for line_no, fn_name in src.line_functions.items():
    dst.line_functions.setdefault(line_no, fn_name)
  for key, count in src.branch_counts.items():
    dst.branch_counts[key] = dst.branch_counts.get(key, 0) + count
  for name, fn in src.functions.items():
    if name in dst.functions:
      dst.functions[name].execution_count += fn.execution_count
    else:
      dst.functions[name] = FunctionRecord(
          name=fn.name,
          start_line=fn.start_line,
          end_line=fn.end_line,
          execution_count=fn.execution_count,
      )


def compute_forward_vs_replay_diff(
    spec: PassSpec, forward: FileCoverage, replay: FileCoverage
) -> FileDiffSummary:
  """Computes exact line, branch, and per-function forward-vs-replay diff."""
  all_lines = sorted(set(forward.line_counts) | set(replay.line_counts))
  fwd_lines = 0
  rep_lines = 0
  both_lines = 0
  fwd_only_lines: list[int] = []
  rep_only_lines: list[int] = []

  for ln in all_lines:
    fc = forward.line_counts.get(ln, 0)
    rc = replay.line_counts.get(ln, 0)
    if fc > 0:
      fwd_lines += 1
    if rc > 0:
      rep_lines += 1
    if fc > 0 and rc > 0:
      both_lines += 1
    elif fc > 0 and rc == 0:
      fwd_only_lines.append(ln)
    elif fc == 0 and rc > 0:
      rep_only_lines.append(ln)

  all_branches = set(forward.branch_counts) | set(replay.branch_counts)
  fwd_br = 0
  rep_br = 0
  both_br = 0
  fwd_only_br = 0
  rep_only_br = 0
  for key in all_branches:
    fc = forward.branch_counts.get(key, 0)
    rc = replay.branch_counts.get(key, 0)
    if fc > 0:
      fwd_br += 1
    if rc > 0:
      rep_br += 1
    if fc > 0 and rc > 0:
      both_br += 1
    elif fc > 0 and rc == 0:
      fwd_only_br += 1
    elif fc == 0 and rc > 0:
      rep_only_br += 1

  # Group lines by function name (falling back to start..end bounds if missing).
  all_fns: dict[str, FunctionRecord] = {}
  for name, fn in forward.functions.items():
    all_fns[name] = FunctionRecord(
        fn.name, fn.start_line, fn.end_line, fn.execution_count
    )
  for name, fn in replay.functions.items():
    if name not in all_fns:
      all_fns[name] = FunctionRecord(fn.name, fn.start_line, fn.end_line, 0)

  fn_lines: dict[str, list[int]] = {name: [] for name in all_fns}
  for ln in all_lines:
    owner = (
        forward.line_functions.get(ln)
        or replay.line_functions.get(ln)
        or ""
    )
    if not owner:
      for name, fn in all_fns.items():
        if fn.start_line <= ln <= fn.end_line:
          owner = name
          break
    if owner in fn_lines:
      fn_lines[owner].append(ln)

  fn_summaries: list[FunctionDiffSummary] = []
  for name, fn in sorted(all_fns.items(), key=lambda kv: (kv[1].start_line, kv[0])):
    lines_in_fn = sorted(fn_lines.get(name, []))
    f_cov = [ln for ln in lines_in_fn if forward.line_counts.get(ln, 0) > 0]
    r_cov = [ln for ln in lines_in_fn if replay.line_counts.get(ln, 0) > 0]
    uncov = [ln for ln in lines_in_fn if forward.line_counts.get(ln, 0) == 0]
    f_only = [
        ln
        for ln in lines_in_fn
        if forward.line_counts.get(ln, 0) > 0 and replay.line_counts.get(ln, 0) == 0
    ]
    r_only = [
        ln
        for ln in lines_in_fn
        if forward.line_counts.get(ln, 0) == 0 and replay.line_counts.get(ln, 0) > 0
    ]
    fn_summaries.append(
        FunctionDiffSummary(
            name=name,
            start_line=fn.start_line,
            end_line=fn.end_line,
            total_lines=len(lines_in_fn),
            forward_covered=len(f_cov),
            replay_covered=len(r_cov),
            uncovered_lines=uncov,
            forward_only_lines=f_only,
            replay_only_lines=r_only,
        )
    )

  return FileDiffSummary(
      spec=spec,
      total_lines=len(all_lines),
      forward_lines=fwd_lines,
      replay_lines=rep_lines,
      both_lines=both_lines,
      forward_only_lines=fwd_only_lines,
      replay_only_lines=rep_only_lines,
      total_branches=len(all_branches),
      forward_branches=fwd_br,
      replay_branches=rep_br,
      both_branches=both_br,
      forward_only_branches=fwd_only_br,
      replay_only_branches=rep_only_br,
      functions=fn_summaries,
  )


def format_line_ranges(lines: list[int], max_ranges: int = 8) -> str:
  """Condenses sorted line numbers into compact ranges like `12-15, 20`."""
  if not lines:
    return "—"
  ranges: list[str] = []
  start = prev = lines[0]
  for ln in lines[1:]:
    if ln == prev + 1:
      prev = ln
    else:
      ranges.append(f"{start}" if start == prev else f"{start}-{prev}")
      start = prev = ln
  ranges.append(f"{start}" if start == prev else f"{start}-{prev}")
  if len(ranges) > max_ranges:
    return ", ".join(ranges[:max_ranges]) + f", ... (+{len(ranges) - max_ranges} more)"
  return ", ".join(ranges)


def _pct(num: int, denom: int) -> str:
  if denom <= 0:
    return "0.0%"
  return f"{(100.0 * num) / denom:.1f}%"


def render_markdown_report(
    diffs: list[FileDiffSummary],
    repro_cmd: str,
    corpus_case_count: int,
    num_workers: int,
) -> str:
  """Renders the complete human-readable Markdown coverage & diff report."""
  out: list[str] = []
  out.append("# GCC 2.7.2 Per-Pass Line & Branch Coverage Baseline (Stages 0–6)")
  out.append("")
  out.append(
      f"- **Shared Corpus Size**: {corpus_case_count:,} cases (626 base C functions × 7 compiler/assembler flag profiles)"
  )
  out.append(f"- **Parallel Shards / Workers**: {num_workers}")
  out.append(f"- **Reproduction Command**: `{repro_cmd}`")
  out.append(
      "- **Methodology**: Isolated `gcov --coverage -fprofile-abs-path` archive "
      "(`toolchain/libgcc272_harness_cov.a`, 14 MIPS-preprocessed GCC 2.7.2 pass files). "
      "`forward` executes only live `TraceForwardCompilation(tc)` across all corpus cases; "
      "`replay` loads cached traces and executes only live Stages 0–6 right-inverse replay "
      "(with fail-fast exact match assertions on every single check)."
  )
  out.append("")

  # 1. Per-file corpus & replay coverage table
  out.append("## 1. Per-File MIPS-Reachable Line & Branch Coverage")
  out.append("")
  out.append(
      "| Stage | GCC 2.7.2 File | Total Lines | Forward Lines | Replay Lines | "
      "Total Branches | Forward Branches | Replay Branches |"
  )
  out.append("|---|---|---:|---:|---:|---:|---:|---:|")

  s06_tot_l = s06_fwd_l = s06_rep_l = 0
  s06_tot_b = s06_fwd_b = s06_rep_b = 0
  all_tot_l = all_fwd_l = all_rep_l = 0
  all_tot_b = all_fwd_b = all_rep_b = 0

  for d in diffs:
    all_tot_l += d.total_lines
    all_fwd_l += d.forward_lines
    all_rep_l += d.replay_lines
    all_tot_b += d.total_branches
    all_fwd_b += d.forward_branches
    all_rep_b += d.replay_branches
    if d.spec.is_stage_0_6:
      s06_tot_l += d.total_lines
      s06_fwd_l += d.forward_lines
      s06_rep_l += d.replay_lines
      s06_tot_b += d.total_branches
      s06_fwd_b += d.forward_branches
      s06_rep_b += d.replay_branches

    out.append(
        f"| {d.spec.stage_label} | `{d.spec.src_rel}` | {d.total_lines:,} | "
        f"{d.forward_lines:,} ({_pct(d.forward_lines, d.total_lines)}) | "
        f"{d.replay_lines:,} ({_pct(d.replay_lines, d.total_lines)}) | "
        f"{d.total_branches:,} | "
        f"{d.forward_branches:,} ({_pct(d.forward_branches, d.total_branches)}) | "
        f"{d.replay_branches:,} ({_pct(d.replay_branches, d.total_branches)}) |"
    )

  out.append(
      f"| **Stages 0–6 Subtotal (10 files)** | — | **{s06_tot_l:,}** | "
      f"**{s06_fwd_l:,} ({_pct(s06_fwd_l, s06_tot_l)})** | "
      f"**{s06_rep_l:,} ({_pct(s06_rep_l, s06_tot_l)})** | "
      f"**{s06_tot_b:,}** | "
      f"**{s06_fwd_b:,} ({_pct(s06_fwd_b, s06_tot_b)})** | "
      f"**{s06_rep_b:,} ({_pct(s06_rep_b, s06_tot_b)})** |"
  )
  out.append(
      f"| **All 14 Files Total** | — | **{all_tot_l:,}** | "
      f"**{all_fwd_l:,} ({_pct(all_fwd_l, all_tot_l)})** | "
      f"**{all_rep_l:,} ({_pct(all_rep_l, all_tot_l)})** | "
      f"**{all_tot_b:,}** | "
      f"**{all_fwd_b:,} ({_pct(all_fwd_b, all_tot_b)})** | "
      f"**{all_rep_b:,} ({_pct(all_rep_b, all_tot_b)})** |"
  )
  out.append("")

  # 2. Forward vs. Replay Diff Table
  out.append("## 2. Forward ($F_s$) vs. Right-Inverse Replay ($F_s(G_s(x))$) Diff")
  out.append("")
  out.append(
      "| Stage | GCC 2.7.2 File | Both Lines | Forward-Only Lines | Replay-Only Lines | "
      "Replay / Forward Overlap | Forward-Only Branches | Replay-Only Branches |"
  )
  out.append("|---|---|---:|---:|---:|---:|---:|---:|")
  for d in diffs:
    out.append(
        f"| {d.spec.stage_label} | `{d.spec.src_rel}` | {d.both_lines:,} | "
        f"{len(d.forward_only_lines):,} | {len(d.replay_only_lines):,} | "
        f"{_pct(d.both_lines, d.forward_lines)} | "
        f"{d.forward_only_branches:,} | {d.replay_only_branches:,} |"
    )
  out.append("")

  # 3. Per-file detailed gaps for Stages 0-6
  out.append("## 3. Stages 0–6 Per-Function Gap Analysis (`file:line` Citations)")
  out.append("")
  for d in diffs:
    if not d.spec.is_stage_0_6:
      continue
    out.append(f"### `{d.spec.src_rel}` — {d.spec.stage_label}")
    out.append(
        f"- Lines: **{d.forward_lines:,}/{d.total_lines:,} ({_pct(d.forward_lines, d.total_lines)})** forward, "
        f"**{d.replay_lines:,}/{d.total_lines:,} ({_pct(d.replay_lines, d.total_lines)})** replay "
        f"(overlap **{_pct(d.both_lines, d.forward_lines)}**, "
        f"**{len(d.forward_only_lines):,}** forward-only, **{len(d.replay_only_lines):,}** replay-only)."
    )
    out.append(
        f"- Branches: **{d.forward_branches:,}/{d.total_branches:,} ({_pct(d.forward_branches, d.total_branches)})** forward, "
        f"**{d.replay_branches:,}/{d.total_branches:,} ({_pct(d.replay_branches, d.total_branches)})** replay "
        f"(**{d.forward_only_branches:,}** forward-only, **{d.replay_only_branches:,}** replay-only)."
    )
    out.append("")

    top_fwd_only = sorted(
        (fn for fn in d.functions if fn.forward_only_lines),
        key=lambda fn: (-len(fn.forward_only_lines), fn.start_line),
    )[:5]
    if top_fwd_only:
      out.append("**Top Forward-Only Functions** (`forward > 0 && replay == 0`):")
      for fn in top_fwd_only:
        out.append(
            f"- `{fn.name}` (`{d.spec.src_rel}:{fn.start_line}-{fn.end_line}`): "
            f"{len(fn.forward_only_lines)} lines ({format_line_ranges(fn.forward_only_lines)})"
        )
      out.append("")

    top_rep_only = sorted(
        (fn for fn in d.functions if fn.replay_only_lines),
        key=lambda fn: (-len(fn.replay_only_lines), fn.start_line),
    )[:5]
    if top_rep_only:
      out.append("**Top Replay-Only Functions** (`forward == 0 && replay > 0`):")
      for fn in top_rep_only:
        out.append(
            f"- `{fn.name}` (`{d.spec.src_rel}:{fn.start_line}-{fn.end_line}`): "
            f"{len(fn.replay_only_lines)} lines ({format_line_ranges(fn.replay_only_lines)})"
        )
      out.append("")

    top_uncov = sorted(
        (fn for fn in d.functions if fn.uncovered_lines),
        key=lambda fn: (-len(fn.uncovered_lines), fn.start_line),
    )[:5]
    if top_uncov:
      out.append("**Top Corpus-Uncovered Functions** (`forward == 0`):")
      for fn in top_uncov:
        out.append(
            f"- `{fn.name}` (`{d.spec.src_rel}:{fn.start_line}-{fn.end_line}`): "
            f"{len(fn.uncovered_lines)}/{fn.total_lines} lines uncovered "
            f"(e.g. {format_line_ranges(fn.uncovered_lines, max_ranges=5)})"
        )
      out.append("")

  return "\n".join(out).rstrip() + "\n"


def _run_cmd(cmd: list[str], cwd: Path, env: dict[str, str] | None = None) -> None:
  proc = subprocess.run(cmd, cwd=str(cwd), env=env, check=False)
  if proc.returncode != 0:
    raise RuntimeError(f"Command failed (exit {proc.returncode}): {' '.join(cmd)}")


def parse_shard_indices(spec: str | None, num_shards: int) -> list[int]:
  """Parses an optional comma-separated shard index list (e.g. '0,1' or '2-3')."""
  if not spec:
    return list(range(num_shards))
  indices: list[int] = []
  seen: set[int] = set()
  for token in spec.split(","):
    token = token.strip()
    if not token:
      continue
    if "-" in token:
      parts = token.split("-", 1)
      lo, hi = int(parts[0]), int(parts[1])
      if lo < 0 or hi >= num_shards or lo > hi:
        raise ValueError(
            f"Shard range '{token}' out of bounds for num_shards={num_shards}"
        )
      for idx in range(lo, hi + 1):
        if idx not in seen:
          seen.add(idx)
          indices.append(idx)
    else:
      idx = int(token)
      if idx < 0 or idx >= num_shards:
        raise ValueError(
            f"Shard index {idx} out of bounds for num_shards={num_shards}"
        )
      if idx not in seen:
        seen.add(idx)
        indices.append(idx)
  if not indices:
    raise ValueError("Empty --shard-indices specification")
  return indices


def _run_mode_workers(
    repo_root: Path,
    runner_bin: Path,
    mode: str,
    num_shards: int,
    shard_indices: list[int],
    max_workers: int,
    work_root: Path,
) -> list[Path]:
  runfiles_dir = Path(str(runner_bin) + ".runfiles")
  shard_roots: list[Path] = []

  def run_one(shard_idx: int) -> Path:
    prefix = work_root / mode / f"shard_{shard_idx:02d}"
    if prefix.exists():
      shutil.rmtree(prefix)
    prefix.mkdir(parents=True, exist_ok=True)
    env = os.environ.copy()
    env["GCOV_PREFIX"] = str(prefix)
    env["GCOV_PREFIX_STRIP"] = "0"
    if runfiles_dir.is_dir():
      env["RUNFILES_DIR"] = str(runfiles_dir)
      env["TEST_WORKSPACE"] = "_main"
    cmd = [
        str(runner_bin),
        f"--mode={mode}",
        f"--shard_index={shard_idx}",
        f"--num_shards={num_shards}",
    ]
    proc = subprocess.run(cmd, cwd=str(repo_root), env=env, check=False)
    if proc.returncode != 0:
      raise RuntimeError(
          f"Coverage worker failed: mode={mode} shard={shard_idx}/{num_shards} "
          f"(exit {proc.returncode})"
      )
    print(
        f"[gcc_pass_coverage] Finished '{mode}' shard {shard_idx}/{num_shards}",
        flush=True,
    )
    return prefix

  print(
      f"[gcc_pass_coverage] Running '{mode}' shards {shard_indices} of {num_shards} "
      f"(max_workers={max_workers})...",
      flush=True,
  )
  with ThreadPoolExecutor(max_workers=max(1, min(max_workers, len(shard_indices)))) as pool:
    futures = [pool.submit(run_one, i) for i in shard_indices]
    for fut in futures:
      shard_roots.append(fut.result())
  return shard_roots


def _collect_mode_coverage(
    cov_build_dir: Path, shard_roots: list[Path]
) -> dict[str, FileCoverage]:
  agg: dict[str, FileCoverage] = {
      spec.src_rel: FileCoverage(src_rel=spec.src_rel) for spec in PASS_SPECS
  }
  abs_cov_rel = str(cov_build_dir.resolve()).lstrip("/")

  for shard_root in shard_roots:
    mirrored_dir = shard_root / abs_cov_rel
    if not mirrored_dir.is_dir():
      raise RuntimeError(
          f"Missing mirrored gcov shard directory: {mirrored_dir}"
      )

    # Explicitly symlink every `.gcno` alongside its shard `.gcda` in `mirrored_dir`.
    for spec in PASS_SPECS:
      gcno_src = cov_build_dir / f"{spec.obj_stem}.gcno"
      gcno_dst = mirrored_dir / f"{spec.obj_stem}.gcno"
      if gcno_dst.exists() or gcno_dst.is_symlink():
        gcno_dst.unlink()
      gcno_dst.symlink_to(gcno_src)

    for spec in PASS_SPECS:
      gcno_dst = mirrored_dir / f"{spec.obj_stem}.gcno"
      proc = subprocess.run(
          ["gcov", "-b", "-c", "-j", "-t", str(gcno_dst)],
          cwd=str(mirrored_dir),
          stdout=subprocess.PIPE,
          stderr=subprocess.PIPE,
          text=True,
          check=False,
      )
      if proc.returncode != 0:
        raise RuntimeError(
            f"gcov failed for {spec.obj_stem} in {mirrored_dir}: {proc.stderr}"
        )
      parsed = parse_gcov_json_text(proc.stdout, spec.src_rel)
      merge_file_coverage(agg[spec.src_rel], parsed)

  return agg


def _serialize_summary_json(diffs: list[FileDiffSummary]) -> dict[str, Any]:
  files_out = []
  for d in diffs:
    files_out.append({
        "stage_label": d.spec.stage_label,
        "src_rel": d.spec.src_rel,
        "is_stage_0_6": d.spec.is_stage_0_6,
        "total_lines": d.total_lines,
        "forward_lines": d.forward_lines,
        "replay_lines": d.replay_lines,
        "both_lines": d.both_lines,
        "forward_only_line_count": len(d.forward_only_lines),
        "replay_only_line_count": len(d.replay_only_lines),
        "forward_only_lines": d.forward_only_lines,
        "replay_only_lines": d.replay_only_lines,
        "total_branches": d.total_branches,
        "forward_branches": d.forward_branches,
        "replay_branches": d.replay_branches,
        "both_branches": d.both_branches,
        "forward_only_branches": d.forward_only_branches,
        "replay_only_branches": d.replay_only_branches,
    })
  return {"files": files_out}


def main(argv: list[str] | None = None) -> int:
  parser = argparse.ArgumentParser(
      description="Collect GCC 2.7.2 per-pass line & branch coverage (forward vs. replay)."
  )
  parser.add_argument(
      "--markdown-out",
      type=Path,
      default=Path("docs/gcc_pass_coverage_stages_0_6.md"),
      help="Destination Markdown report path.",
  )
  parser.add_argument(
      "--json-out",
      type=Path,
      default=None,
      help="Optional machine-readable JSON summary output path.",
  )
  parser.add_argument(
      "--skip-build-archive",
      action="store_true",
      help="Skip running tools/build_gcc272_coverage_harness.sh.",
  )
  parser.add_argument(
      "--workers",
      type=int,
      default=min(os.cpu_count() or 2, 16),
      help="Maximum number of concurrent worker processes per mode.",
  )
  parser.add_argument(
      "--num-shards",
      type=int,
      default=None,
      help="Total shard count across the corpus (defaults to --workers).",
  )
  parser.add_argument(
      "--shard-indices",
      type=str,
      default=None,
      help="Optional subset of shards to execute (e.g. '0,1' or '0-3').",
  )
  parser.add_argument(
      "--gcda-dir",
      type=Path,
      default=None,
      help="Optional persistent directory for shard .gcda data (enables multi-phase runs).",
  )
  parser.add_argument(
      "--phase",
      choices=("all", "build", "forward", "replay", "report"),
      default="all",
      help="Execution phase (default: all).",
  )
  args = parser.parse_args(argv)

  repo_root = Path(__file__).resolve().parent.parent
  cov_build_dir = repo_root / "toolchain" / "gcc-2.7.2-cov-build"
  num_workers = max(1, int(args.workers))
  num_shards = max(1, int(args.num_shards)) if args.num_shards is not None else num_workers
  shard_indices = parse_shard_indices(args.shard_indices, num_shards)

  if args.phase in ("all", "build"):
    if not args.skip_build_archive:
      _run_cmd([str(repo_root / "tools" / "build_gcc272_coverage_harness.sh")], repo_root)

    # Ensure uninstrumented cached shards exist and build instrumented runner.
    print(
        "[gcc_pass_coverage] Building cached trace shards and instrumented coverage runner...",
        flush=True,
    )
    _run_cmd(["bazel", "build", "-c", "opt", "//gcc_harness:shared_corpus_traces"], repo_root)
    _run_cmd(
        [
            "bazel",
            "build",
            "-c",
            "opt",
            "--//gcc_harness:coverage=true",
            "//lifter:gcc_pass_coverage_runner",
        ],
        repo_root,
    )
    # Clean any stray `.gcda` files under `toolchain/gcc-2.7.2-cov-build/`.
    for stray in cov_build_dir.glob("*.gcda"):
      stray.unlink()
    if args.phase == "build":
      return 0

  runner_bin = repo_root / "bazel-bin" / "lifter" / "gcc_pass_coverage_runner"
  cleanup_tmp = args.gcda_dir is None
  if args.gcda_dir is not None:
    work_root = args.gcda_dir
    work_root.mkdir(parents=True, exist_ok=True)
  else:
    if args.phase != "all":
      raise RuntimeError(f"--phase={args.phase} requires --gcda-dir")
    work_root = Path(tempfile.mkdtemp(prefix="gcc_pass_cov_"))

  try:
    if args.phase in ("all", "forward"):
      _run_mode_workers(
          repo_root, runner_bin, "forward", num_shards, shard_indices, num_workers, work_root
      )
      if args.phase == "forward":
        return 0

    if args.phase in ("all", "replay"):
      _run_mode_workers(
          repo_root, runner_bin, "replay", num_shards, shard_indices, num_workers, work_root
      )
      if args.phase == "replay":
        return 0

    fwd_shards = [work_root / "forward" / f"shard_{i:02d}" for i in range(num_shards)]
    rep_shards = [work_root / "replay" / f"shard_{i:02d}" for i in range(num_shards)]
    print(
        f"[gcc_pass_coverage] Aggregating gcov JSON across {num_shards} shards per mode...",
        flush=True,
    )
    fwd_cov = _collect_mode_coverage(cov_build_dir, fwd_shards)
    rep_cov = _collect_mode_coverage(cov_build_dir, rep_shards)
  finally:
    if cleanup_tmp:
      shutil.rmtree(work_root, ignore_errors=True)

  diffs = [
      compute_forward_vs_replay_diff(spec, fwd_cov[spec.src_rel], rep_cov[spec.src_rel])
      for spec in PASS_SPECS
  ]

  md_path = args.markdown_out if args.markdown_out.is_absolute() else (repo_root / args.markdown_out)
  md_path.parent.mkdir(parents=True, exist_ok=True)
  repro_cmd = f"python3 tools/gcc_pass_coverage.py --markdown-out={args.markdown_out}"
  report_text = render_markdown_report(
      diffs=diffs,
      repro_cmd=repro_cmd,
      corpus_case_count=4382,
      num_workers=num_shards,
  )
  md_path.write_text(report_text, encoding="utf-8")
  print(f"[gcc_pass_coverage] Wrote Markdown report to {md_path}", flush=True)

  if args.json_out is not None:
    json_path = args.json_out if args.json_out.is_absolute() else (repo_root / args.json_out)
    json_path.parent.mkdir(parents=True, exist_ok=True)
    json_path.write_text(
        json.dumps(_serialize_summary_json(diffs), indent=2) + "\n", encoding="utf-8"
    )
    print(f"[gcc_pass_coverage] Wrote JSON summary to {json_path}", flush=True)

  return 0


if __name__ == "__main__":
  sys.exit(main())
