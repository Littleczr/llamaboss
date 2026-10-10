# R0 test drop

This drop adds one generic deterministic JSON action and two R0 S3 cases.

## Files changed

- `Tests/LlamaBossTests/json_case_loader.cpp`
  - Adds `source_contract`, which locates a repository-relative source file and checks required, forbidden, and exact-occurrence text.
- `Tests/LlamaBossTests/Cases/r0_s3_contract.json`
  - Locks the corrected `<name>py</name>` example.
  - Rejects the malformed `<n>py</n>` example.
  - Requires the calculate-with-code rule in both XML and native prompt branches.
  - Locks the persistent-kernel namespace and trailing-expression behavior.
- `Tests/LlamaBossTests/R0/R0_LIVE_SMOKE.md`
  - Defines the live PA-17 exit criteria.
- `Tests/LlamaBossTests/R0/R0_RESULT_TEMPLATE.md`
  - Records the native and live evidence in one place.

## Run

Rebuild `LlamaBossTests`, then run the normal suite from the repository root. To run only the new cases:

```powershell
.\LlamaBossTests.exe --filter R0_S3_ --cases Tests\LlamaBossTests\Cases
```

The deterministic cases prove that the production source still contains the correct prompt and persistent-session contracts. They intentionally do not claim that a model will follow the prompt. The live PA-17 smoke test is the behavioral proof.
