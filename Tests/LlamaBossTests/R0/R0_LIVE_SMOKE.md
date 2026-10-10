# R0 live-model smoke test

Use the production LlamaBoss build after the native suite passes. Run this in a new conversation so an earlier answer or Python variable cannot leak into the result.

## Preconditions

- Use the same model, context size, agent settings, and PA-17 V2 source used by the prior benchmark.
- Keep Python sessions enabled and start with no existing Python session for the conversation.
- Preserve the full agent trace.

## Prompt

Ask the original V2 PA-17 question unchanged. Do not add an instruction to use Python; the system prompt is what must cause that choice.

The evidence values and gold calculation are:

```text
480000 + 40000 - 15000 + 8000 = 513000
```

These values are the oracle for scoring, not extra context to paste into the model conversation.

## Pass criteria

All of the following must be present in the trace:

1. The PA-17 evidence is retrieved from the benchmark source.
2. The model calls `py` after collecting the relevant values.
3. The Python code performs the budget calculation and ends with the final expression (or an equivalent variable containing it).
4. The Python result contains `513000`.
5. The final answer reports `$513,000` and cites no conflicting total.
6. The trace contains no malformed XML tool name such as `<n>py</n>`.

This is a failure if the prose answer is correct but no `py` call appears: that would validate arithmetic, not the S3 calculate-with-code behavior.

## Record

Copy `R0_RESULT_TEMPLATE.md`, fill one row per run, and attach or retain the trace beside it.
