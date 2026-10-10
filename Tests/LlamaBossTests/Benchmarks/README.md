# LlamaBoss RLM model benchmark

This opt-in benchmark complements `LlamaBossTests.exe`.

- `LlamaBossTests.exe` remains the offline, deterministic native regression suite.
- `rlm_benchmark.py` measures end-to-end model quality, retrieval behavior, token usage, and latency against an OpenAI-compatible endpoint.

The benchmark gives the same task and source to the same model in two modes:

1. `direct` inserts the complete source into the request.
2. `rlm` inserts a compact variable card and exposes bounded `grep` and `read_range` tools.

Model output is nondeterministic, so benchmark failures do not make the native regression suite fail.

## Offline validation

From the repository root:

```powershell
py Tests\LlamaBossTests\Benchmarks\rlm_benchmark.py --self-test

py Tests\LlamaBossTests\Benchmarks\rlm_benchmark.py `
  --cases Tests\LlamaBossTests\Benchmarks\Cases `
  --dry-run
```

Neither command contacts a model.

## Run against local Qwen

Start the same OpenAI-compatible llama-server endpoint used by LlamaBoss, then run:

```powershell
py Tests\LlamaBossTests\Benchmarks\rlm_benchmark.py `
  --cases Tests\LlamaBossTests\Benchmarks\Cases `
  --output BenchmarkResults `
  --endpoint http://127.0.0.1:8080/v1/chat/completions `
  --model Qwen3.8-27B-UD-Q4_K_XL `
  --temperature 0 `
  --repetitions 3
```

Use the exact endpoint port and wire model id shown by the active LlamaBoss model server. To run only variable-backed retrieval during initial validation, add `--modes rlm`.

## Remote endpoints

Secrets never belong in benchmark JSON. Put the bearer token in an environment variable and name it with `--api-key-env`:

```powershell
$env:LLAMABOSS_BENCHMARK_API_KEY = '<temporary token>'

py Tests\LlamaBossTests\Benchmarks\rlm_benchmark.py `
  --cases Tests\LlamaBossTests\Benchmarks\Cases `
  --output BenchmarkResults `
  --endpoint https://provider.example/v1/chat/completions `
  --model provider/model-id `
  --api-key-env LLAMABOSS_BENCHMARK_API_KEY
```

The token is read at runtime and is never written to reports.

## Results

The output folder contains:

- `BENCHMARK_RESULTS.md`: comparison table and concise failure list.
- `benchmark_results.json`: complete machine-readable measurements.
- `transcripts/*.json`: final answer and bounded tool trace for each run.

Recorded measurements include required-fact coverage, pass rate, source size, context payload size, model-reported token usage when available, tool calls, steps, and wall time.

Exit codes:

- `0`: every empirical run met its grading contract.
- `1`: one or more completed runs missed their grading contract.
- `2`: invalid cases, CLI configuration, endpoint response, or report setup.
- `130`: cancelled by the user.

## JSON schema

Benchmark files use `schema_version: 1` and a `benchmarks` array. Each case defines:

- a unique `name`;
- a natural-language `task`;
- `modes` containing `direct`, `rlm`, or both;
- a generated or file-backed `source`;
- required answer facts, forbidden conclusions, optional required RLM tools, and a minimum coverage score under `expect`.

The model may access only the source owned by the current benchmark case. The tool executor does not run shell commands, Python, or model-selected files.
