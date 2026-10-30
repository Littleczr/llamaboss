# Strata vs Qwen3.8-27B: local model test results

*30 October 2026 · Same Windows PC (one RTX 5090, CUDA), one model loaded at a time · 59 auto-scored tests plus long-context, speed and writing checks*

These are notes from one machine, shared for awareness. Both models were run as local providers that LlamaBoss can talk to. They are not a formal benchmark; see [Caveats](#caveats).

## Summary

**Strata scored 59/59 and Qwen3.8-27B scored 56/59.** The 27B never gave a wrong final answer. All three of its misses came from **thinking too long**: it used up the whole token budget before writing an answer. In one case the long thinking also led it to **invent facts**.

Strata was also faster at writing (about 190 against 123 tokens/s). The 27B was faster at reading long documents (48k tokens in about 22 s against about 60 s).

Strata works with LlamaBoss as a plain custom endpoint (no API key, OpenAI-compatible), with no code changes needed.

## The two setups

| | Strata | Qwen3.8-27B |
|---|---|---|
| Model | qwen3.8-flash-next-abliterated, IQ2_XS (MoE) | Qwen3.8-27B-Uncensored, Q4_K_M (15.7 GB) + vision projector (dense) |
| Server | Strata custom engine (OpenAI + Anthropic API) | llama.cpp `llama-server`, CUDA 12, MTP speculative decoding (draft 2) |
| Context | 131,072 | 131,072 (KV cache q8_0, flash attention) |
| Sampling | Temperature 0 for the main run, 6,000-token cap per answer | Same. Stuck tests re-run 3× at Qwen's recommended settings (temp 0.6, top_p 0.95, top_k 20) with a 12,000-token cap |

Note that both are community uncensored/abliterated variants, not the stock Qwen releases, and they are different models. This compares two local setups, not Strata against llama.cpp running the same model.

## Scoreboard

| Category | What it checks | Strata | 27B |
|---|---|---|---|
| Math | 4-digit multiplication, trick questions, algebra, combinatorics, modular arithmetic | 8/8 | 8/8 |
| Logic | Letter counting, word reversal, knights and knaves, river crossing | 7/7 | 7/7 |
| Knowledge | Facts, plus a made-up novel to test whether it invents an answer | **8/8** | 7/8 |
| Instructions | JSON only, CSV only, exactly 5 words, no letter "e", system-prompt persona | 7/7 | 7/7 |
| Code | 6 functions + 1 bug fix, each run against unit tests | **7/7** | 5/7 |
| Languages | Spanish, French, Chinese, Japanese | 4/4 | 4/4 |
| Tool calls | Right function and arguments; no call when none is needed | 3/3 | 3/3 |
| Vision | Colors, counting shapes, left vs right | 3/3 | 3/3 |
| Writing | Length limits, email, one-sentence summary | 3/3 | 3/3 |
| Long context | A code hidden at 10/50/90% of 4k, 16k and 48k-token texts | 9/9 | 9/9 |
| **Total** | | **59/59** | 56/59 |

## Speed

| Measure | Strata | 27B |
|---|---|---|
| Writing speed (400-word story) | **~190 tok/s** | ~123 tok/s |
| Time to first token | **0.4 s** | 1.1 s |
| Reading a 48k-token document | ~60 s | **~22 s** |
| Median answer length (incl. thinking) | 121 tok | 98 tok |
| Answers that hit the 6,000 cap with no answer | **0** | 4 |

On short, simple prompts the 27B's thinking is actually shorter than Strata's. The problem is the long tail: on a handful of prompts it never stops.

The 27B also ran the GPU noticeably harder (more fan and coil noise). That fits the architectures: a dense model runs all its weights for every token, while a MoE model only runs a few experts per token.

## Findings

### 1. The 27B gets stuck in thinking loops (main issue)

On 4 of 61 prompts it thought until it hit the limit and produced no answer. Its reasoning repeated the same few paragraphs: in one trace, only 36 of 83 lines were distinct, and one paragraph appeared 8 times word for word.

### 2. The loop also caused it to invent facts (trust risk)

Asked to summarize a novel that doesn't exist, Strata replied that it had no reliable information about it. The 27B invented an author biography in its thinking and then, at recommended settings, presented an invented plot as fact in 2 of 3 runs. The third run ran out of tokens.

> "Tavish D. Quinlan (1934-2011) American writer. The Glass Orchard of Pelmora is a fantasy novel. I need accurate plot…"
> …then 23,000 characters cycling between invented plot ideas.

This rests on a single made-up-entity prompt, so treat it as a warning sign rather than a measured rate.

### 3. Recommended sampling helps only partly

Qwen warns that temperature 0 can cause repetition in thinking mode, so the stuck tests were re-run 3× at the recommended settings with double the token budget:

| Test | Passed | Tokens per run | Time per run |
|---|---|---|---|
| Made-up novel | 0/3 | 8.3k – 12k | 71 – 92 s |
| Dijkstra shortest path | 1/3 | 3.5k – 12k | 29 – 101 s |
| Word frequency | 2/3 | 7.4k – 12k | 63 – 108 s |
| 400-word story | 3/3 | 5.3k – 8.6k | 39 – 70 s |

When it finishes, the result is good: the code passes its tests and the story is well written. On the same prompts Strata needed about 4.6k tokens and 45 s for the code, and 8 s for the story.

### 4. Strata overthinks too, but finishes (minor)

On vague instructions Strata also spends a long time thinking. For "describe the ocean using only lowercase letters" it wrote about 20,000 characters debating whether periods count as lowercase letters, then answered correctly. It never hit the cap. Its 4.6k tokens on the Dijkstra task were also not far from the 6,000 cap.

### 5. Everything else is a tie

Both models were perfect on math, logic, instructions, languages, tool calls, vision and finding text in long documents (48k tokens). The tests were too easy to separate them on raw reasoning, so the differences above come almost entirely from thinking length and honesty.

## Why the 27B might overthink

This matches what I see in day-to-day use: once, after being told "you are overthinking", the model agreed in its own reasoning. A likely explanation is training: reasoning models are trained with reinforcement learning that rewards a correct final answer and barely penalizes length, so re-checking "just once more" pays off. Heavy quantization and uncensoring fine-tunes may also weaken the model's sense of when to stop. These tests show the behavior; they don't prove the cause.

Ways to reduce it:

- Use Qwen's recommended sampling (temp 0.6, top_p 0.95, top_k 20) instead of greedy decoding. A small presence penalty can also break up repeated paragraphs.
- Cap the thinking: set a max-tokens limit, and have the client stop and ask for the answer when the thinking repeats or passes a budget.
- Turn thinking off for simple lookups and chat, if the chat template supports a no-think switch. Keep it on for math and code.
- For factual questions, a system prompt that explicitly allows "I don't know" makes inventing an answer less likely.

## Related: llama.cpp now has an MoE expert cache

Strata's speed comes from keeping the most-used MoE experts in VRAM. Mainline llama.cpp merged a similar feature on 7 October 2026 ([PR #29887](https://github.com/ggml-org/llama.cpp/pull/29887), multi-GPU in #30112): `--moe-cache-mib N` keeps recently used experts on the GPU while the rest stay in RAM. It is off by default, it speeds up generation but not long-prompt reading, and its benefit depends on the cache hit rate. Check your build with `llama-server --help | findstr moe-cache`.

A natural follow-up is running the same Qwen3.8-Flash-Next IQ2_XS GGUF on both Strata and `llama-server --cpu-moe --moe-cache-mib …`, comparing generation speed, long-prompt reading speed and VRAM. These notes don't include that test yet.

## Caveats

- The 27B was tested directly through llama-server. A chat app that stops runaway reasoning on its own may feel better in practice than these numbers suggest.
- Each prompt was run once at temperature 0, except the 27B re-runs above. Strata was not re-run at sampled settings, so the comparison favors Strata somewhat.
- 59 tests is a small set, and most were easy for both models. A harder set would be needed to compare their raw reasoning ability.
- One machine, one run. Your numbers will differ with other GPUs, RAM speeds and driver versions.
