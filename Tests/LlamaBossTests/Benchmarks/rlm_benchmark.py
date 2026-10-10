#!/usr/bin/env python3
"""Opt-in, JSON-driven end-to-end RLM benchmark runner.

This is deliberately separate from LlamaBossTests.exe's deterministic native
regression registry.  It talks to an OpenAI-compatible chat-completions
endpoint, so results are empirical measurements rather than unit-test truth.

The runner compares two modes against the same source and task:

* direct: the entire source is placed in the user message;
* rlm: only a compact variable card is placed in context and the model may
  retrieve evidence with bounded ``grep`` and ``read_range`` tools.

Only the benchmark-owned source is readable by the tools.  The runner never
executes model-provided shell commands or Python.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import statistics
import sys
import time
import urllib.error
import urllib.request
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Iterable


SCHEMA_VERSION = 1
MAX_HTTP_RESPONSE_BYTES = 8 * 1024 * 1024
MAX_GREP_MATCHES = 200
MAX_GREP_OUTPUT_BYTES = 64 * 1024
MAX_RANGES = 20
MAX_RANGE_LINES = 500
MAX_COMBINED_RANGE_LINES = 1000
MAX_TOOL_OUTPUT_BYTES = 128 * 1024
DEFAULT_MAX_STEPS = 8


class BenchmarkError(RuntimeError):
    pass


@dataclass
class Usage:
    prompt_tokens: int = 0
    completion_tokens: int = 0
    reported: bool = False

    @property
    def total_tokens(self) -> int:
        return self.prompt_tokens + self.completion_tokens

    def add_response(self, response: dict[str, Any]) -> None:
        raw = response.get("usage")
        if not isinstance(raw, dict):
            return
        prompt = raw.get("prompt_tokens", 0)
        completion = raw.get("completion_tokens", 0)
        if isinstance(prompt, int) and prompt >= 0:
            self.prompt_tokens += prompt
        if isinstance(completion, int) and completion >= 0:
            self.completion_tokens += completion
        self.reported = True


@dataclass
class RunResult:
    case: str
    mode: str
    repetition: int
    passed: bool = False
    score: float = 0.0
    found: list[str] = field(default_factory=list)
    missing: list[str] = field(default_factory=list)
    forbidden_hits: list[str] = field(default_factory=list)
    missing_tools: list[str] = field(default_factory=list)
    answer: str = ""
    error: str = ""
    tool_trace: list[dict[str, Any]] = field(default_factory=list)
    steps: int = 0
    elapsed_ms: int = 0
    source_bytes: int = 0
    context_payload_bytes: int = 0
    prompt_tokens: int | None = None
    completion_tokens: int | None = None
    total_tokens: int | None = None

    def to_json(self) -> dict[str, Any]:
        return {
            "case": self.case,
            "mode": self.mode,
            "repetition": self.repetition,
            "passed": self.passed,
            "score": round(self.score, 6),
            "found": self.found,
            "missing": self.missing,
            "forbidden_hits": self.forbidden_hits,
            "missing_tools": self.missing_tools,
            "answer": self.answer,
            "error": self.error,
            "tool_trace": self.tool_trace,
            "steps": self.steps,
            "elapsed_ms": self.elapsed_ms,
            "source_bytes": self.source_bytes,
            "context_payload_bytes": self.context_payload_bytes,
            "prompt_tokens": self.prompt_tokens,
            "completion_tokens": self.completion_tokens,
            "total_tokens": self.total_tokens,
        }


def require(condition: bool, message: str) -> None:
    if not condition:
        raise BenchmarkError(message)


def require_dict(value: Any, where: str) -> dict[str, Any]:
    require(isinstance(value, dict), f"{where} must be an object.")
    return value


def require_list(value: Any, where: str) -> list[Any]:
    require(isinstance(value, list), f"{where} must be an array.")
    return value


def require_string(value: Any, where: str) -> str:
    require(isinstance(value, str) and value != "", f"{where} must be a non-empty string.")
    return value


def safe_name(value: str) -> str:
    cleaned = re.sub(r"[^A-Za-z0-9_.-]+", "_", value).strip("._")
    return cleaned or "benchmark"


def load_case_files(cases_dir: Path) -> list[tuple[dict[str, Any], Path]]:
    require(cases_dir.is_dir(), f"Benchmark Cases directory not found: {cases_dir}")
    files = sorted(cases_dir.glob("*.json"), key=lambda p: p.name.lower())
    require(bool(files), f"Benchmark Cases directory contains no JSON files: {cases_dir}")

    loaded: list[tuple[dict[str, Any], Path]] = []
    names: set[str] = set()
    for path in files:
        try:
            root = json.loads(path.read_text(encoding="utf-8-sig"))
        except (OSError, json.JSONDecodeError) as exc:
            raise BenchmarkError(f"Invalid benchmark JSON {path}: {exc}") from exc
        root = require_dict(root, str(path))
        require(root.get("schema_version") == SCHEMA_VERSION,
                f"{path} requires schema_version {SCHEMA_VERSION}.")
        cases = require_list(root.get("benchmarks"), f"{path} benchmarks")
        for index, raw_case in enumerate(cases, start=1):
            case = require_dict(raw_case, f"{path} benchmark {index}")
            name = require_string(case.get("name"), f"{path} benchmark {index} name")
            require(name not in names, f"Duplicate benchmark name: {name}")
            names.add(name)
            validate_case(case, path)
            loaded.append((case, path))
    require(bool(loaded), "Benchmark files registered no cases.")
    return loaded


def validate_case(case: dict[str, Any], source_path: Path) -> None:
    name = case["name"]
    require_string(case.get("task"), f"{name}.task")
    source = require_dict(case.get("source"), f"{name}.source")
    source_type = source.get("type")
    require(source_type in ("generated_lines", "file"),
            f"{name}.source.type must be generated_lines or file.")
    if source_type == "generated_lines":
        line_count = source.get("line_count")
        require(isinstance(line_count, int) and 1 <= line_count <= 1_000_000,
                f"{name}.source.line_count must be between 1 and 1000000.")
        template = source.get("template", "line {line}: filler")
        require(isinstance(template, str), f"{name}.source.template must be a string.")
        patches = source.get("patches", [])
        require_list(patches, f"{name}.source.patches")
        seen_lines: set[int] = set()
        for index, raw_patch in enumerate(patches, start=1):
            patch = require_dict(raw_patch, f"{name}.source.patches[{index}]")
            line = patch.get("line")
            require(isinstance(line, int) and 1 <= line <= line_count,
                    f"{name} patch line must be inside the generated source.")
            require(line not in seen_lines, f"{name} has two patches for line {line}.")
            seen_lines.add(line)
            require(isinstance(patch.get("text"), str), f"{name} patch text must be a string.")
    else:
        relative = Path(require_string(source.get("path"), f"{name}.source.path"))
        require(not relative.is_absolute() and ".." not in relative.parts,
                f"{name}.source.path must stay beneath {source_path.parent}.")

    modes = case.get("modes", ["direct", "rlm"])
    require(isinstance(modes, list) and bool(modes), f"{name}.modes must be a non-empty array.")
    require(all(mode in ("direct", "rlm") for mode in modes),
            f"{name}.modes may contain only direct and rlm.")
    require(len(set(modes)) == len(modes), f"{name}.modes contains duplicates.")

    expect = require_dict(case.get("expect"), f"{name}.expect")
    required = require_list(expect.get("required"), f"{name}.expect.required")
    require(bool(required), f"{name}.expect.required must not be empty.")
    ids: set[str] = set()
    for index, raw_item in enumerate(required, start=1):
        item = require_dict(raw_item, f"{name}.expect.required[{index}]")
        item_id = require_string(item.get("id"), f"{name} required id")
        require(item_id not in ids, f"{name} has duplicate required id {item_id}.")
        ids.add(item_id)
        any_of = item.get("any_of", [])
        all_of = item.get("all_of", [])
        require(isinstance(any_of, list) and all(isinstance(x, str) and x for x in any_of),
                f"{name}.{item_id}.any_of must be an array of non-empty strings.")
        require(isinstance(all_of, list) and all(isinstance(x, str) and x for x in all_of),
                f"{name}.{item_id}.all_of must be an array of non-empty strings.")
        require(bool(any_of or all_of), f"{name}.{item_id} needs any_of or all_of.")
    minimum = expect.get("min_score", 1.0)
    require(isinstance(minimum, (int, float)) and 0.0 <= float(minimum) <= 1.0,
            f"{name}.expect.min_score must be between 0 and 1.")


def build_source(case: dict[str, Any], case_file: Path) -> tuple[str, str]:
    source = case["source"]
    if source["type"] == "generated_lines":
        line_count = source["line_count"]
        template = source.get("template", "line {line}: filler")
        patches = {item["line"]: item["text"] for item in source.get("patches", [])}
        lines = []
        for line_number in range(1, line_count + 1):
            text = patches.get(line_number)
            if text is None:
                try:
                    text = template.format(line=line_number)
                except (KeyError, ValueError) as exc:
                    raise BenchmarkError(
                        f"{case['name']} has an invalid source template: {exc}") from exc
            lines.append(text)
        return "\n".join(lines) + "\n", f"{safe_name(case['name'])}.txt"

    relative = Path(source["path"])
    resolved = (case_file.parent / relative).resolve()
    root = case_file.parent.resolve()
    require(resolved == root or root in resolved.parents,
            f"Source path escaped benchmark Cases folder: {relative}")
    try:
        return resolved.read_text(encoding=source.get("encoding", "utf-8")), relative.name
    except OSError as exc:
        raise BenchmarkError(f"Could not read benchmark source {resolved}: {exc}") from exc


def build_variable_card(source: str, source_name: str) -> str:
    lines = source.splitlines()
    head = lines[:14]
    tail = lines[-8:] if len(lines) > 22 else []
    parts = [
        "[LARGE OUTPUT -> stored as variable]",
        f"file: Vars/{source_name}  ({len(source.encode('utf-8'))} bytes, {len(lines)} lines)",
        "This is a PREVIEW ONLY. The complete content is stored outside the model context.",
        "Use grep to locate evidence and read_range to retrieve bounded neighborhoods.",
        "A whole-file read is unavailable in this benchmark.",
        "",
        f"-- head (first {len(head)} lines) --",
        *head,
    ]
    if tail:
        parts.extend([f"-- tail (last {len(tail)} lines) --", *tail])
    return "\n".join(parts)


TOOLS: list[dict[str, Any]] = [
    {
        "type": "function",
        "function": {
            "name": "grep",
            "description": "Search the external variable for a literal text pattern and return matching lines with optional context.",
            "parameters": {
                "type": "object",
                "properties": {
                    "pattern": {"type": "string"},
                    "path": {"type": "string"},
                    "context": {"type": "integer", "minimum": 0, "maximum": 50},
                },
                "required": ["pattern", "path"],
            },
        },
    },
    {
        "type": "function",
        "function": {
            "name": "read_range",
            "description": "Read one or several 1-based inclusive line ranges from the external variable.",
            "parameters": {
                "type": "object",
                "properties": {
                    "path": {"type": "string"},
                    "start": {"type": "integer", "minimum": 1},
                    "end": {"type": "integer", "minimum": 1},
                    "ranges": {
                        "type": "array",
                        "maxItems": MAX_RANGES,
                        "items": {
                            "type": "object",
                            "properties": {
                                "start": {"type": "integer", "minimum": 1},
                                "end": {"type": "integer", "minimum": 1},
                            },
                            "required": ["start", "end"],
                        },
                    },
                },
                "required": ["path"],
            },
        },
    },
]


def clamp_output(text: str, cap: int) -> str:
    encoded = text.encode("utf-8")
    if len(encoded) <= cap:
        return text
    clipped = encoded[:cap].decode("utf-8", errors="ignore")
    return clipped + "\n[output truncated by benchmark cap]"


class SourceTools:
    def __init__(self, source: str, source_name: str):
        self.source_name = source_name
        self.lines = source.splitlines()

    def _validate_path(self, value: Any) -> None:
        require(isinstance(value, str) and value != "", "Tool path must be a non-empty string.")
        normalized = value.replace("\\", "/")
        require(Path(normalized).name == self.source_name,
                f"Only Vars/{self.source_name} is readable in this benchmark.")

    def grep(self, args: dict[str, Any]) -> str:
        self._validate_path(args.get("path"))
        pattern = args.get("pattern")
        require(isinstance(pattern, str) and pattern != "", "grep pattern must be non-empty.")
        context = args.get("context", 0)
        require(isinstance(context, int) and 0 <= context <= 50,
                "grep context must be between 0 and 50.")
        needle = pattern.casefold()
        matching = [index for index, line in enumerate(self.lines) if needle in line.casefold()]
        if not matching:
            return "0 matches"
        matching = matching[:MAX_GREP_MATCHES]
        neighborhoods: list[tuple[int, int]] = []
        for index in matching:
            start = max(0, index - context)
            end = min(len(self.lines), index + context + 1)
            if neighborhoods and start <= neighborhoods[-1][1]:
                neighborhoods[-1] = (neighborhoods[-1][0], max(neighborhoods[-1][1], end))
            else:
                neighborhoods.append((start, end))
        output: list[str] = []
        matching_set = set(matching)
        for block_index, (start, end) in enumerate(neighborhoods):
            if block_index:
                output.append("--")
            for line_index in range(start, end):
                marker = ":" if line_index in matching_set else "-"
                output.append(f"{self.source_name}:{line_index + 1}{marker}{self.lines[line_index]}")
        suffix = ""
        if len(matching) == MAX_GREP_MATCHES:
            suffix = f"\n[match cap {MAX_GREP_MATCHES} reached]"
        return clamp_output("\n".join(output) + suffix, MAX_GREP_OUTPUT_BYTES)

    def read_range(self, args: dict[str, Any]) -> str:
        self._validate_path(args.get("path"))
        raw_ranges = args.get("ranges")
        if raw_ranges is None:
            raw_ranges = [{"start": args.get("start"), "end": args.get("end")}]
        require(isinstance(raw_ranges, list) and raw_ranges,
                "read_range requires ranges or start/end.")
        require(len(raw_ranges) <= MAX_RANGES,
                f"read_range accepts at most {MAX_RANGES} ranges.")

        ranges: list[tuple[int, int]] = []
        requested = 0
        previous_end = 0
        for raw in raw_ranges:
            require(isinstance(raw, dict), "read_range entries must be objects.")
            start = raw.get("start")
            end = raw.get("end")
            require(isinstance(start, int) and isinstance(end, int),
                    "read_range start/end must be integers.")
            require(start >= 1 and end >= start, "read_range requires 1 <= start <= end.")
            require(start > previous_end, "read_range ranges must be ascending and non-overlapping.")
            require(start <= len(self.lines),
                    f"read_range starts beyond EOF ({len(self.lines)} lines).")
            end = min(end, start + MAX_RANGE_LINES - 1, len(self.lines))
            requested += end - start + 1
            require(requested <= MAX_COMBINED_RANGE_LINES,
                    f"read_range exceeds {MAX_COMBINED_RANGE_LINES} combined lines.")
            ranges.append((start, end))
            previous_end = end

        output: list[str] = []
        for index, (start, end) in enumerate(ranges, start=1):
            if len(ranges) > 1:
                output.append(f"[range {index}: lines {start}-{end} of {len(self.lines)}]")
            output.extend(self.lines[start - 1:end])
        return clamp_output("\n".join(output) + "\n", MAX_TOOL_OUTPUT_BYTES)

    def execute(self, name: str, arguments: Any) -> str:
        require(isinstance(arguments, dict), f"{name} arguments must be an object.")
        if name == "grep":
            return self.grep(arguments)
        if name == "read_range":
            return self.read_range(arguments)
        raise BenchmarkError(f"Unsupported tool requested by model: {name}")


class OpenAIClient:
    def __init__(self, endpoint: str, model: str, api_key: str | None, timeout: float):
        self.endpoint = endpoint
        self.model = model
        self.api_key = api_key
        self.timeout = timeout

    def complete(self, messages: list[dict[str, Any]], *, tools: list[dict[str, Any]] | None,
                 temperature: float, max_tokens: int, seed: int | None) -> dict[str, Any]:
        payload: dict[str, Any] = {
            "model": self.model,
            "messages": messages,
            "stream": False,
            "temperature": temperature,
            "max_tokens": max_tokens,
        }
        if seed is not None:
            payload["seed"] = seed
        if tools:
            payload["tools"] = tools
            payload["tool_choice"] = "auto"
            payload["parallel_tool_calls"] = True
        data = json.dumps(payload, ensure_ascii=False).encode("utf-8")
        headers = {"Content-Type": "application/json", "Accept": "application/json"}
        if self.api_key:
            headers["Authorization"] = f"Bearer {self.api_key}"
        request = urllib.request.Request(self.endpoint, data=data, headers=headers, method="POST")
        try:
            with urllib.request.urlopen(request, timeout=self.timeout) as response:
                body = response.read(MAX_HTTP_RESPONSE_BYTES + 1)
                require(len(body) <= MAX_HTTP_RESPONSE_BYTES,
                        "Endpoint response exceeded the 8 MiB safety cap.")
        except urllib.error.HTTPError as exc:
            detail = exc.read(64 * 1024).decode("utf-8", errors="replace")
            raise BenchmarkError(f"Endpoint returned HTTP {exc.code}: {detail}") from exc
        except urllib.error.URLError as exc:
            raise BenchmarkError(f"Could not reach endpoint {self.endpoint}: {exc.reason}") from exc
        try:
            parsed = json.loads(body.decode("utf-8"))
        except (UnicodeDecodeError, json.JSONDecodeError) as exc:
            raise BenchmarkError(f"Endpoint returned invalid JSON: {exc}") from exc
        require(isinstance(parsed, dict), "Endpoint JSON root must be an object.")
        return parsed


def extract_message(response: dict[str, Any]) -> dict[str, Any]:
    choices = response.get("choices")
    require(isinstance(choices, list) and choices, "Endpoint response has no choices.")
    first = require_dict(choices[0], "response choice")
    message = require_dict(first.get("message"), "response choice message")
    return message


def parse_tool_arguments(raw: Any) -> dict[str, Any]:
    if isinstance(raw, dict):
        return raw
    require(isinstance(raw, str), "Tool arguments must be a JSON string or object.")
    require(len(raw.encode("utf-8")) <= 256 * 1024, "Tool arguments exceeded 256 KiB.")
    try:
        parsed = json.loads(raw)
    except json.JSONDecodeError as exc:
        raise BenchmarkError(f"Model emitted invalid tool arguments: {exc}") from exc
    return require_dict(parsed, "tool arguments")


def run_model(client: OpenAIClient, case: dict[str, Any], mode: str,
              source: str, source_name: str, args: argparse.Namespace) -> tuple[str, list[dict[str, Any]], int, Usage, int]:
    if mode == "direct":
        system = (
            "You are being evaluated on long-context question answering. "
            "Answer only from the supplied source. Be concise and preserve exact names, dates, and values."
        )
        user = f"TASK:\n{case['task']}\n\nSOURCE ({source_name}):\n{source}"
        tools = None
        context_payload_bytes = len(source.encode("utf-8"))
    else:
        card = build_variable_card(source, source_name)
        system = (
            "You are being evaluated on variable-backed long-context retrieval. "
            "The complete source is outside your context. Use grep to locate every relevant block, "
            "then read_range to retrieve sufficient evidence before answering. Do not invent facts. "
            "When evidence occurs in several places, cover all of it before synthesis.\n\n" + card
        )
        user = f"TASK:\n{case['task']}"
        tools = TOOLS
        context_payload_bytes = len(card.encode("utf-8"))

    messages: list[dict[str, Any]] = [
        {"role": "system", "content": system},
        {"role": "user", "content": user},
    ]
    source_tools = SourceTools(source, source_name)
    trace: list[dict[str, Any]] = []
    usage = Usage()

    for step in range(1, args.max_steps + 1):
        response = client.complete(
            messages,
            tools=tools,
            temperature=args.temperature,
            max_tokens=args.max_tokens,
            seed=args.seed,
        )
        usage.add_response(response)
        message = extract_message(response)
        content = message.get("content")
        if content is None:
            content = ""
        require(isinstance(content, str), "Assistant content must be a string or null.")
        raw_tool_calls = message.get("tool_calls", [])
        require(isinstance(raw_tool_calls, list), "Assistant tool_calls must be an array.")

        assistant_message: dict[str, Any] = {"role": "assistant", "content": content or None}
        if raw_tool_calls:
            assistant_message["tool_calls"] = raw_tool_calls
        messages.append(assistant_message)

        if not raw_tool_calls:
            require(content.strip() != "", "Model returned neither content nor tool calls.")
            return content, trace, step, usage, context_payload_bytes

        require(mode == "rlm", "Direct mode unexpectedly emitted tool calls.")
        for call_index, raw_call in enumerate(raw_tool_calls, start=1):
            call = require_dict(raw_call, "tool call")
            function = require_dict(call.get("function"), "tool call function")
            name = require_string(function.get("name"), "tool name")
            call_id = call.get("id")
            if not isinstance(call_id, str) or not call_id:
                call_id = f"benchmark_call_{step}_{call_index}"
            parsed_args = parse_tool_arguments(function.get("arguments", "{}"))
            try:
                output = source_tools.execute(name, parsed_args)
                ok = True
            except BenchmarkError as exc:
                output = f"ERROR: {exc}"
                ok = False
            trace.append({
                "step": step,
                "tool": name,
                "arguments": parsed_args,
                "ok": ok,
                "output": output,
            })
            messages.append({
                "role": "tool",
                "tool_call_id": call_id,
                "content": output,
            })

    raise BenchmarkError(f"Model did not produce a final answer within {args.max_steps} steps.")


def contains(text: str, needle: str, case_sensitive: bool) -> bool:
    if case_sensitive:
        return needle in text
    return needle.casefold() in text.casefold()


def grade_answer(answer: str, expect: dict[str, Any], mode: str,
                 trace: list[dict[str, Any]]) -> tuple[bool, float, list[str], list[str], list[str], list[str]]:
    case_sensitive = bool(expect.get("case_sensitive", False))
    found: list[str] = []
    missing: list[str] = []
    for item in expect["required"]:
        any_of = item.get("any_of", [])
        all_of = item.get("all_of", [])
        any_ok = not any_of or any(contains(answer, value, case_sensitive) for value in any_of)
        all_ok = all(contains(answer, value, case_sensitive) for value in all_of)
        (found if any_ok and all_ok else missing).append(item["id"])
    score = len(found) / len(expect["required"])
    forbidden_hits = [
        value for value in expect.get("forbidden", [])
        if isinstance(value, str) and value and contains(answer, value, case_sensitive)
    ]
    missing_tools: list[str] = []
    if mode == "rlm":
        used = {entry["tool"] for entry in trace if entry.get("ok")}
        missing_tools = [tool for tool in expect.get("required_tools", []) if tool not in used]
    passed = (
        score >= float(expect.get("min_score", 1.0))
        and not forbidden_hits
        and not missing_tools
    )
    return passed, score, found, missing, forbidden_hits, missing_tools


def run_one(client: OpenAIClient, case: dict[str, Any], case_file: Path,
            mode: str, repetition: int, args: argparse.Namespace) -> RunResult:
    source, source_name = build_source(case, case_file)
    result = RunResult(
        case=case["name"],
        mode=mode,
        repetition=repetition,
        source_bytes=len(source.encode("utf-8")),
    )
    started = time.monotonic()
    try:
        answer, trace, steps, usage, context_payload_bytes = run_model(
            client, case, mode, source, source_name, args)
        result.answer = answer
        result.tool_trace = trace
        result.steps = steps
        result.context_payload_bytes = context_payload_bytes
        if usage.reported:
            result.prompt_tokens = usage.prompt_tokens
            result.completion_tokens = usage.completion_tokens
            result.total_tokens = usage.total_tokens
        graded = grade_answer(answer, case["expect"], mode, trace)
        (result.passed, result.score, result.found, result.missing,
         result.forbidden_hits, result.missing_tools) = graded
    except Exception as exc:  # Each empirical run must still reach the report.
        result.error = str(exc)
    result.elapsed_ms = round((time.monotonic() - started) * 1000)
    return result


def mean_or_none(values: Iterable[int | None]) -> float | None:
    concrete = [value for value in values if value is not None]
    return round(statistics.mean(concrete), 2) if concrete else None


def aggregates(results: list[RunResult]) -> list[dict[str, Any]]:
    groups: dict[tuple[str, str], list[RunResult]] = {}
    for result in results:
        groups.setdefault((result.case, result.mode), []).append(result)
    rows: list[dict[str, Any]] = []
    for (case, mode), items in sorted(groups.items()):
        rows.append({
            "case": case,
            "mode": mode,
            "runs": len(items),
            "passed": sum(1 for item in items if item.passed),
            "pass_rate": round(sum(1 for item in items if item.passed) / len(items), 6),
            "mean_score": round(statistics.mean(item.score for item in items), 6),
            "mean_prompt_tokens": mean_or_none(item.prompt_tokens for item in items),
            "mean_total_tokens": mean_or_none(item.total_tokens for item in items),
            "mean_elapsed_ms": round(statistics.mean(item.elapsed_ms for item in items), 2),
            "mean_context_payload_bytes": round(
                statistics.mean(item.context_payload_bytes for item in items), 2),
        })
    return rows


def markdown_escape(value: str) -> str:
    return value.replace("|", "\\|").replace("\n", " ")


def write_reports(output_dir: Path, args: argparse.Namespace,
                  results: list[RunResult]) -> None:
    output_dir.mkdir(parents=True, exist_ok=True)
    transcript_dir = output_dir / "transcripts"
    transcript_dir.mkdir(parents=True, exist_ok=True)
    aggregate_rows = aggregates(results)
    generated = time.strftime("%Y-%m-%d %H:%M:%S")

    document = {
        "schema_version": SCHEMA_VERSION,
        "generated": generated,
        "endpoint": args.endpoint,
        "model": args.model,
        "temperature": args.temperature,
        "repetitions": args.repetitions,
        "aggregates": aggregate_rows,
        "runs": [result.to_json() for result in results],
    }
    (output_dir / "benchmark_results.json").write_text(
        json.dumps(document, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")

    for result in results:
        name = safe_name(f"{result.case}_{result.mode}_{result.repetition}") + ".json"
        (transcript_dir / name).write_text(
            json.dumps(result.to_json(), indent=2, ensure_ascii=False) + "\n", encoding="utf-8")

    lines = [
        "# LlamaBoss RLM Benchmark Results",
        "",
        f"Generated: {generated}",
        "",
        f"Model: `{args.model}`  ",
        f"Endpoint: `{args.endpoint}`  ",
        f"Temperature: {args.temperature}  ",
        f"Repetitions: {args.repetitions}",
        "",
        "These are empirical model evaluations, not deterministic native regression tests.",
        "",
        "| Case | Mode | Passed | Pass rate | Mean score | Mean prompt tokens | Mean total tokens | Mean time | Context payload |",
        "| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |",
    ]
    for row in aggregate_rows:
        prompt = "n/a" if row["mean_prompt_tokens"] is None else f"{row['mean_prompt_tokens']:.0f}"
        total = "n/a" if row["mean_total_tokens"] is None else f"{row['mean_total_tokens']:.0f}"
        lines.append(
            f"| {markdown_escape(row['case'])} | {row['mode']} | {row['passed']}/{row['runs']} "
            f"| {row['pass_rate']:.0%} | {row['mean_score']:.0%} | {prompt} | {total} "
            f"| {row['mean_elapsed_ms'] / 1000:.2f}s | {row['mean_context_payload_bytes'] / 1024:.1f} KiB |"
        )
    failures = [result for result in results if not result.passed]
    if failures:
        lines.extend(["", "## Failed runs", ""])
        for result in failures:
            detail = result.error or (
                f"missing facts={result.missing}; forbidden={result.forbidden_hits}; "
                f"missing tools={result.missing_tools}"
            )
            lines.append(
                f"- `{result.case}` / `{result.mode}` / repetition {result.repetition}: "
                f"{markdown_escape(detail)}"
            )
    lines.extend([
        "",
        "Full answers and bounded tool traces are in `benchmark_results.json` and `transcripts/`.",
        "",
    ])
    (output_dir / "BENCHMARK_RESULTS.md").write_text("\n".join(lines), encoding="utf-8")


def self_test() -> None:
    case = {
        "name": "SelfTest",
        "task": "Find the needle.",
        "source": {
            "type": "generated_lines",
            "line_count": 400,
            "template": "line {line}: filler",
            "patches": [{"line": 250, "text": "line 250: NEEDLE_XYZZY"}],
        },
        "expect": {
            "required": [{"id": "needle", "any_of": ["NEEDLE_XYZZY"]}],
            "required_tools": ["grep", "read_range"],
        },
    }
    source, name = build_source(case, Path("self_test.json"))
    require("NEEDLE_XYZZY" in source, "Generated source self-test failed.")
    tools = SourceTools(source, name)
    grep_output = tools.grep({"pattern": "needle_xyZZy", "path": f"Vars/{name}", "context": 1})
    require(f"{name}:250:" in grep_output, "grep self-test failed.")
    ranged = tools.read_range({"path": name, "ranges": [{"start": 249, "end": 251}]})
    require("NEEDLE_XYZZY" in ranged, "read_range self-test failed.")
    grade = grade_answer("The value is NEEDLE_XYZZY.", case["expect"], "rlm", [
        {"tool": "grep", "ok": True}, {"tool": "read_range", "ok": True}
    ])
    require(grade[0] and grade[1] == 1.0, "grader self-test failed.")
    card = build_variable_card(source, name)
    require("PREVIEW ONLY" in card and len(card) < len(source), "variable-card self-test failed.")
    print("RLM benchmark self-test passed.")


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Compare direct-context and variable-backed RLM behavior on an OpenAI-compatible model.")
    parser.add_argument("--cases", type=Path, help="Directory containing benchmark JSON files.")
    parser.add_argument("--output", type=Path, default=Path("BenchmarkResults"),
                        help="Directory for benchmark_results.json, BENCHMARK_RESULTS.md, and transcripts.")
    parser.add_argument("--endpoint", help="Full OpenAI-compatible /v1/chat/completions URL.")
    parser.add_argument("--model", help="Model id sent in the request body.")
    parser.add_argument("--api-key-env", default="LLAMABOSS_BENCHMARK_API_KEY",
                        help="Environment variable containing a bearer token; unset means no authentication.")
    parser.add_argument("--modes", default="direct,rlm",
                        help="Comma-separated subset of direct,rlm. Case modes still apply.")
    parser.add_argument("--repetitions", type=int, default=1)
    parser.add_argument("--temperature", type=float, default=0.0)
    parser.add_argument("--max-tokens", type=int, default=2048)
    parser.add_argument("--max-steps", type=int, default=DEFAULT_MAX_STEPS)
    parser.add_argument("--timeout", type=float, default=300.0)
    parser.add_argument("--seed", type=int)
    parser.add_argument("--filter", default="", help="Run only case names containing this text.")
    parser.add_argument("--dry-run", action="store_true", help="Validate cases and report generated sizes without calling a model.")
    parser.add_argument("--self-test", action="store_true", help="Run offline tests of generation, retrieval, and grading.")
    args = parser.parse_args(argv)

    if args.self_test:
        return args
    require(args.cases is not None, "--cases is required.")
    if not args.dry_run:
        require(bool(args.endpoint), "--endpoint is required unless --dry-run is used.")
        require(bool(args.model), "--model is required unless --dry-run is used.")
    require(1 <= args.repetitions <= 100, "--repetitions must be between 1 and 100.")
    require(0.0 <= args.temperature <= 2.0, "--temperature must be between 0 and 2.")
    require(1 <= args.max_tokens <= 1_000_000, "--max-tokens must be positive.")
    require(1 <= args.max_steps <= 32, "--max-steps must be between 1 and 32.")
    require(args.timeout > 0, "--timeout must be positive.")
    requested_modes = [mode.strip() for mode in args.modes.split(",") if mode.strip()]
    require(bool(requested_modes) and all(mode in ("direct", "rlm") for mode in requested_modes),
            "--modes must contain direct, rlm, or both.")
    args.requested_modes = requested_modes
    return args


def main(argv: list[str]) -> int:
    try:
        args = parse_args(argv)
        if args.self_test:
            self_test()
            return 0

        cases = load_case_files(args.cases.resolve())
        if args.filter:
            cases = [item for item in cases if args.filter.casefold() in item[0]["name"].casefold()]
            require(bool(cases), f"No benchmark names matched filter: {args.filter}")

        if args.dry_run:
            print(f"Validated {len(cases)} benchmark case(s).")
            for case, path in cases:
                source, source_name = build_source(case, path)
                card = build_variable_card(source, source_name)
                print(
                    f"{case['name']}: source={len(source.encode('utf-8'))} bytes, "
                    f"RLM card={len(card.encode('utf-8'))} bytes, modes={case.get('modes', ['direct', 'rlm'])}"
                )
            return 0

        api_key = os.environ.get(args.api_key_env) if args.api_key_env else None
        client = OpenAIClient(args.endpoint, args.model, api_key, args.timeout)
        results: list[RunResult] = []
        for case, path in cases:
            case_modes = case.get("modes", ["direct", "rlm"])
            modes = [mode for mode in args.requested_modes if mode in case_modes]
            for repetition in range(1, args.repetitions + 1):
                for mode in modes:
                    print(f"[RUN] {case['name']} / {mode} / repetition {repetition}")
                    result = run_one(client, case, path, mode, repetition, args)
                    print(
                        f"[{'PASS' if result.passed else 'FAIL'}] score={result.score:.0%} "
                        f"tools={len(result.tool_trace)} time={result.elapsed_ms / 1000:.2f}s"
                    )
                    results.append(result)
        require(bool(results), "No benchmark runs were selected.")
        write_reports(args.output.resolve(), args, results)
        failures = sum(1 for result in results if not result.passed)
        print(f"\n{len(results) - failures} passed, {failures} failed")
        print(f"Reports: {args.output.resolve()}")
        return 0 if failures == 0 else 1
    except BenchmarkError as exc:
        print(f"Benchmark configuration error: {exc}", file=sys.stderr)
        return 2
    except KeyboardInterrupt:
        print("Benchmark cancelled.", file=sys.stderr)
        return 130


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
