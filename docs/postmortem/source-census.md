# G-Diffuser Claude Code usage census

Scanned 1265 jsonl files, 81,491 records, across: `<project>` (1264 files), `<project-2>` (1 files)

Date range: **2026-08-11T03:15:08.663Z** to **2026-09-03T19:04:41.247Z** (15 distinct active UTC days).

## Pricing assumptions ($/M tokens, input/output)

| model | scenario A | scenario B |
|---|---|---|
| claude-opus-4-8 | 5/25 | 5/25 |
| claude-opus-5 | 5/25 | 5/25 |
| claude-fable-5 | 10/50 | 10/50 |
| claude-fable-5-1 | 10/50 | 10/50 |
| claude-sonnet-* | 3/15 | 3/15 |
| claude-haiku-* | 1/5 | 1/5 |
| <synthetic>/unknown | 0/0 | 0/0 |

Published list prices (platform.claude.com/docs/en/about-claude/pricing, fetched 2026-09-03). Scenario A: 5-minute cache writes (1.25x input). Scenario B: 1-hour cache writes (2x input), the TTL Claude Code used. Cache reads 0.10x input, except Fable 5.1 at 0.025x ($0.25/MTok).

## 1. Token totals

| scope | input | output | cache_creation | cache_read | assistant turns (API calls) |
|---|---|---|---|---|---|
| main | 314,331 | 1,723,243 | 17,983,364 | 837,567,829 | 1,870 |
| subagent | 1,190,714 | 8,342,095 | 64,046,019 | 2,121,074,321 | 13,924 |
| workflow | 2,625,315 | 3,172,178 | 17,487,796 | 96,006,868 | 6,686 |
| combined | 4,130,360 | 13,237,516 | 99,517,179 | 3,054,649,018 | 22,480 |

### Per model x scope

| model | scope | input | output | cache_creation | cache_read | turns |
|---|---|---|---|---|---|---|
| <synthetic> | main | 0 | 0 | 0 | 0 | 22 |
| <synthetic> | subagent | 0 | 0 | 0 | 0 | 20 |
| <synthetic> | workflow | 0 | 0 | 0 | 0 | 70 |
| <synthetic> | combined | 0 | 0 | 0 | 0 | 112 |
| claude-fable-5 | main | 164,390 | 655,657 | 9,049,632 | 402,957,607 | 805 |
| claude-fable-5 | subagent | 815,934 | 6,244,002 | 30,388,357 | 1,700,810,451 | 10,082 |
| claude-fable-5 | workflow | 1,903,701 | 2,393,741 | 10,638,274 | 48,159,896 | 4,303 |
| claude-fable-5 | combined | 2,884,025 | 9,293,400 | 50,076,263 | 2,151,927,954 | 15,190 |
| claude-fable-5-1 | main | 45,437 | 564,437 | 3,586,631 | 292,623,122 | 628 |
| claude-fable-5-1 | subagent | 172,926 | 923,927 | 28,187,312 | 168,631,140 | 1,109 |
| claude-fable-5-1 | workflow | 107,402 | 366,970 | 3,995,481 | 36,858,609 | 1,185 |
| claude-fable-5-1 | combined | 325,765 | 1,855,334 | 35,769,424 | 498,112,871 | 2,922 |
| claude-haiku-4-5-20251001 | subagent | 564 | 43,756 | 253,218 | 3,930,411 | 91 |
| claude-haiku-4-5-20251001 | combined | 564 | 43,756 | 253,218 | 3,930,411 | 91 |
| claude-opus-4-8 | main | 104,504 | 503,149 | 5,347,101 | 141,987,100 | 415 |
| claude-opus-4-8 | subagent | 201,230 | 1,126,628 | 4,817,450 | 245,945,463 | 2,592 |
| claude-opus-4-8 | workflow | 614,212 | 411,467 | 2,854,041 | 10,988,363 | 1,128 |
| claude-opus-4-8 | combined | 919,946 | 2,041,244 | 13,018,592 | 398,920,926 | 4,135 |
| claude-opus-5 | subagent | 60 | 3,782 | 399,682 | 1,756,856 | 30 |
| claude-opus-5 | combined | 60 | 3,782 | 399,682 | 1,756,856 | 30 |

De-dup: 22,480 distinct assistant message ids; 20,910 duplicate records skipped (10692 streamed chunks where output_tokens grew vs the first chunk, so max-per-field was kept; 0 decreases; 0 cross-file). turn_duration system records: 446; API error records: 109.

## 2. Sessions

| session | project dir | bytes | first | last | turns | human prompts | in+cache tokens | output | titles |
|---|---|---|---|---|---|---|---|---|---|
| session-1 | <project> | 267 | - | - | 0 | 0 | 0 | 0 |  |
| session-2 | <project> | 149 | - | - | 0 | 0 | 0 | 0 |  |
| session-4 | <project> | 82,748,680 | 2026-08-11T03:15:08.663Z | 2026-09-03T19:04:41.247Z | 1,867 | 221 | 855,779,919 | 1,720,746 | Explore libultraship and 3DS targeting |
| session-5 | <project-2> | 7,950 | 2026-08-20T02:51:06.850Z | 2026-08-20T02:51:34.956Z | 0 | 0 | 0 | 0 |  |
| session-3 | <project> | 96,279 | 2026-08-23T20:49:35.890Z | 2026-08-23T20:51:35.059Z | 3 | 2 | 85,605 | 2,497 | Explain 3DS port impressiveness to friend |

### Tokens per day (UTC, all scopes)

| day | input | output | cache_creation | cache_read | turns | turns main/sub/wf | human prompts | agents sub/wf started | cost A | cost B |
|---|---|---|---|---|---|---|---|---|---|---|
| 2026-08-11 | 322,498 | 489,513 | 2,588,775 | 17,244,567 | 794 | 74/59/661 | 6 | 3/103 | $74.91 | $93.73 |
| 2026-08-12 | 48 | 3,239 | 301,280 | 31,852 | 2 | 2/0/0 | 0 | 0/0 | $3.96 | $6.22 |
| 2026-08-13 | 147,848 | 1,130,543 | 5,793,917 | 220,041,357 | 1,471 | 127/1344/0 | 21 | 16/0 | $347.04 | $390.06 |
| 2026-08-14 | 890,244 | 2,249,336 | 13,269,424 | 733,860,487 | 5,144 | 147/3713/1284 | 38 | 18/205 | $997.45 | $1,091.19 |
| 2026-08-15 | 305,890 | 211,736 | 2,889,259 | 10,942,025 | 607 | 14/0/593 | 3 | 0/103 | $30.35 | $41.19 |
| 2026-08-20 | 245,648 | 1,444,641 | 5,967,975 | 333,120,968 | 2,741 | 316/2425/0 | 15 | 33/0 | $241.20 | $263.58 |
| 2026-08-21 | 1,195,510 | 3,302,928 | 13,940,011 | 568,680,157 | 4,724 | 350/2355/2019 | 46 | 30/298 | $890.05 | $988.61 |
| 2026-08-22 | 21,845 | 173,665 | 1,498,385 | 57,423,274 | 334 | 23/311/0 | 4 | 2/0 | $80.56 | $89.88 |
| 2026-08-23 | 4,922 | 2,497 | 15,971 | 64,712 | 3 | 3/0/0 | 2 | 0/0 | $0.22 | $0.28 |
| 2026-08-27 | 610,802 | 1,797,508 | 13,223,887 | 405,528,301 | 2,834 | 110/1780/944 | 20 | 11/195 | $666.81 | $765.99 |
| 2026-08-28 | 45,335 | 535,073 | 3,726,272 | 205,613,018 | 837 | 59/778/0 | 11 | 5/0 | $274.24 | $300.39 |
| 2026-08-31 | 13,720 | 228 | 25,838 | 0 | 2 | 2/0/0 | 3 | 0/0 | $0.24 | $0.33 |
| 2026-09-01 | 86,305 | 432,375 | 6,200,098 | 49,223,305 | 796 | 69/180/547 | 6 | 4/99 | $112.29 | $158.79 |
| 2026-09-02 | 157,979 | 948,007 | 20,156,053 | 181,184,165 | 1,465 | 194/633/638 | 24 | 12/100 | $344.71 | $495.52 |
| 2026-09-03 | 81,766 | 516,227 | 9,920,034 | 271,690,830 | 726 | 380/346/0 | 24 | 13/0 | $215.89 | $288.50 |

## 3. Human prompts

- Strict (typed/queued by the user, excluding meta, task-notifications, local-command echoes, compact summaries): **223**
- Structural (user record with plain string or text item and no tool_result), excluding `isMeta`: 495; including meta: 578
- Total characters typed by the user (strict set): **19,751**
- By content kind: {'str': 200, 'text': 23}; by promptSource: {'typed': 207, 'None': 9, 'queued': 7}

| day | human prompts |
|---|---|
| 2026-08-11 | 6 |
| 2026-08-13 | 21 |
| 2026-08-14 | 38 |
| 2026-08-15 | 3 |
| 2026-08-20 | 15 |
| 2026-08-21 | 46 |
| 2026-08-22 | 4 |
| 2026-08-23 | 2 |
| 2026-08-27 | 20 |
| 2026-08-28 | 11 |
| 2026-08-31 | 3 |
| 2026-09-01 | 6 |
| 2026-09-02 | 24 |
| 2026-09-03 | 24 |

## 4. Tool use counts

| tool | main | subagents | workflow agents | total |
|---|---|---|---|---|
| Bash | 956 | 11717 | 2438 | 15111 |
| Read | 138 | 1974 | 80 | 2192 |
| WebFetch | 2 | 38 | 1869 | 1909 |
| Edit | 59 | 1223 | 0 | 1282 |
| ToolSearch | 14 | 67 | 988 | 1069 |
| StructuredOutput | 0 | 0 | 1039 | 1039 |
| WebSearch | 1 | 11 | 888 | 900 |
| Write | 56 | 147 | 0 | 203 |
| Agent | 130 | 17 | 0 | 147 |
| Monitor | 13 | 103 | 2 | 118 |
| TaskUpdate | 101 | 0 | 0 | 101 |
| TaskStop | 9 | 47 | 3 | 59 |
| TaskCreate | 39 | 0 | 0 | 39 |
| SendMessage | 21 | 0 | 0 | 21 |
| Workflow | 12 | 0 | 0 | 12 |
| AskUserQuestion | 5 | 0 | 0 | 5 |
| Skill | 3 | 2 | 0 | 5 |
| Artifact | 2 | 0 | 0 | 2 |
| ListAgents | 1 | 0 | 0 | 1 |
| EnterWorktree | 0 | 1 | 0 | 1 |

## 5. Subagents and workflows

- Subagent transcripts (`subagents/agent-*.jsonl`): **147** (147 matched to a parent `Agent` launch by agentId)
- Workflow runs (`wf_*` dirs): **10**, containing **1103** agent transcripts
- Launches recorded in the parent with no transcript on disk: 0
- Subagent types: {'Explore': 7, 'general-purpose': 138, 'caveman:cavecrew-reviewer': 2}; launched from: {'main': 130, 'subagent': 17}

| workflow | name | launches (incl. resumes) | agent transcripts | journal started | journal results |
|---|---|---|---|---|---|
| wf_00951cbd-e2e | deep-research | 1 | 103 | 103 | 95 |
| wf_162be8db-033 | deep-research | 1 | 100 | 100 | 100 |
| wf_2fef2dd1-2b2 | deep-research | 1 | 99 | 99 | 99 |
| wf_6e3fb5f9-cc8 | deep-research | 1 | 103 | 103 | 102 |
| wf_7c2ff94f-da9 | deep-research | 1 | 102 | 102 | 102 |
| wf_808466a2-e9f | deep-research | 1 | 102 | 102 | 102 |
| wf_910a75c7-57d | deep-research | 1 | 99 | 99 | 99 |
| wf_b4dbb5ed-1e4 | deep-research | 3 | 195 | 195 | 127 |
| wf_dd769a75-2c4 | deep-research | 1 | 97 | 97 | 97 |
| wf_f462bfdf-b71 | deep-research | 1 | 103 | 103 | 103 |

### Subagents started per day (UTC)

| day | subagents | workflow agents |
|---|---|---|
| 2026-08-11 | 3 | 103 |
| 2026-08-13 | 16 | 0 |
| 2026-08-14 | 18 | 205 |
| 2026-08-15 | 0 | 103 |
| 2026-08-20 | 33 | 0 |
| 2026-08-21 | 30 | 298 |
| 2026-08-22 | 2 | 0 |
| 2026-08-27 | 11 | 195 |
| 2026-08-28 | 5 | 0 |
| 2026-09-01 | 4 | 99 |
| 2026-09-02 | 12 | 100 |
| 2026-09-03 | 13 | 0 |

### Per subagent (Agent-tool subagents)

| agent | first ts | model | type | description | turns | input | output | cache_cr | cache_rd | cost A | cost B |
|---|---|---|---|---|---|---|---|---|---|---|---|
| aa860659ff9f134f7 | 2026-08-11T03:17 | claude-haiku-4-5-20251001 | Explore | Map repo for 3DS port facts | 21 | 143 | 10,709 | 87,179 | 1,037,790 | $0.27 | $0.33 |
| a7d0c28a31987c678 | 2026-08-11T03:48 | claude-fable-5 | general-purpose | Review 3DS port plan | 6 | 4,037 | 13,949 | 41,888 | 129,563 | $1.39 | $1.71 |
| acc2d787155af60e8 | 2026-08-11T13:43 | claude-fable-5 | general-purpose | LUS carve compile spike | 32 | 4,052 | 42,582 | 185,575 | 1,834,421 | $6.32 | $7.72 |
| a0b50d842f2627f06 | 2026-08-13T01:10 | claude-fable-5 | general-purpose | Stream A: citro3d renderer | 64 | 7,292 | 93,564 | 286,557 | 6,941,746 | $15.27 | $17.42 |
| a845f5c4a64a50e4c | 2026-08-13T01:10 | claude-fable-5 | general-purpose | Stream B: libctru os layer | 25 | 4,610 | 31,967 | 67,868 | 1,220,657 | $3.71 | $4.22 |
| a436230b52b5a57ae | 2026-08-13T01:10 | claude-fable-5 | general-purpose | Stream C: ndsp audio backend | 22 | 4,873 | 38,078 | 81,008 | 1,137,816 | $4.10 | $4.71 |
| ae711900f85ddaa36 | 2026-08-13T01:11 | claude-fable-5 | general-purpose | Stream D: assets + prebake | 52 | 17,215 | 38,746 | 131,140 | 4,196,969 | $7.95 | $8.93 |
| ad143cff9f55b8c23 | 2026-08-13T01:11 | claude-fable-5 | general-purpose | Stream E: 32-bit sweep | 129 | 9,285 | 91,391 | 238,431 | 18,482,859 | $26.13 | $27.91 |
| a02f8f48a3ee8b0df | 2026-08-13T01:11 | claude-fable-5 | general-purpose | Stream F: combiner+memory census | 34 | 5,768 | 40,050 | 171,549 | 2,283,320 | $6.49 | $7.77 |
| aa0f565e69882ef8f | 2026-08-13T01:15 | claude-haiku-4-5-20251001 | Explore | Gather memory budget facts | 55 | 303 | 7,569 | 63,455 | 2,641,401 | $0.38 | $0.43 |
| aec3e9abedec5ffb4 | 2026-08-13T04:45 | claude-fable-5 | general-purpose | M1 core: decomp compile + link | 198 | 10,662 | 157,303 | 741,961 | 53,331,400 | $70.58 | $76.14 |
| aee25a4d1915e79f0 | 2026-08-13T04:45 | claude-fable-5 | general-purpose | Harness: synthetic DL replay tests | 93 | 4,217 | 94,983 | 228,103 | 12,650,660 | $20.29 | $22.00 |
| a661ac13fee6e04d9 | 2026-08-13T05:21 | claude-fable-5 | general-purpose | Stream A shift 2: fixes + census | 76 | 9,241 | 94,167 | 284,141 | 11,324,632 | $19.68 | $21.81 |
| affae5d4bd187cd54 | 2026-08-13T05:47 | claude-fable-5 | general-purpose | M1 debug: black-screen boot hang | 136 | 15,478 | 60,432 | 164,238 | 13,134,660 | $18.36 | $19.60 |
| ab263319124c74878 | 2026-08-13T06:09 | <synthetic>,claude-fable-5 | general-purpose | M1 render: first pixels | 139 | 8,307 | 102,346 | 606,616 | 19,037,675 | $31.82 | $36.37 |
| a956cfe011ba950b0 | 2026-08-13T12:38 | claude-fable-5 | general-purpose | M1 assets: fix empty archive index | 61 | 7,066 | 30,463 | 133,503 | 3,250,707 | $6.51 | $7.51 |
| a5c7dc9140ef09044 | 2026-08-13T12:56 | <synthetic>,claude-fable-5 | general-purpose | M1 present: pixels to screen | 212 | 15,014 | 131,839 | 1,293,071 | 34,872,285 | $57.78 | $67.48 |
| a6dec786e71a23000 | 2026-08-13T23:28 | claude-fable-5 | general-purpose | M1 audio: kill cxd4 crash | 46 | 4,080 | 23,022 | 85,145 | 2,354,507 | $4.61 | $5.25 |
| ae2622f11dea49862 | 2026-08-13T23:59 | claude-fable-5 | general-purpose | M1 race freeze fix | 155 | 25,602 | 106,251 | 408,437 | 26,479,199 | $37.15 | $40.22 |
| ae31a474bbe3a7d06 | 2026-08-14T00:37 | claude-fable-5 | general-purpose | M1 memory: segment cache eviction | 131 | 17,184 | 86,324 | 361,218 | 19,774,990 | $28.78 | $31.49 |
| aa98792bbf4812602 | 2026-08-14T01:13 | claude-fable-5 | general-purpose | V: race visual fixes | 139 | 11,329 | 146,195 | 1,139,239 | 28,864,331 | $50.53 | $59.07 |
| a71699552072a1e7a | 2026-08-14T01:13 | claude-fable-5 | general-purpose | B: bridge DL drops | 94 | 4,176 | 70,331 | 418,365 | 12,570,293 | $21.36 | $24.50 |
| a337b7b4ed9e7fe79 | 2026-08-14T01:14 | claude-fable-5 | general-purpose | P: perf baseline + non-gfx wins | 48 | 26,184 | 29,660 | 314,842 | 3,572,596 | $9.25 | $11.61 |
| af6bbbe683966bef2 | 2026-08-14T02:57 | claude-fable-5 | general-purpose | T: texture upload cache | 218 | 32,702 | 113,078 | 764,115 | 35,997,118 | $51.53 | $57.26 |
| a4a5c23090f5d7509 | 2026-08-14T04:07 | claude-fable-5 | general-purpose | B2: finish bridge DL drops | 229 | 12,607 | 104,959 | 361,258 | 40,219,032 | $50.11 | $52.82 |
| a69fa66a7e31e6b3f | 2026-08-14T04:07 | claude-fable-5 | general-purpose | P2: pacer, audio core, malloc | 42 | 4,115 | 20,959 | 144,934 | 2,009,504 | $4.91 | $6.00 |
| a2483cd76f1dce006 | 2026-08-14T04:28 | claude-fable-5 | general-purpose | H: hardware prep + cia | 45 | 7,008 | 41,381 | 155,507 | 2,759,414 | $6.84 | $8.01 |
| a985bb2cd21e75c07 | 2026-08-14T04:28 | claude-fable-5 | general-purpose | R: patch consolidation review | 24 | 4,079 | 32,062 | 73,308 | 1,103,217 | $3.66 | $4.21 |
| adc98141411e439b9 | 2026-08-14T04:28 | claude-fable-5 | general-purpose | D: local CI + user docs | 46 | 8,696 | 41,172 | 121,296 | 3,909,460 | $7.57 | $8.48 |
| a574b70ac8e737294 | 2026-08-14T04:49 | <synthetic>,claude-fable-5 | general-purpose | C2: invisible track cull bug | 239 | 16,533 | 122,167 | 895,348 | 42,544,130 | $60.01 | $66.72 |
| a88a7cddc07ece6be | 2026-08-14T05:09 | claude-fable-5 | general-purpose | K: 60fps campaign plan | 13 | 4,057 | 22,726 | 111,173 | 765,394 | $3.33 | $4.17 |
| ae22071439a0da850 | 2026-08-14T05:09 | <synthetic>,claude-fable-5 | general-purpose | G: GPU profiling telemetry | 933 | 32,914 | 115,761 | 411,019 | 195,740,425 | $207.00 | $210.08 |
| aae016fabbf29df4d | 2026-08-14T05:09 | <synthetic>,claude-fable-5 | general-purpose | S: stereo foundation | 928 | 10,522 | 123,186 | 562,962 | 191,161,191 | $204.46 | $208.69 |
| a712ed6c8bf1b66f4 | 2026-08-14T12:20 | <synthetic>,claude-fable-5 | general-purpose | C3: salvage + finish track cull | 214 | 16,222 | 154,302 | 985,077 | 32,661,911 | $52.85 | $60.24 |
| a883b757133faba96 | 2026-08-14T12:20 | <synthetic>,claude-fable-5,claude-opus-4-8 | general-purpose | F2: verify + close G and S work | 92 | 16,609 | 53,011 | 693,351 | 7,873,493 | $16.23 | $20.57 |
| aa139649c40733678 | 2026-08-14T12:23 | claude-haiku-4-5-20251001 | Explore | Cross-check STEREO.md vs research | 2 | 10 | 2,564 | 18,469 | 8,347 | $0.04 | $0.05 |
| abc49ec848475ef65 | 2026-08-14T13:43 | <synthetic>,claude-fable-5,claude-opus-4-8 | general-purpose | C4: land the fog/track fix | 123 | 17,141 | 81,301 | 649,621 | 11,815,650 | $17.75 | $21.20 |
| ac5fb13fffba12846 | 2026-08-20T05:40 | claude-opus-4-8 | general-purpose | Fog per-vertex combiner fix | 106 | 7,308 | 40,830 | 285,553 | 11,199,583 | $8.44 | $9.51 |
| abdc8659f5e46d3be | 2026-08-20T05:40 | claude-opus-4-8 | general-purpose | Merge gpuprof + add telemetry cols | 58 | 4,147 | 20,580 | 66,820 | 2,508,111 | $2.21 | $2.46 |
| ae8d0839107fabb3d | 2026-08-20T05:41 | claude-opus-4-8 | general-purpose | Merge stereo foundation | 38 | 4,107 | 12,968 | 57,699 | 1,336,627 | $1.37 | $1.59 |
| ae6522caffa3a3ad9 | 2026-08-20T05:41 | claude-opus-4-8 | general-purpose | Perf: pacer, audio core2, malloc | 60 | 4,430 | 27,321 | 104,093 | 4,273,618 | $3.49 | $3.88 |
| aa45b37054dfe96fc | 2026-08-20T05:42 | claude-opus-4-8 | general-purpose | Tunnel-roof backface cull fix | 36 | 4,107 | 15,823 | 38,058 | 974,068 | $1.14 | $1.28 |
| a4f1f742d2b4df593 | 2026-08-20T05:47 | claude-opus-4-8 | general-purpose | PICA200 combiner fog input research | 13 | 6,209 | 9,974 | 35,825 | 259,034 | $0.63 | $0.77 |
| a14be77e8fc7f5229 | 2026-08-20T06:06 | claude-opus-4-8 | general-purpose | Integrate 4 branches into m1-next | 93 | 7,054 | 32,662 | 92,018 | 5,108,970 | $3.98 | $4.33 |
| a96a6ae53926ad914 | 2026-08-20T06:27 | claude-opus-4-8 | general-purpose | Measurement pass on m1 | 20 | 9,512 | 8,531 | 105,164 | 852,276 | $1.34 | $1.74 |
| a8043aae022568638 | 2026-08-20T06:27 | claude-opus-4-8 | general-purpose | Tunnel-roof near-plane clip | 97 | 9,614 | 41,311 | 110,337 | 6,490,050 | $5.02 | $5.43 |
| a0874b13ebcd51e98 | 2026-08-20T06:28 | claude-opus-4-8 | general-purpose | Fix ci-3ds.sh patch list | 41 | 4,113 | 24,012 | 55,161 | 1,719,962 | $1.83 | $2.03 |
| a7c849d409e5c0102 | 2026-08-20T06:40 | claude-opus-4-8 | general-purpose | Black sky/backdrop fog fix | 89 | 5,349 | 39,028 | 109,635 | 6,190,294 | $4.78 | $5.19 |
| ab912306285b8f1ce | 2026-08-20T06:41 | claude-opus-4-8 | general-purpose | HUD garbage textures fix | 81 | 4,193 | 32,105 | 106,042 | 5,266,579 | $4.12 | $4.52 |
| af4fda908cd633c43 | 2026-08-20T06:41 | claude-opus-4-8 | general-purpose | Shadow + boost blend fix | 94 | 7,485 | 42,447 | 176,730 | 6,869,370 | $5.64 | $6.30 |
| a47dd0fe281694516 | 2026-08-20T06:43 | claude-opus-4-8 | general-purpose | Locate shadow and boost draws | 24 | 4,690 | 12,400 | 64,157 | 994,651 | $1.23 | $1.47 |
| a9a3fe5aaf37183b4 | 2026-08-20T07:05 | claude-opus-4-8 | general-purpose | HUD garbage deep root-cause | 106 | 4,243 | 53,274 | 163,383 | 10,488,984 | $7.62 | $8.23 |
| abfb2041dc3cbb57a | 2026-08-20T07:06 | claude-opus-4-8 | general-purpose | Night emulator verify + sky fix | 491 | 12,708 | 188,504 | 736,015 | 100,284,569 | $59.52 | $62.28 |
| aaeb24f261e14c210 | 2026-08-20T09:19 | claude-opus-4-8 | general-purpose | km/h speedometer format decode | 91 | 4,494 | 42,418 | 123,393 | 6,793,463 | $5.25 | $5.71 |
| a828bc3ca92ce8c5d | 2026-08-20T09:22 | claude-opus-4-8 | general-purpose | CPU-vs-GPU measurement pass | 27 | 4,438 | 15,810 | 80,414 | 980,257 | $1.41 | $1.71 |
| a7a9e998d3582199b | 2026-08-20T12:42 | claude-opus-4-8 | general-purpose | Machine shadow wrong shape | 80 | 4,467 | 37,665 | 166,444 | 5,753,152 | $4.88 | $5.50 |
| a665bd8db7145def1 | 2026-08-20T12:43 | claude-opus-4-8 | general-purpose | Building distance texture pop-in | 77 | 4,472 | 32,379 | 148,977 | 5,073,900 | $4.30 | $4.86 |
| afc0c3f23f2e5bd2e | 2026-08-20T12:51 | claude-opus-4-8 | general-purpose | Find Mute City building LOD in decomp | 18 | 4,108 | 4,528 | 37,240 | 464,095 | $0.60 | $0.74 |
| a650d4be49234e6f4 | 2026-08-20T12:57 | claude-opus-4-8 | general-purpose | Decode building DL combiner mode | 14 | 4,443 | 6,100 | 23,893 | 252,319 | $0.45 | $0.54 |
| ab87da40f6656c696 | 2026-08-20T13:17 | claude-opus-4-8 | general-purpose | Fix shadow UV + km/h downstream | 112 | 6,230 | 63,666 | 240,549 | 11,925,675 | $9.09 | $9.99 |
| a47638ab6aa053c60 | 2026-08-20T13:18 | <synthetic>,claude-opus-4-8 | general-purpose | Fix building distance pop-in | 60 | 4,715 | 25,958 | 118,410 | 4,914,735 | $3.87 | $4.31 |
| a2d22ce2609e37d2b | 2026-08-20T13:19 | claude-opus-4-8 | general-purpose | Fix sky black wedge (design) | 68 | 4,167 | 24,437 | 90,196 | 4,234,973 | $3.31 | $3.65 |
| abbdb2086b7ff3115 | 2026-08-20T13:22 | claude-opus-4-8 | general-purpose | Trace shadow geometry in racer.c | 22 | 4,116 | 7,386 | 32,208 | 596,127 | $0.70 | $0.83 |
| a1254c5815e73fb67 | 2026-08-20T13:38 | claude-opus-4-8 | general-purpose | Fix building pop-in (respawn) | 57 | 4,145 | 27,592 | 110,770 | 4,590,912 | $3.70 | $4.11 |
| ac889498e29cc2a4f | 2026-08-20T13:45 | claude-opus-4-8 | general-purpose | Trace HUD speedometer draw path | 30 | 4,132 | 11,057 | 31,680 | 690,683 | $0.84 | $0.96 |
| a4cbbcfbd509b46f6 | 2026-08-20T14:53 | claude-opus-4-8 | general-purpose | Shadow shape geometry angle | 91 | 4,213 | 36,033 | 115,018 | 6,734,047 | $5.01 | $5.44 |
| a9037963d1cddf097 | 2026-08-20T22:11 | claude-opus-4-8 | general-purpose | Texture ground-truth dump diagnostic | 51 | 4,410 | 22,294 | 85,919 | 2,625,818 | $2.43 | $2.75 |
| ae4473e274dcb0c81 | 2026-08-20T22:27 | claude-opus-4-8 | general-purpose | Fix RGBA16 km/h decode | 60 | 4,151 | 28,550 | 95,162 | 3,718,025 | $3.19 | $3.55 |
| ad3fcde7dbd6803e8 | 2026-08-20T22:46 | claude-opus-4-8 | general-purpose | Gate RGBA16 word-swap by load interleave | 81 | 8,446 | 41,244 | 146,474 | 6,523,407 | $5.25 | $5.80 |
| a731d1902d04f23ab | 2026-08-20T23:07 | claude-opus-4-8 | general-purpose | Sky wedge fill v2 (viewport-scoped) | 39 | 4,109 | 19,357 | 63,301 | 1,566,934 | $1.68 | $1.92 |
| a44b9a8291436675d | 2026-08-21T00:27 | claude-opus-4-8 | general-purpose | Desktop-vs-3DS decode + scanout oracle | 20 | 4,272 | 17,827 | 68,181 | 749,045 | $1.27 | $1.52 |
| ac3fb9a86dff3bc6e | 2026-08-21T00:29 | claude-opus-4-8 | general-purpose | Map RGBA16/I4 decode paths | 11 | 4,094 | 8,136 | 39,916 | 294,681 | $0.62 | $0.77 |
| aa458c63595a56e3f | 2026-08-21T00:35 | claude-opus-4-8 | general-purpose | Fix scanout oracle + no_tmem gate | 49 | 4,129 | 22,839 | 84,220 | 2,741,137 | $2.49 | $2.80 |
| aadb962b640b16367 | 2026-08-21T01:18 | claude-fable-5 | general-purpose | Fix segment-3 image sourcing | 77 | 14,354 | 64,859 | 389,650 | 8,957,191 | $17.21 | $20.14 |
| a8a331dd7e00c1d8b | 2026-08-21T01:42 | claude-fable-5 | general-purpose | Find real race speedo draw path | 64 | 14,037 | 61,680 | 168,005 | 6,245,693 | $11.57 | $12.83 |
| a866317dfbdcbe5c3 | 2026-08-21T02:07 | claude-fable-5 | general-purpose | Fix COPY-mode texrect garble | 124 | 4,563 | 108,594 | 311,961 | 21,706,209 | $31.08 | $33.42 |
| a0b8d8cd7e71e6828 | 2026-08-21T02:48 | claude-fable-5 | general-purpose | Ship body texture wrong | 124 | 4,279 | 80,789 | 239,986 | 14,495,424 | $21.58 | $23.38 |
| a3547dcc500fe1da9 | 2026-08-21T03:25 | claude-fable-5 | general-purpose | Ship multi-tile binding fix | 74 | 4,179 | 60,120 | 154,789 | 6,007,133 | $10.99 | $12.15 |
| a7c9f91db8dd5b394 | 2026-08-21T03:47 | claude-fable-5 | general-purpose | Menu performance sprint | 102 | 6,344 | 65,033 | 195,899 | 11,984,327 | $17.75 | $19.22 |
| af3d15349c5f5457d | 2026-08-21T03:49 | claude-fable-5 | general-purpose | S7 interpreter CPU reduction | 147 | 8,930 | 133,251 | 305,306 | 25,627,672 | $36.20 | $38.49 |
| a032b9437389a1796 | 2026-08-21T03:49 | claude-fable-5 | general-purpose | Rebase sky fill onto new m1 | 22 | 4,075 | 8,203 | 26,556 | 445,339 | $1.23 | $1.43 |
| a7a1c5fcf21a80dfe | 2026-08-21T04:41 | claude-fable-5 | general-purpose | Select-screen perf + import memo | 89 | 7,665 | 62,308 | 179,390 | 9,687,393 | $15.12 | $16.47 |
| a6e8408776f7514c7 | 2026-08-21T05:07 | claude-fable-5 | general-purpose | Menu task-cadence root cause | 109 | 4,810 | 94,069 | 259,607 | 15,584,913 | $23.58 | $25.53 |
| a579c2bc1a73c5730 | 2026-08-21T05:08 | claude-fable-5 | general-purpose | S7 re-bisect vs current base | 62 | 5,583 | 56,692 | 321,268 | 6,490,704 | $13.40 | $15.81 |
| aed47a39b8060f6eb | 2026-08-21T05:08 | claude-fable-5 | general-purpose | Ship decal interleave tripwire | 60 | 4,432 | 54,016 | 136,705 | 5,187,698 | $9.64 | $10.67 |
| a0e7bd211bf0cb4f1 | 2026-08-21T05:29 | claude-fable-5 | general-purpose | Stale prim-color constant hunt | 73 | 5,194 | 62,202 | 174,851 | 7,799,921 | $13.15 | $14.46 |
| a99e4203761c7e2d2 | 2026-08-21T13:04 | claude-fable-5 | general-purpose | Profile WHERE build ms goes | 57 | 9,590 | 51,858 | 153,965 | 5,406,688 | $10.02 | $11.17 |
| a015a56cc71941bdc | 2026-08-21T13:19 | claude-fable-5 | general-purpose | Flat dispatch table fix | 65 | 4,438 | 47,267 | 91,024 | 3,900,134 | $7.45 | $8.13 |
| a18233f74ef1ca7b9 | 2026-08-21T13:38 | claude-fable-5 | general-purpose | LoadBlock handler optimization | 88 | 14,323 | 107,684 | 213,402 | 10,950,177 | $19.15 | $20.75 |
| a867245741e5bc15c | 2026-08-21T14:11 | claude-fable-5 | general-purpose | Texrect state fast path | 72 | 4,747 | 70,031 | 173,716 | 8,097,143 | $13.82 | $15.12 |
| afa76d445e8b4fbdd | 2026-08-21T15:55 | claude-fable-5 | general-purpose | Fix silent audio end-to-end | 58 | 8,988 | 53,043 | 211,918 | 4,767,530 | $10.16 | $11.75 |
| acfbbe5fede7bdecf | 2026-08-21T15:56 | claude-fable-5 | general-purpose | Bottom-screen FPS counter | 48 | 4,127 | 36,875 | 93,732 | 3,064,319 | $6.12 | $6.82 |
| ac260b8537281178d | 2026-08-21T16:56 | claude-fable-5 | general-purpose | Why audio synthesis outputs zeros | 228 | 27,032 | 142,891 | 388,667 | 52,779,842 | $65.05 | $67.97 |
| aeed0bd66893f44be | 2026-08-21T17:54 | claude-fable-5 | general-purpose | 3DS input remap: L drift + ZL/ZR boost | 57 | 4,427 | 25,601 | 73,075 | 2,681,092 | $4.92 | $5.47 |
| a24b51551fd3cc65a | 2026-08-21T18:40 | claude-fable-5 | general-purpose | HW DSP consumes but silent | 15 | 4,061 | 21,214 | 55,459 | 577,252 | $2.37 | $2.79 |
| a5fc91f1bf527be26 | 2026-08-21T19:02 | claude-fable-5 | general-purpose | HW-only zero synthesis hunt | 85 | 8,912 | 84,924 | 202,052 | 11,560,092 | $18.42 | $19.94 |
| a9c48037d96b6921e | 2026-08-21T20:44 | claude-fable-5 | general-purpose | Forensic bisect: emu music regression | 82 | 12,454 | 96,713 | 703,136 | 9,046,018 | $22.80 | $28.07 |
| ad42e98c45fb29a34 | 2026-08-21T21:23 | claude-fable-5 | general-purpose | Audio load hitches + hex spam | 132 | 4,295 | 100,717 | 462,607 | 18,669,191 | $29.53 | $33.00 |
| a4679ca433563e123 | 2026-08-21T22:50 | claude-fable-5 | general-purpose | Ship decal sequencing fix v2 | 157 | 4,973 | 101,229 | 241,798 | 20,209,730 | $28.34 | $30.16 |
| a053fbd32553fb87b | 2026-08-21T23:59 | claude-fable-5 | general-purpose | Race-start hitch + spam + gputrace latch | 204 | 7,463 | 81,635 | 661,884 | 36,121,112 | $48.55 | $53.52 |
| a588d8de339a8784f | 2026-08-22T01:18 | <synthetic>,claude-fable-5 | general-purpose | Traffic-tail grind: bridge + pipesync | 61 | 4,435 | 39,253 | 171,013 | 4,969,295 | $9.11 | $10.40 |
| a17ac8eb92eb2fd48 | 2026-08-22T01:18 | <synthetic>,claude-fable-5 | general-purpose | Sky wedge: cloud-texture theory | 50 | 4,418 | 38,096 | 153,514 | 4,475,180 | $8.34 | $9.49 |
| a371be21cce20e72b | 2026-08-27T06:21 | claude-fable-5 | general-purpose | Traffic-tail grind: bridge + pipesync | 200 | 10,699 | 154,508 | 1,017,872 | 43,926,983 | $64.48 | $72.12 |
| ae51139ab7e47a95b | 2026-08-27T06:22 | claude-fable-5 | general-purpose | Sky wedge: cloud-texture theory | 122 | 11,897 | 77,957 | 311,101 | 15,757,354 | $23.66 | $26.00 |
| a096f29e80e614d9c | 2026-08-27T06:25 | claude-fable-5 | general-purpose | Map ccmux 11 explosion combiner | 67 | 5,829 | 47,792 | 117,770 | 4,581,668 | $8.50 | $9.38 |
| a7f3842b785aa7995 | 2026-08-27T06:25 | <synthetic>,claude-fable-5 | general-purpose | Shadow shape root cause | 47 | 4,397 | 18,651 | 73,773 | 2,048,472 | $3.95 | $4.50 |
| a945178b41fd2bd2a | 2026-08-27T06:25 | claude-fable-5 | general-purpose | Stereo 3D per-eye projection | 42 | 4,115 | 40,805 | 76,140 | 2,083,361 | $5.12 | $5.69 |
| ac9c076c47c71da01 | 2026-08-27T06:34 | claude-fable-5 | general-purpose | Shadow shape root cause (respawn) | 81 | 4,826 | 69,790 | 181,550 | 9,348,582 | $15.16 | $16.52 |
| acc9ff2491f66d1ef | 2026-08-27T16:01 | claude-fable-5 | general-purpose | Fix filelog OOM + leak audit | 264 | 19,228 | 145,180 | 856,453 | 49,540,186 | $67.70 | $74.12 |
| a99036f777a03a5f6 | 2026-08-27T16:02 | claude-fable-5 | general-purpose | Race-exit transition glitch+slow | 253 | 10,161 | 202,286 | 473,550 | 70,528,660 | $86.66 | $90.22 |
| af34f914564f64847 | 2026-08-27T18:07 | claude-fable-5 | general-purpose | Bottom-screen touch menu v1 | 208 | 9,494 | 192,907 | 964,751 | 51,119,303 | $72.92 | $80.15 |
| a9c77db382ffa9c33 | 2026-08-27T19:22 | claude-fable-5 | general-purpose | Crowd grind round 2 | 217 | 10,994 | 120,622 | 1,974,404 | 29,542,005 | $60.36 | $75.17 |
| a13855ef6d7b5d2d7 | 2026-08-27T19:31 | claude-fable-5 | general-purpose | Time-trial init hang | 279 | 86,351 | 153,831 | 2,578,255 | 72,331,823 | $113.12 | $132.45 |
| a03eb3f8c63c5c14c | 2026-08-28T00:18 | claude-fable-5 | general-purpose | Rival detail perf option | 100 | 8,391 | 74,428 | 759,088 | 13,598,032 | $26.89 | $32.59 |
| a947fa2f9372c74f4 | 2026-08-28T00:19 | claude-fable-5 | general-purpose | Tri-loop copy-out micro-opt | 95 | 4,501 | 101,114 | 240,445 | 15,200,198 | $23.31 | $25.11 |
| ad11ef59a6fa57858 | 2026-08-28T04:28 | claude-fable-5 | general-purpose | GPU vertex transform moonshot | 450 | 10,361 | 192,565 | 655,950 | 133,031,083 | $150.96 | $155.88 |
| abf4882e3ec8a654e | 2026-08-28T04:36 | claude-fable-5 | general-purpose | APT power/home handling | 68 | 4,450 | 43,760 | 264,114 | 6,836,140 | $12.37 | $14.35 |
| ad22a3fde4d5385f7 | 2026-08-28T13:18 | <synthetic>,claude-fable-5 | general-purpose | Bridge translation cache | 65 | 4,445 | 76,833 | 224,737 | 7,742,296 | $14.44 | $16.12 |
| ac33f26579ee5fabc | 2026-09-01T21:33 | claude-fable-5-1 | general-purpose | Bridge translation cache lever | 149 | 22,703 | 148,482 | 5,973,833 | 23,926,871 | $88.31 | $133.11 |
| a9d00de2bedadf9a6 | 2026-09-01T21:37 | claude-fable-5-1 | general-purpose | Audit ProcessList global reads | 17 | 5,219 | 9,777 | 92,918 | 1,057,887 | $1.97 | $2.66 |
| aeea82e7782a0fee4 | 2026-09-01T21:37 | claude-fable-5-1 | general-purpose | Audit bridge resolver helpers' globals | 21 | 4,993 | 21,435 | 107,210 | 1,554,696 | $2.85 | $3.65 |
| ac89b8cb5498eb2c8 | 2026-09-01T23:56 | claude-fable-5-1 | general-purpose | Audit 3DS HOME suspend path | 11 | 3,435 | 7,219 | 189,823 | 1,148,453 | $3.06 | $4.48 |
| ac920fda2a30b6769 | 2026-09-02T00:15 | claude-fable-5-1 | general-purpose | Fix HOME-press crash on 3DS | 40 | 7,792 | 18,858 | 292,115 | 3,152,065 | $5.46 | $7.65 |
| a3cb6e399e37b33f4 | 2026-09-02T14:07 | claude-fable-5-1 | general-purpose | Fix close-from-HOME teardown hang | 4 | 2,011 | 2,705 | 50,687 | 97,487 | $0.81 | $1.19 |
| a7edeed110b582a04 | 2026-09-02T14:09 | claude-fable-5-1 | general-purpose | Fix close-from-HOME teardown hang | 23 | 3,660 | 25,962 | 103,796 | 1,617,725 | $3.04 | $3.82 |
| a93b39ec945678cd4 | 2026-09-02T14:51 | claude-fable-5-1 | general-purpose | Task A: texrect batching lever | 74 | 12,633 | 133,374 | 4,049,931 | 11,971,871 | $60.41 | $90.79 |
| a20b12ea197c6174c | 2026-09-02T14:51 | claude-fable-5-1 | general-purpose | Task B: TMEM load bookkeeping lever | 67 | 9,973 | 58,782 | 2,454,441 | 7,981,002 | $35.71 | $54.12 |
| ac3114180800d4983 | 2026-09-02T14:55 | claude-fable-5-1 | general-purpose | Task C: per-triangle cost lever | 108 | 8,430 | 80,891 | 2,128,186 | 21,163,634 | $36.02 | $51.98 |
| a610efc456ccb70ff | 2026-09-02T14:56 | claude-fable-5-1 | general-purpose | Task D: boot audio jitter fix | 43 | 4,837 | 33,769 | 704,613 | 3,513,087 | $11.42 | $16.71 |
| ac8911180e2571fdc | 2026-09-02T15:09 | claude-haiku-4-5-20251001 | caveman:cavecrew-reviewer | Review tmemfast teardown hunk | 8 | 66 | 22,643 | 41,139 | 156,192 | $0.18 | $0.21 |
| a1e55173c3d89b600 | 2026-09-02T18:20 | claude-fable-5-1 | general-purpose | Audit atlas x tri-memo interaction | 22 | 6,518 | 35,542 | 380,476 | 2,880,404 | $7.32 | $10.17 |
| ae09c31ee72b9aa86 | 2026-09-02T22:02 | claude-fable-5-1 | general-purpose | Task F: dispatch-remainder census + lever | 84 | 8,165 | 78,429 | 1,582,204 | 11,236,127 | $26.59 | $38.46 |
| af81715290189bf91 | 2026-09-02T22:02 | claude-fable-5-1 | general-purpose | Task G: boot audio second attempt | 72 | 8,997 | 43,791 | 1,593,873 | 8,200,880 | $24.25 | $36.21 |
| a61b459a3bbe22a71 | 2026-09-02T22:24 | <synthetic>,claude-fable-5-1 | general-purpose | Task H: render thread on core 2 | 115 | 20,883 | 67,509 | 3,353,989 | 33,973,660 | $54.00 | $79.16 |
| a937aae66b9a49d7e | 2026-09-03T01:50 | claude-fable-5-1 | general-purpose | Task I: balance cores (bridge on main) | 65 | 8,012 | 54,241 | 915,563 | 9,817,450 | $16.69 | $23.56 |
| a10bfb903f340580e | 2026-09-03T01:51 | claude-fable-5-1 | general-purpose | Task J: dynamic rival detail | 53 | 5,625 | 22,890 | 1,505,085 | 5,032,660 | $21.27 | $32.56 |
| ac1f2b361a4b1be22 | 2026-09-03T01:51 | claude-fable-5-1 | general-purpose | Task E: machine texture atlas | 93 | 8,077 | 31,992 | 1,756,904 | 15,057,083 | $27.41 | $40.58 |
| aab04ef90c9125f13 | 2026-09-03T02:13 | claude-haiku-4-5-20251001 | caveman:cavecrew-reviewer | Review bridge-on-main split diff | 5 | 42 | 271 | 42,976 | 86,681 | $0.06 | $0.10 |
| a94bf452b9e6a9ca3 | 2026-09-03T13:55 | <synthetic> | general-purpose | Finish texture-cache ownership fix | 2 | 0 | 0 | 0 | 0 | $0.00 | $0.00 |
| a5b20af281c31b6be | 2026-09-03T14:25 | <synthetic> | general-purpose | Task V: sub-viewport 3D placement bug | 1 | 0 | 0 | 0 | 0 | $0.00 | $0.00 |
| a7a0e598a24da02e2 | 2026-09-03T18:10 | claude-fable-5-1 | general-purpose | Token and cost census | 17 | 6,593 | 15,848 | 123,656 | 1,145,356 | $2.69 | $3.62 |
| a014695829344d30f | 2026-09-03T18:10 | claude-fable-5-1 | general-purpose | Project timeline from git and docs | 15 | 5,040 | 1,755 | 491,199 | 2,142,084 | $6.81 | $10.50 |
| a65c4d8f998e52da0 | 2026-09-03T18:10 | claude-fable-5-1 | general-purpose | Extract fps data for charts | 20 | 9,330 | 30,676 | 336,810 | 1,960,658 | $6.33 | $8.85 |
| aa363cb68f3829e46 | 2026-09-03T18:11 | claude-opus-5 | Explore | Extract perf numbers from progress docs A | 5 | 10 | 1,455 | 60,163 | 116,360 | $0.47 | $0.70 |
| a862056a877ac32b2 | 2026-09-03T18:11 | claude-opus-5 | Explore | Extract perf numbers from progress docs B | 8 | 16 | 840 | 88,269 | 275,250 | $0.71 | $1.04 |
| a0e9850d5ed70a4e9 | 2026-09-03T18:12 | claude-opus-5 | Explore | Summarize early 3DS research docs | 13 | 26 | 1,462 | 173,029 | 1,206,214 | $1.72 | $2.37 |
| a3dc11ebd5148c2be | 2026-09-03T18:12 | claude-opus-5 | Explore | Summarize Aug-20..Sep-2 perf docs | 4 | 8 | 25 | 78,221 | 159,032 | $0.57 | $0.86 |

### Workflow agents, aggregated per workflow run

| workflow | first ts | skill/name | models | agents | turns | input | output | cache_cr | cache_rd | cost A | cost B |
|---|---|---|---|---|---|---|---|---|---|---|---|
| wf_f462bfdf-b71 | 2026-08-11T03:16 | deep-research | claude-fable-5 | 103 | 661 | 302,896 | 343,811 | 1,552,552 | 7,113,238 | $46.74 | $58.38 |
| wf_808466a2-e9f | 2026-08-14T04:29 | deep-research | claude-fable-5 | 102 | 587 | 307,835 | 317,616 | 1,601,723 | 5,889,670 | $44.87 | $56.88 |
| wf_6e3fb5f9-cc8 | 2026-08-14T04:30 | deep-research | claude-fable-5 | 103 | 697 | 304,473 | 373,032 | 1,611,198 | 7,901,197 | $49.74 | $61.82 |
| wf_00951cbd-e2e | 2026-08-15T06:23 | deep-research | <synthetic>,claude-opus-4-8 | 103 | 593 | 304,747 | 202,037 | 1,501,559 | 6,088,170 | $19.00 | $24.63 |
| wf_7c2ff94f-da9 | 2026-08-21T00:27 | deep-research | claude-opus-4-8 | 102 | 537 | 309,465 | 209,430 | 1,352,482 | 4,900,193 | $17.69 | $22.76 |
| wf_2fef2dd1-2b2 | 2026-08-21T13:04 | deep-research | claude-fable-5 | 99 | 617 | 304,174 | 367,129 | 1,575,168 | 6,437,387 | $47.53 | $59.34 |
| wf_dd769a75-2c4 | 2026-08-21T20:43 | deep-research | claude-fable-5 | 97 | 865 | 292,139 | 493,829 | 1,815,313 | 11,007,270 | $61.31 | $74.93 |
| wf_b4dbb5ed-1e4 | 2026-08-27T22:40 | deep-research | <synthetic>,claude-fable-5 | 195 | 944 | 392,184 | 498,324 | 2,482,320 | 9,811,134 | $69.68 | $88.30 |
| wf_910a75c7-57d | 2026-09-01T22:42 | deep-research | claude-fable-5-1 | 99 | 547 | 47,717 | 196,918 | 1,929,598 | 16,476,784 | $38.56 | $53.03 |
| wf_162be8db-033 | 2026-09-02T22:00 | deep-research | claude-fable-5-1 | 100 | 638 | 59,685 | 170,052 | 2,065,883 | 20,381,825 | $40.02 | $55.51 |

## 6. Cost estimate (USD)

| scope | scenario A | scenario B |
|---|---|---|
| main | $814.68 | $929.51 |
| subagent | $3,030.10 | $3,489.17 |
| workflow | $435.13 | $555.59 |
| **total** | **$4,279.92** | **$4,974.27** |

### Per model

| model | A main | A sub | A wf | A total | B main | B sub | B wf | B total |
|---|---|---|---|---|---|---|---|---|
| <synthetic> | $0.00 | $0.00 | $0.00 | $0.00 | $0.00 | $0.00 | $0.00 | $0.00 |
| claude-fable-5 | $550.50 | $2,401.02 | $319.86 | $3,271.39 | $618.38 | $2,628.94 | $399.65 | $3,646.96 |
| claude-fable-5-1 | $146.66 | $442.42 | $78.58 | $667.67 | $173.56 | $653.83 | $108.55 | $935.94 |
| claude-haiku-4-5-20251001 | $0.00 | $0.93 | $0.00 | $0.93 | $0.00 | $1.12 | $0.00 | $1.12 |
| claude-opus-4-8 | $117.51 | $182.25 | $36.69 | $336.46 | $137.57 | $200.32 | $47.39 | $385.28 |
| claude-opus-5 | $0.00 | $3.47 | $0.00 | $3.47 | $0.00 | $4.97 | $0.00 | $4.97 |

## 7. Misc counts

- Assistant output text characters (text blocks): **3,283,283**; thinking chars: 62,891; tool_use input JSON chars: 13,223,142
- Bash commands: **15,111**; of which containing `git commit`: **334**
- Edit/Write calls (Edit, Write, MultiEdit, NotebookEdit): **1,485**; distinct files edited: **469**
