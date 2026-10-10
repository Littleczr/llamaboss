# R0 result record

Build/commit:  
Date:  
Model:  
Context size:  
Prompt format: XML / native  
PA-17 V2 source/version:  

| Check | Result | Evidence |
| --- | --- | --- |
| Native `R0_S3_*` cases pass | PASS / FAIL | `TestResults/RESULTS.md` |
| PA-17 evidence retrieved | PASS / FAIL | Trace step(s): |
| `py` selected after retrieval | PASS / FAIL | Trace step: |
| Python code uses all four terms | PASS / FAIL | Code: |
| Python output is `513000` | PASS / FAIL | Output: |
| Final answer is `$513,000` | PASS / FAIL | Answer: |
| No malformed `<n>py</n>` call | PASS / FAIL | Trace search: |

Overall R0 result: PASS / FAIL  
Notes:  

R0 passes only when every check above passes. A correct final number without a `py` call is still an S3 failure.
