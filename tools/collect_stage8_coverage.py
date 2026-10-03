#!/usr/bin/env python3
"""Collect GCC gcov JSON line coverage for SignalLab production modules."""

from __future__ import annotations

import argparse
import datetime as dt
import gzip
import json
import os
import shutil
import subprocess
import sys
import tempfile
from collections import defaultdict
from pathlib import Path
from typing import Any


def parse_arguments() -> argparse.Namespace:
    repository_root = Path(__file__).resolve().parent.parent
    build_directory = repository_root / "cmake-build-stage8-coverage-mingw64"
    parser = argparse.ArgumentParser(
        description=(
            "Read gcov JSON data, summarize executable-line coverage for "
            "non-GUI production sources, and enforce the Stage 8 threshold."
        )
    )
    parser.add_argument(
        "--build-dir",
        type=Path,
        default=build_directory,
        help="instrumented CMake build directory",
    )
    parser.add_argument(
        "--source-root",
        type=Path,
        default=repository_root / "src",
        help="production source root (src/gui is always excluded)",
    )
    parser.add_argument(
        "--gcov",
        type=Path,
        default=Path("E:/MSYS2/mingw64/bin/gcov.exe"),
        help="GCC gcov executable",
    )
    parser.add_argument(
        "--output-json",
        type=Path,
        default=None,
        help=(
            "machine-readable report path (default: "
            "BUILD_DIR/stage8-coverage/coverage.json)"
        ),
    )
    parser.add_argument(
        "--output-markdown",
        type=Path,
        default=None,
        help=(
            "human-readable report path (default: "
            "BUILD_DIR/stage8-coverage/coverage.md)"
        ),
    )
    parser.add_argument(
        "--threshold",
        type=float,
        default=70.0,
        help="minimum executable-line percentage required for every module",
    )
    arguments = parser.parse_args()
    if not 0.0 <= arguments.threshold <= 100.0:
        parser.error("--threshold must be between 0 and 100")
    return arguments


def canonical(path: Path) -> str:
    return os.path.normcase(str(path.resolve(strict=False)))


def is_relative_to(path: Path, parent: Path) -> bool:
    try:
        path.relative_to(parent)
        return True
    except ValueError:
        return False


def resolve_reported_source(
    reported_path: str,
    current_working_directory: str,
    source_root: Path,
    build_directory: Path,
) -> Path:
    path = Path(reported_path)
    if path.is_absolute():
        return path.resolve(strict=False)

    candidates: list[Path] = []
    if current_working_directory:
        candidates.append(Path(current_working_directory) / path)
    candidates.extend(
        (
            source_root.parent / path,
            build_directory / path,
        )
    )
    for candidate in candidates:
        if candidate.exists():
            return candidate.resolve(strict=False)
    return candidates[0].resolve(strict=False)


def load_gcov_json(path: Path) -> dict[str, Any]:
    with gzip.open(path, mode="rt", encoding="utf-8") as stream:
        payload = json.load(stream)
    if not isinstance(payload, dict):
        raise ValueError(f"gcov JSON root is not an object: {path}")
    return payload


def expected_production_sources(source_root: Path) -> dict[str, Path]:
    sources: dict[str, Path] = {}
    for source in source_root.rglob("*.cpp"):
        relative = source.relative_to(source_root)
        if relative.parts and relative.parts[0].lower() == "gui":
            continue
        sources[canonical(source)] = source.resolve()
    return sources


def collect_coverage(
    gcov: Path,
    build_directory: Path,
    source_root: Path,
    note_files: list[Path],
) -> tuple[dict[str, dict[int, int]], list[str]]:
    line_counts: dict[str, dict[int, int]] = defaultdict(dict)
    diagnostics: list[str] = []

    for note_file in note_files:
        with tempfile.TemporaryDirectory(prefix="signallab-gcov-") as temporary:
            completed = subprocess.run(
                [str(gcov), "--json-format", str(note_file)],
                cwd=temporary,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
                encoding="utf-8",
                errors="replace",
                check=False,
            )
            json_files = list(Path(temporary).glob("*.gcov.json.gz"))
            if not json_files:
                diagnostics.append(
                    f"{note_file}: gcov exit {completed.returncode}; "
                    f"{completed.stdout.strip()}"
                )
                continue

            for json_file in json_files:
                payload = load_gcov_json(json_file)
                working_directory = str(
                    payload.get("current_working_directory", "")
                )
                for file_entry in payload.get("files", []):
                    if not isinstance(file_entry, dict) or "file" not in file_entry:
                        continue
                    source = resolve_reported_source(
                        str(file_entry["file"]),
                        working_directory,
                        source_root,
                        build_directory,
                    )
                    if not is_relative_to(source, source_root):
                        continue
                    relative = source.relative_to(source_root)
                    if relative.parts and relative.parts[0].lower() == "gui":
                        continue
                    if source.suffix.lower() != ".cpp":
                        continue

                    source_key = canonical(source)
                    for line_entry in file_entry.get("lines", []):
                        if not isinstance(line_entry, dict):
                            continue
                        line_number = line_entry.get("line_number")
                        execution_count = line_entry.get("count")
                        if not isinstance(line_number, int):
                            continue
                        if not isinstance(execution_count, int):
                            continue
                        previous = line_counts[source_key].get(line_number, 0)
                        line_counts[source_key][line_number] = max(
                            previous, execution_count
                        )

    return line_counts, diagnostics


def coverage_row(name: str, line_counts: dict[int, int]) -> dict[str, Any]:
    executable_lines = len(line_counts)
    covered_lines = sum(count > 0 for count in line_counts.values())
    percentage = (
        covered_lines * 100.0 / executable_lines if executable_lines else 0.0
    )
    return {
        "name": name,
        "covered_lines": covered_lines,
        "executable_lines": executable_lines,
        "line_coverage_percent": round(percentage, 2),
    }


def build_report(
    source_root: Path,
    build_directory: Path,
    gcov: Path,
    gcov_version: str,
    threshold: float,
    note_file_count: int,
    data_file_count: int,
    expected_sources: dict[str, Path],
    line_counts: dict[str, dict[int, int]],
    diagnostics: list[str],
) -> dict[str, Any]:
    file_rows: list[dict[str, Any]] = []
    module_counts: dict[str, dict[int, int]] = defaultdict(dict)
    module_next_line: dict[str, int] = defaultdict(int)

    for source_key, source in sorted(
        expected_sources.items(), key=lambda item: str(item[1]).lower()
    ):
        counts = line_counts.get(source_key, {})
        relative = source.relative_to(source_root)
        module = relative.parts[0]
        row = coverage_row(relative.as_posix(), counts)
        row["module"] = module
        file_rows.append(row)

        # Module totals are sums across files. Synthetic sequential keys avoid
        # collisions between identical line numbers in different files.
        for count in counts.values():
            synthetic_line = module_next_line[module]
            module_counts[module][synthetic_line] = count
            module_next_line[module] += 1

    module_rows: list[dict[str, Any]] = []
    for module in sorted(module_counts):
        row = coverage_row(module, module_counts[module])
        row["meets_threshold"] = row["line_coverage_percent"] >= threshold
        module_rows.append(row)

    all_counts: dict[int, int] = {}
    next_line = 0
    for counts in module_counts.values():
        for count in counts.values():
            all_counts[next_line] = count
            next_line += 1
    overall = coverage_row("all non-GUI production modules", all_counts)
    overall["meets_threshold"] = overall["line_coverage_percent"] >= threshold

    missing_sources = [
        source.relative_to(source_root).as_posix()
        for key, source in expected_sources.items()
        if key not in line_counts or not line_counts[key]
    ]
    low_modules = [
        row["name"] for row in module_rows if not row["meets_threshold"]
    ]
    complete = not missing_sources and bool(module_rows)
    passed = complete and overall["meets_threshold"] and not low_modules

    return {
        "schema_version": 1,
        "generated_utc": dt.datetime.now(dt.timezone.utc).isoformat(),
        "result": "PASS" if passed else "FAIL",
        "threshold_percent": threshold,
        "source_root": str(source_root),
        "build_directory": str(build_directory),
        "gcov": str(gcov),
        "gcov_version": gcov_version,
        "gcno_files": note_file_count,
        "gcda_files": data_file_count,
        "overall": overall,
        "modules": module_rows,
        "files": file_rows,
        "missing_sources": sorted(missing_sources),
        "low_modules": low_modules,
        "gcov_diagnostics": diagnostics,
        "report_complete": complete,
    }


def write_reports(
    report: dict[str, Any],
    json_path: Path,
    markdown_path: Path,
) -> None:
    json_path.parent.mkdir(parents=True, exist_ok=True)
    markdown_path.parent.mkdir(parents=True, exist_ok=True)
    json_path.write_text(
        json.dumps(report, ensure_ascii=False, indent=2) + "\n",
        encoding="utf-8",
    )

    lines = [
        "# SignalLab Stage 8 coverage",
        "",
        f"- Result: **{report['result']}**",
        f"- Threshold: {report['threshold_percent']:.2f}% per module",
        f"- Build: `{report['build_directory']}`",
        f"- Generated (UTC): {report['generated_utc']}",
        "",
        "| Module | Covered lines | Executable lines | Coverage | Gate |",
        "|---|---:|---:|---:|---|",
    ]
    for module in report["modules"]:
        gate = "PASS" if module["meets_threshold"] else "FAIL"
        lines.append(
            f"| {module['name']} | {module['covered_lines']} | "
            f"{module['executable_lines']} | "
            f"{module['line_coverage_percent']:.2f}% | {gate} |"
        )

    overall = report["overall"]
    overall_gate = "PASS" if overall["meets_threshold"] else "FAIL"
    lines.extend(
        (
            f"| **Overall** | **{overall['covered_lines']}** | "
            f"**{overall['executable_lines']}** | "
            f"**{overall['line_coverage_percent']:.2f}%** | "
            f"**{overall_gate}** |",
            "",
        )
    )

    if report["missing_sources"]:
        lines.extend(("## Missing production sources", ""))
        lines.extend(f"- `{source}`" for source in report["missing_sources"])
        lines.append("")
    if report["gcov_diagnostics"]:
        lines.extend(
            (
                "## gcov diagnostics",
                "",
                f"{len(report['gcov_diagnostics'])} object file(s) did not "
                "produce JSON. See the machine-readable report for details.",
                "",
            )
        )

    markdown_path.write_text("\n".join(lines), encoding="utf-8")


def main() -> int:
    arguments = parse_arguments()
    build_directory = arguments.build_dir.resolve()
    source_root = arguments.source_root.resolve()
    gcov = arguments.gcov.resolve()
    report_directory = build_directory / "stage8-coverage"
    output_json = (
        arguments.output_json.resolve()
        if arguments.output_json is not None
        else report_directory / "coverage.json"
    )
    output_markdown = (
        arguments.output_markdown.resolve()
        if arguments.output_markdown is not None
        else report_directory / "coverage.md"
    )

    if not build_directory.is_dir():
        print(f"error: build directory not found: {build_directory}", file=sys.stderr)
        return 1
    if not source_root.is_dir():
        print(f"error: source root not found: {source_root}", file=sys.stderr)
        return 1
    if not gcov.is_file():
        discovered_gcov = shutil.which(str(arguments.gcov))
        if not discovered_gcov:
            print(f"error: gcov not found: {gcov}", file=sys.stderr)
            return 1
        gcov = Path(discovered_gcov).resolve()

    note_files = sorted(build_directory.rglob("*.gcno"))
    data_files = sorted(build_directory.rglob("*.gcda"))
    if not note_files:
        print(
            "error: no .gcno files found; configure and build the coverage preset first",
            file=sys.stderr,
        )
        return 1
    if not data_files:
        print(
            "error: no .gcda files found; run the coverage CTest preset first",
            file=sys.stderr,
        )
        return 1

    version_result = subprocess.run(
        [str(gcov), "--version"],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        encoding="utf-8",
        errors="replace",
        check=False,
    )
    gcov_version = version_result.stdout.splitlines()[0].strip()

    expected_sources = expected_production_sources(source_root)
    line_counts, diagnostics = collect_coverage(
        gcov,
        build_directory,
        source_root,
        note_files,
    )
    report = build_report(
        source_root,
        build_directory,
        gcov,
        gcov_version,
        arguments.threshold,
        len(note_files),
        len(data_files),
        expected_sources,
        line_counts,
        diagnostics,
    )
    write_reports(report, output_json, output_markdown)

    overall = report["overall"]
    print(
        "Stage 8 coverage: "
        f"{overall['covered_lines']}/{overall['executable_lines']} lines "
        f"({overall['line_coverage_percent']:.2f}%), result {report['result']}"
    )
    for module in report["modules"]:
        print(
            f"  {module['name']}: {module['covered_lines']}/"
            f"{module['executable_lines']} "
            f"({module['line_coverage_percent']:.2f}%)"
        )
    print(f"JSON: {output_json}")
    print(f"Markdown: {output_markdown}")

    if not report["report_complete"]:
        print(
            "error: coverage report is incomplete; inspect missing_sources",
            file=sys.stderr,
        )
        return 1
    return 0 if report["result"] == "PASS" else 2


if __name__ == "__main__":
    raise SystemExit(main())
