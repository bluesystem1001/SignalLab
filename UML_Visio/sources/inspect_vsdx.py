"""Inspect editable VSDX content using only Python's standard library.

This does not launch Visio or change the diagram/models.  It checks the OPC
package, each page's native shapes/text/glue, and selected code/model facts.
Run from any directory: python inspect_vsdx.py
"""

from __future__ import annotations

import argparse
import hashlib
import json
import posixpath
import re
import sys
import zipfile
from datetime import datetime, timezone
from pathlib import Path
import xml.etree.ElementTree as ET


VISIO = "http://schemas.microsoft.com/office/visio/2012/main"
REL = "http://schemas.openxmlformats.org/package/2006/relationships"
DOC_REL = "http://schemas.openxmlformats.org/officeDocument/2006/relationships"
NS = {"v": VISIO, "r": REL}
MODEL_FILES = (
    "guide_pages.json", "structure_pages.json", "class_pages.json",
    "behavior_pages.json", "service_pages.json", "extension_pages.json",
)


def read_json(path: Path):
    return json.loads(path.read_text(encoding="utf-8-sig"))


def normal(text: str) -> str:
    return " ".join(text.split())


def cells(shape):
    return {cell.get("N"): cell for cell in shape.findall("v:Cell", NS)}


def text_of(shape):
    return "\n".join("".join(t.itertext()) for t in shape.findall(".//v:Text", NS))


def code_audit(project: Path, models: dict) -> dict:
    """Recheck concrete claims; this supplements a human source review."""
    facts, warnings, errors = [], [], []

    def fact(name, paths, predicate):
        source = "\n".join((project / p).read_text(encoding="utf-8") for p in paths)
        ok = bool(predicate(source))
        facts.append({"fact": name, "passed": ok, "sources": paths})
        if not ok:
            errors.append(name)

    fact("Signal metadata and imported entries are value members",
         ["include/signallab/core/signal.hpp", "include/signallab/app/import_service.hpp"],
         lambda s: all(v in s for v in ["SignalMetadata metadata_", "core::Signal signal", "std::vector<SignalEntry> entries_"]))
    fact("SQLite foreign keys and unique source relationship match page 06",
         ["migrations/001_init.sql"],
         lambda s: all(v in s for v in [
             "source_id INTEGER NOT NULL UNIQUE", "file_version_id INTEGER,",
             "REFERENCES file_version(id) ON DELETE SET NULL",
             "REFERENCES signal_source(id) ON DELETE CASCADE",
             "REFERENCES signal_record(id) ON DELETE SET NULL",
             "REFERENCES analysis_task(id) ON DELETE CASCADE",
             "signal_unit TEXT", "signal_source_label TEXT"]))
    fact("Persisted analysis includes CANCELLED; script lifecycle has source identity",
         ["migrations/002_task_scheduler.sql", "migrations/003_signal_script.sql"],
         lambda s: "'CANCELLED'" in s and "ADD COLUMN source_hash" in s and "ADD COLUMN error_column" in s)
    fact("Scheduler owns two value pools and a map of shared task records",
         ["include/signallab/task/scheduler.hpp"],
         lambda s: all(v in s for v in ["ThreadPool computePool_", "ThreadPool ioPool_", "std::shared_ptr<TaskRecord>> tasks_"]))
    fact("TaskStatus is the five-state in-memory lifecycle",
         ["include/signallab/task/task.hpp"],
         lambda s: bool(re.search(r"enum class TaskStatus\s*\{\s*Pending,\s*Running,\s*Succeeded,\s*Failed,\s*Cancelled\s*\}", s)))
    fact("Audio input is unique ownership; FIR is optional value state",
         ["include/signallab/app/audio_capture_service.hpp"],
         lambda s: "std::unique_ptr<repository::AudioInput> input_" in s and "std::optional<algorithm::StreamingFirFilter> filter_" in s)
    fact("Backup collaborators are shared pointers, not exclusive composition",
         ["include/signallab/app/backup_service.hpp"],
         lambda s: "std::shared_ptr<repository::BackupSource> source_" in s and "std::shared_ptr<repository::BackupRepository> repository_" in s)
    fact("Audio and backup implementations realize their repository interfaces",
         ["include/signallab/io/windows_audio_input.hpp", "include/signallab/database/project_backup_source.hpp", "include/signallab/database/mariadb_backup_repository.hpp"],
         lambda s: all(v in s for v in ["public repository::AudioInput", "public repository::BackupSource", "public repository::BackupRepository"]))
    fact("Backup GUI launches an independent std::async worker",
         ["src/gui/database_backup_widget.cpp"],
         lambda s: "std::async(std::launch::async" in s)
    fact("Current source requires MariaDB client; backup snapshot uses SQLite VACUUM INTO",
         ["cmake/Dependencies.cmake", "src/database/project_backup_source.cpp"],
         lambda s: "MariaDB_LIBRARY NAMES mariadb libmariadb REQUIRED" in s and "VACUUM main INTO ?" in s)

    persistence = models.get("06 持久化类图", {})
    relations = {(e.get("from"), e.get("to")): e for e in persistence.get("edges", [])}
    expected = {
        ("tbl_file", "tbl_source"): ("0..1", "0..*"),
        ("tbl_source", "tbl_signal"): ("1", "0..1"),
        ("tbl_signal", "tbl_task"): ("0..1", "0..*"),
        ("tbl_task", "tbl_feature"): ("1", "0..*"),
    }
    for pair, multiplicities in expected.items():
        edge = relations.get(pair, {})
        if (edge.get("sourceMultiplicity"), edge.get("targetMultiplicity")) != multiplicities:
            errors.append("Page 06 multiplicity mismatch: " + repr(pair))
    if any("tbl_audit" in (e.get("from"), e.get("to")) for e in persistence.get("edges", [])):
        errors.append("operation_log must not be represented as a foreign-key association")

    extension = models.get("14 音频与备份类", {})
    for edge in extension.get("edges", []):
        pair = (edge.get("from"), edge.get("to"))
        if pair == ("ext_audio_service", "ext_audio_input"):
            if edge.get("targetMultiplicity") != "0..1":
                warnings.append({
                    "page": "14 音频与备份类",
                    "issue": "AudioInput is marked 1, although AudioCaptureService accepts a null unique_ptr; the GUI injects one concrete input.",
                    "sources": ["src/app/audio_capture_service.cpp:12–16,25–29,43–45"],
                })
        if pair[0] == "ext_backup_service" and pair[1] in {"ext_backup_source", "ext_backup_repository"}:
            if edge.get("style") != "association" or edge.get("targetMultiplicity") != "0..1":
                errors.append("Backup shared-pointer relationships must be nullable ordinary associations")
        if pair[0] in {"ext_windows_audio", "ext_project_backup", "ext_mariadb_backup"}:
            if edge.get("style") != "realization":
                errors.append("Extension implementation relation must point toward its interface")
    return {"facts": facts, "errors": errors, "warnings": warnings,
            "passed": not errors, "scope": "Selected structural facts checked against source; not an application regression test."}


def inspect(vsdx: Path, expected_file: Path, sources: Path) -> dict:
    expected = read_json(expected_file)
    model_list = [p for name in MODEL_FILES for p in read_json(sources / name)]
    models = {p["name"]: p for p in model_list}
    expected_by_name = {p["page"]: p for p in expected}
    errors, warnings, pages = [], [], []
    data = vsdx.read_bytes()
    with zipfile.ZipFile(vsdx) as package:
        names = package.namelist()
        corrupt = package.testzip()
        if corrupt:
            errors.append("ZIP CRC failure: " + corrupt)
        xml = {}
        for name in names:
            if name.endswith((".xml", ".rels")):
                try:
                    xml[name] = ET.fromstring(package.read(name))
                except ET.ParseError as exc:
                    errors.append(f"Malformed XML {name}: {exc}")
        media = [n for n in names if n.lower().startswith("visio/media/")]
        embeddings = [n for n in names if "/embeddings/" in n.lower()]
        image_relationships = []
        for name, root in xml.items():
            if not name.endswith(".rels"):
                continue
            for relation in root:
                if relation.get("Type", "").lower().endswith("/image"):
                    image_relationships.append({"part": name, **relation.attrib})
        if media or embeddings or image_relationships:
            errors.append("Page content contains media, embeddings, or image relationships")
        root = xml["visio/pages/pages.xml"]
        rels = {r.get("Id"): r.get("Target") for r in xml["visio/pages/_rels/pages.xml.rels"]}
        actual_names = [p.get("Name") for p in root.findall("v:Page", NS)]
        if actual_names != [p["page"] for p in expected]:
            errors.append("Page order/names do not match verification.json")
        if len(actual_names) != 15:
            errors.append(f"Expected 15 pages, found {len(actual_names)}")
        for page in root.findall("v:Page", NS):
            name = page.get("Name")
            rel_id = page.find("v:Rel", NS).get("{" + DOC_REL + "}id")
            part = posixpath.normpath(posixpath.join("visio/pages", rels[rel_id]))
            content = xml[part]
            shapes = content.findall(".//v:Shape", NS)
            top = content.findall("v:Shapes/v:Shape", NS)
            shape_map = {s.get("ID"): s for s in shapes}
            shape_names = {s.get("NameU"): s for s in shapes}
            page_errors = []
            foreign = content.findall(".//v:ForeignData", NS)
            if foreign or any(s.get("Type") == "Foreign" for s in shapes):
                page_errors.append("Foreign/image shape found")
            refs = []
            for shape in shapes:
                for cell in shape.findall(".//v:Cell", NS):
                    formula = cell.get("F", "")
                    if "#REF!" in formula or "#NAME?" in formula:
                        page_errors.append("Broken formula: " + formula)
                    for ref in re.findall(r"Sheet\.(\d+)!", formula):
                        refs.append(ref)
                        if ref not in shape_map:
                            page_errors.append("Nonlocal or missing Sheet reference: " + ref)
            connections = content.findall("v:Connects/v:Connect", NS)
            for connection in connections:
                if connection.get("FromSheet") not in shape_map or connection.get("ToSheet") not in shape_map:
                    page_errors.append("Glue references missing local shape")
            connector_shapes = [s for s in shapes if s.get("NameU", "").startswith("UML_Connector_")]
            glued_count = 0
            for connector in connector_shapes:
                c = cells(connector)
                if all("Sheet." in c.get(key, {}).get("F", "") for key in ("BeginX", "EndX")):
                    glued_count += 1
            spec = models[name]
            desired_glue = sum(bool(e.get("from") and e.get("to")) for e in spec["edges"])
            if glued_count != desired_glue or len(connections) != 2 * desired_glue:
                page_errors.append(f"Endpoint glue mismatch: {glued_count}/{desired_glue}; records={len(connections)}")
            if len(connector_shapes) != len(spec["edges"]):
                page_errors.append("Native connector count does not match model")
            if len(top) != expected_by_name[name]["topLevelShapes"]:
                page_errors.append("Top-level native shape count differs from verification.json")
            all_text = normal(text_of(content))
            missing_text = []
            for node in spec["nodes"]:
                if "UML_" + node["id"] not in shape_names:
                    page_errors.append("Missing native model node: " + node["id"])
                for value in [node.get("text", ""), *node.get("sections", [])]:
                    if value.strip() and normal(value) not in all_text:
                        missing_text.append(value)
            for edge_index, edge in enumerate(spec["edges"]):
                connector = shape_names.get("UML_Connector_" + str(edge_index))
                if connector is None:
                    continue
                c = cells(connector)
                style = edge.get("style")
                if style == "realization" and (c.get("LinePattern", {}).get("V") != "2" or c.get("EndArrow", {}).get("V") != "14"):
                    page_errors.append(f"Interface realization {edge_index} lacks dashed triangle toward interface")
                if edge.get("label") and normal(edge["label"]) not in all_text:
                    missing_text.append(edge["label"])
            if missing_text:
                page_errors.append("Model text not preserved in native XML")
            errors.extend(f"{name}: {e}" for e in page_errors)
            pages.append({
                "page": name, "part": part, "top_level_shape_count": len(top),
                "all_shape_count_including_group_children": len(shapes),
                "native_group_count": sum(s.get("Type") == "Group" for s in shapes),
                "text_shape_count": len(content.findall(".//v:Text", NS)),
                "native_connector_count": len(connector_shapes),
                "glued_connector_count": glued_count, "local_glue_records": len(connections),
                "local_sheet_formula_reference_count": len(refs),
                "foreign_data_count": len(foreign),
                "representative_text": [n.get("text", "") for n in spec["nodes"] if n.get("text")][:4],
                "missing_model_text": missing_text, "errors": page_errors,
                "passed": not page_errors,
                "note": "Sequence-message geometry is intentionally native but not glued to lifeline header groups." if desired_glue < len(spec["edges"]) else "All model endpoint associations use local native connection formulas.",
            })
        thumbnail = [n for n in names if n.lower().startswith("docprops/thumbnail")]
    audit = code_audit(sources.parent.parent, models)
    errors.extend(audit["errors"])
    warnings.extend(audit["warnings"])
    return {
        "file": str(vsdx.resolve()), "sha256": hashlib.sha256(data).hexdigest(),
        "checked_at_utc": datetime.now(timezone.utc).isoformat(),
        "validator": "Python standard-library ZIP/XML inspection; no Visio automation",
        "page_count": len(pages), "all_page_names_match_manifest": actual_names == [p["page"] for p in expected],
        "native_editable_content": not media and not embeddings and not image_relationships and all(p["foreign_data_count"] == 0 for p in pages),
        "visio_media_parts": media, "embedded_object_parts": embeddings,
        "image_relationships": image_relationships,
        "package_thumbnail_parts": thumbnail,
        "thumbnail_note": "The standard docProps thumbnail is a package preview, not a diagram image or page shape.",
        "pages": pages, "source_model_review": audit,
        "errors": errors, "warnings": warnings, "passed": not errors,
        "limits": "Checks native structure, text preservation, formula locality and selected source facts. Layout/visual QA and interaction are separate checks.",
    }


def main():
    folder = Path(__file__).resolve().parent.parent
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--vsdx", type=Path, default=folder / "SignalLab_UML.vsdx")
    parser.add_argument("--expected", type=Path, default=folder / "verification.json")
    parser.add_argument("--output", type=Path, default=folder / "package-verification.json")
    args = parser.parse_args()
    report = inspect(args.vsdx, args.expected, folder / "sources")
    args.output.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"passed": report["passed"], "pages": report["page_count"],
                      "native_editable": report["native_editable_content"],
                      "errors": len(report["errors"]), "warnings": len(report["warnings"])}))
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    sys.exit(main())
