---
description: Run deterministic checks for the project OpenCode infrastructure
agent: build
subtask: false
---

Run `scripts/verify-agent-structure.ps1` from the project root. Report its exit
code and complete output. Do not fix files automatically. If a check fails,
explain the failing invariant and stop without changing anything.
