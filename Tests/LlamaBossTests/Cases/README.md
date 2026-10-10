# JSON regression cases

`LlamaBossTests.exe` discovers every `.json` file in this directory at runtime.
Adding or changing a supported case does not require recompiling the test
project; save the JSON file and run the existing executable again.

Use schema version `1`:

```json
{
  "schema_version": 1,
  "cases": []
}
```

## Supported actions

### `read_ranges`

Generates a deterministic numbered text file, calls the real production
`ReadFileRanges`, and verifies its body/error policy.

```json
{
  "name": "SingleRangeExample",
  "action": "read_ranges",
  "setup": {
    "file": "numbered.txt",
    "numbered_lines": 100
  },
  "input": {
    "path": "numbered.txt",
    "ctx_tokens": 131072,
    "ranges": [
      { "start": 25, "end": 30 }
    ]
  },
  "expect": {
    "success": true,
    "body_mode": "numbered_lines"
  }
}
```

Valid `body_mode` values are `numbered_lines`, `numbered_ranges`, and `empty`.
Optional expectations are `error_contains` and
`history_inline_budget_bytes`.

### `parse_grep`

Calls the same UI-free grep argument parser used by production
`tool_router.cpp`.

```json
{
  "name": "QuotedGrepExample",
  "action": "parse_grep",
  "input": {
    "args": "\"Record key: PA-17\" raw2.txt",
    "max_context_lines": 50
  },
  "expect": {
    "pattern": "Record key: PA-17",
    "path": "raw2.txt",
    "context_lines": 0,
    "error": ""
  }
}
```

Case names must be unique across all JSON and native C++ tests. Invalid JSON,
an unknown action, or an invalid field is a runner configuration error and
returns exit code `2`.
