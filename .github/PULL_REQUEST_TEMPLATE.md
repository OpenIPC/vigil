<!--
Review standards live in best_practices.md and pr_compliance_checklist.yaml at
the repo root. A change that alters what runs on a board needs every section.
-->

## Problem

<!-- What is wrong or missing, and where it shows. -->

## Change

<!-- What the diff does, in a few lines. -->

## Tested on

<!--
Core-only changes: the ctest result.
Platform changes: the board (SoC, model, firmware) and what was run on it.
If something was not verified, say so here.
-->

## Evidence

<!-- Paste the output (logs, counters, captured frames). A description of the output is not the output. -->

```

```

## Scope

- [ ] No vendor header or platform conditional in src/core, src/ui or src/app
- [ ] No vendor SDK files, binaries or code copied from vendor samples
- [ ] New sources are compiled by some VIGIL_PLATFORM
- [ ] Parser/protocol changes come with a test in tests/
