# Installed Claude Code Skills

> Sources: Ben Guetta, 2026-05-21
> Raw: [Installed Skills](../../raw/skills/2026-05-21-installed-skills.md)

## Overview

Eight skills installed in this project as of 2026-05-21, covering browser automation, UI development, debugging methodology, knowledge management, and skill creation. All installed via `npx skills add <repo> --skill <name>` and symlinked into `.claude/skills/` for Claude Code.

## Skills Reference

### find-skills
Discovers and installs agent skills from the open ecosystem using `npx skills` as a package manager. Guides through quality checks (install count, source reputation, GitHub stars) before installation.
- Source: `vercel-labs/skills` | Risk: Gen=Safe, Snyk=Med

### agent-browser
Full browser automation via Chrome/Chromium Chrome DevTools Protocol (CDP). Provides accessibility-tree snapshots, element interaction, session persistence, and video recording. Use for website automation, form filling, screenshots, scraping, testing, and Electron apps.
- Source: `vercel-labs/agent-browser` | Risk: Gen=Safe, Snyk=Med

### skill-creator
Complete workflow for creating, testing, benchmarking, and publishing new skills. Runs paired with/without-skill evals, generates a browser-based review viewer, and iterates based on feedback. Includes description optimizer via `run_loop.py`.
- Source: `anthropics/skills` (official) | Risk: Gen=Safe, Snyk=Low

### shadcn
Manages shadcn/ui components and design systems. Provides component documentation, CLI integration, and design token guidance. Primarily for React/Next.js + Tailwind projects, but its OKLCH color token system and design language are applicable to any CSS project.
- Source: `shadcn/ui` | Risk: Gen=Safe, Snyk=Med

### using-superpowers
Meta-skill that establishes the skill invocation protocol. Key rule: if there's even a 1% chance a skill applies, invoke it. Helps prioritize process skills before implementation skills.
- Source: `obra/superpowers` | Risk: Gen=Safe, Snyk=Low (Socket: 1 alert)

### systematic-debugging
Four-phase hypothesis-driven debugging: Root Cause Investigation → Pattern Analysis → Hypothesis Testing → Implementation. Discourages quick fixes, guessing, and applying multiple changes at once. After 3 failed fixes, question the architecture.
- Source: `obra/superpowers` | Risk: Gen=Safe, Snyk=Low

### ui-ux-pro-max
UX and visual design best practices for building polished interfaces.
- Source: `nextlevelbuilder/ui-ux-pro-max-skill` | Risk: **Gen=High Risk**, Snyk=Low — review before heavy use.

### karpathy-llm-wiki
Builds and maintains a personal LLM-powered knowledge base. Two-layer architecture: `raw/` (immutable source material) and `wiki/` (compiled articles). Supports Ingest, Query, and Lint operations. Based on Andrej Karpathy's wiki methodology.
- Source: `astro-han/karpathy-llm-wiki` | Risk: Gen=Safe, Snyk=Med

## Installation Command Pattern

```bash
npx skills add https://github.com/<org>/<repo> --skill <skill-name>
```

## See Also

- [System Architecture](../vpd-controller/system-architecture.md)
