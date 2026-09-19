---
description: Create a project plan in .opencode/plans without changing implementation files
agent: planner
subtask: false
---

Create a plan for this task: $ARGUMENTS

Use the project planner rules. Inspect the minimum relevant files, create the
plan in `.opencode/plans/`, and do not modify any implementation, configuration,
documentation, MCP, skill, or Git file. After the file is actually created,
print its exact relative path on a separate line using exactly:

PLAN: .opencode/plans/YYYY-MM-DD-name.md
