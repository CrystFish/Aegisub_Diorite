#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Import Codex CLI conversations for a specific project into DeepSeek Harness (dsh).

This script is intentionally conservative:
  1. It locates Codex session JSONL files.
  2. It filters sessions that mention the target project path.
  3. It parses the Codex JSONL into a simple normalized transcript.
  4. It tries to find dsh's data directory or CLI.
  5. It writes the normalized transcripts as JSONL files for dsh, or exports them
     under the project if no dsh directory/CLI is found.

Usage examples:
  python tools/import_codex_to_dsh.py --project "D:\\Code\\Custom\\Aegisub_Toshi-ban"
  python tools/import_codex_to_dsh.py --dry-run
  python tools/import_codex_to_dsh.py --dsh-dir "%USERPROFILE%\\.dsh"
"""

from __future__ import annotations

import argparse
import json
import os
import shutil
import subprocess
import sys
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Iterable


def utc_now_iso() -> str:
    return datetime.now(timezone.utc).isoformat()


def find_codex_session_dirs() -> list[Path]:
    """Return candidate directories where Codex CLI stores session JSONL files."""
    candidates: list[Path] = []
    home = Path.home()

    # Common Codex CLI locations.
    candidates.append(home / ".codex" / "sessions")
    candidates.append(home / ".codex" / "session")
    candidates.append(home / ".config" / "codex" / "sessions")
    candidates.append(home / "AppData" / "Local" / "codex" / "sessions")
    candidates.append(home / "AppData" / "Roaming" / "codex" / "sessions")

    # Also allow the project-local .codex folder if someone keeps it there.
    # The caller's --project is added later by the caller.
    return [p for p in candidates if p.is_dir()]


def find_codex_session_files(project: Path, extra_dirs: Iterable[Path] = ()) -> list[Path]:
    """Find Codex session JSONL files whose content mentions the project path."""
    project_str = str(project).lower().replace("/", "\\")
    dirs = list(find_codex_session_dirs()) + [p for p in extra_dirs if p.is_dir()]
    files: list[Path] = []

    seen: set[Path] = set()
    for d in dirs:
        if d in seen:
            continue
        seen.add(d)
        if not d.exists():
            continue
        for p in sorted(d.glob("*.jsonl")):
            try:
                # Fast filter: only keep sessions that mention this project.
                text = p.read_text(encoding="utf-8", errors="ignore").lower()
                if project_str in text:
                    files.append(p)
            except OSError:
                continue

    # De-duplicate by resolved path.
    unique: dict[str, Path] = {}
    for p in files:
        try:
            key = str(p.resolve()).lower()
        except OSError:
            key = str(p).lower()
        unique.setdefault(key, p)

    return sorted(unique.values(), key=lambda p: p.stat().st_mtime)


def extract_text_from_content(content: Any) -> str:
    """Flatten Codex content (string or list of content parts) to readable text."""
    if content is None:
        return ""
    if isinstance(content, str):
        return content
    if isinstance(content, list):
        parts: list[str] = []
        for item in content:
            if isinstance(item, str):
                parts.append(item)
            elif isinstance(item, dict):
                if item.get("type") == "text":
                    parts.append(str(item.get("text", "")))
                elif item.get("type") == "input_text":
                    parts.append(str(item.get("text", "")))
                elif item.get("type") == "output_text":
                    parts.append(str(item.get("text", "")))
                elif item.get("type") == "tool_use":
                    name = item.get("name", "tool")
                    inp = item.get("input", {})
                    parts.append(f"[tool_use: {name}] {json.dumps(inp, ensure_ascii=False)}")
                elif item.get("type") == "tool_result":
                    parts.append(f"[tool_result] {extract_text_from_content(item.get('content', ''))}")
                elif "text" in item:
                    parts.append(str(item["text"]))
                elif "content" in item:
                    parts.append(extract_text_from_content(item["content"]))
            else:
                parts.append(str(item))
        return "\n".join(part for part in parts if part)
    if isinstance(content, dict):
        if "text" in content:
            return str(content["text"])
        if "content" in content:
            return extract_text_from_content(content["content"])
        return json.dumps(content, ensure_ascii=False)
    return str(content)


def parse_codex_session(path: Path, project: Path) -> dict[str, Any]:
    """Parse one Codex session JSONL into a normalized dsh-ish transcript."""
    session: dict[str, Any] = {
        "source": "codex",
        "source_file": str(path),
        "project": str(project),
        "imported_at": utc_now_iso(),
        "session_id": path.stem,
        "title": path.stem,
        "cwd": None,
        "messages": [],
    }

    lines: list[dict[str, Any]] = []
    with path.open("r", encoding="utf-8", errors="replace") as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            try:
                obj = json.loads(line)
            except json.JSONDecodeError:
                continue
            lines.append(obj)

    # Collect metadata from any line that carries cwd/title.
    for obj in lines:
        if isinstance(obj, dict):
            cwd = obj.get("cwd") or (obj.get("payload") or {}).get("cwd")
            if cwd and session["cwd"] is None:
                session["cwd"] = str(cwd)
            title = obj.get("title") or (obj.get("payload") or {}).get("title")
            if title and session["title"] == session["session_id"]:
                session["title"] = str(title)

    for obj in lines:
        if not isinstance(obj, dict):
            continue

        timestamp = obj.get("timestamp") or obj.get("ts") or obj.get("time")
        payload = obj.get("payload") if isinstance(obj.get("payload"), dict) else {}
        role = payload.get("role") or obj.get("role")
        content = payload.get("content") if "content" in payload else obj.get("content")
        msg_type = payload.get("type") or obj.get("type")

        # Skip non-message bookkeeping if no role/content.
        if not role and not content and not msg_type:
            continue

        text = extract_text_from_content(content)
        if not text and msg_type:
            # Keep a lightweight marker for events we do not understand.
            text = f"[{msg_type}]"

        message: dict[str, Any] = {
            "role": role or "unknown",
            "content": text,
        }
        if timestamp:
            message["timestamp"] = str(timestamp)
        if msg_type:
            message["type"] = str(msg_type)
        session["messages"].append(message)

    if not session["title"] or session["title"] == session["session_id"]:
        # Use first user text as a basic title.
        for m in session["messages"]:
            if m.get("role") in ("user", "human") and m["content"].strip():
                title = m["content"].strip().replace("\n", " ")[:80]
                if title:
                    session["title"] = title
                    break

    return session


def find_dsh_dir() -> Path | None:
    """Try to locate dsh data directory from environment/common locations."""
    env = os.environ.get("DSH_HOME") or os.environ.get("DEEPSEEK_HARNESS_HOME")
    if env:
        p = Path(env)
        if p.is_dir():
            return p

    home = Path.home()
    candidates = [
        home / ".dsh",
        home / ".deepseek-harness",
        home / ".config" / "dsh",
        home / "AppData" / "Roaming" / "dsh",
        home / "AppData" / "Local" / "dsh",
        home / ".local" / "share" / "dsh",
    ]
    for p in candidates:
        if p.is_dir():
            return p
    return None


def find_dsh_cli() -> str | None:
    """Return the dsh executable name if it can be found on PATH."""
    exe = shutil.which("dsh") or shutil.which("dsh.exe")
    return exe


def write_dsh_jsonl(session: dict[str, Any], dsh_dir: Path) -> Path:
    """Write one normalized session into a dsh JSONL sessions directory."""
    sessions_dir = dsh_dir / "sessions"
    sessions_dir.mkdir(parents=True, exist_ok=True)

    safe_id = "".join(ch for ch in session["session_id"] if ch.isalnum() or ch in "-_")[:80] or "codex"
    out = sessions_dir / f"codex-{safe_id}.jsonl"
    with out.open("w", encoding="utf-8", newline="\n") as f:
        f.write(json.dumps(session, ensure_ascii=False, indent=2) + "\n")
    return out


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--project",
        default=r"D:\Code\Custom\Aegisub_Toshi-ban",
        help="Project path used to filter Codex sessions.",
    )
    parser.add_argument(
        "--extra-codex-dir",
        action="append",
        default=[],
        help="Additional directory containing Codex session JSONL files.",
    )
    parser.add_argument(
        "--dsh-dir",
        default=None,
        help="Explicit dsh data directory. Auto-detected if omitted.",
    )
    parser.add_argument(
        "--dry-run",
        action="store_true",
        help="Only parse and print what would be imported.",
    )
    parser.add_argument(
        "--export-dir",
        default=None,
        help="Write normalized transcripts here if dsh is not found.",
    )
    args = parser.parse_args()

    project = Path(args.project).resolve()
    if not project.exists():
        print(f"Project path does not exist: {project}", file=sys.stderr)
        return 2

    extra_dirs = [Path(p) for p in args.extra_codex_dir]
    session_files = find_codex_session_files(project, extra_dirs)
    if not session_files:
        print("No Codex session files found for project.")
        print(f"Looked in: {[str(p) for p in find_codex_session_dirs()]}")
        return 1

    print(f"Found {len(session_files)} Codex session file(s) for {project}")

    sessions = []
    for sf in session_files:
        s = parse_codex_session(sf, project)
        sessions.append(s)
        print(f"  - {sf.name}: {len(s['messages'])} messages, title={s['title']!r}")

    if args.dry_run:
        return 0

    dsh_dir = Path(args.dsh_dir) if args.dsh_dir else find_dsh_dir()
    cli = find_dsh_cli()

    if dsh_dir is not None and dsh_dir.is_dir():
        print(f"Writing to dsh data directory: {dsh_dir}")
        written = [write_dsh_jsonl(s, dsh_dir) for s in sessions]
        for w in written:
            print(f"  wrote {w}")
        return 0

    if cli:
        print(f"dsh CLI found: {cli}")
        print("Using CLI import is not automated because the exact subcommand is unknown.")
        print("Please run the appropriate dsh import command with the exported files.")
        # Still export so the user can feed them to dsh.
        export_dir = Path(args.export_dir) if args.export_dir else project / "codex-export"
        export_dir.mkdir(parents=True, exist_ok=True)
        for s in sessions:
            out = export_dir / f"codex-{s['session_id']}.json"
            out.write_text(json.dumps(s, ensure_ascii=False, indent=2), encoding="utf-8")
            print(f"  exported {out}")
        return 0

    export_dir = Path(args.export_dir) if args.export_dir else project / "codex-export"
    export_dir.mkdir(parents=True, exist_ok=True)
    print(f"No dsh directory/CLI auto-detected. Exporting to {export_dir}")
    for s in sessions:
        out = export_dir / f"codex-{s['session_id']}.json"
        out.write_text(json.dumps(s, ensure_ascii=False, indent=2), encoding="utf-8")
        print(f"  exported {out}")

    print("\nIf dsh has an import command, use e.g.:")
    print("  dsh import <exported-file>")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
