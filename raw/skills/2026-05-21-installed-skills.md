# Installed Claude Code Skills — VPDcontrol Project

> Source: /Users/liat/Documents/VPDcontrol/.agents/skills/ (installed skill directories)
> Collected: 2026-05-21
> Published: Unknown

All skills installed in this project as of 2026-05-21. Skills are installed via `npx skills add <repo> --skill <name>` and symlinked into .claude/skills for Claude Code access.

## find-skills (vercel-labs/skills)

- **Purpose:** Discover and install agent skills from the open ecosystem
- **Trigger:** "How do I do X", capability questions, skill discovery
- **Security:** Gen=Safe, Socket=0 alerts, Snyk=Med Risk
- **Invoke:** `/find-skills`

## agent-browser (vercel-labs/agent-browser)

- **Purpose:** Full browser automation via Chrome/Chromium CDP
- **Capabilities:** Accessibility-tree snapshots, element references, sessions, auth, state persistence, video recording
- **Triggers:** Website navigation, form filling, screenshots, scraping, testing, Electron apps
- **Security:** Gen=Safe, Socket=0 alerts, Snyk=Med Risk
- **Invoke:** `/agent-browser`

## skill-creator (anthropics/skills)

- **Purpose:** Create, test, and publish new skills; run evals and benchmarks
- **Workflow:** Draft → test cases → run with/without skill → review via browser viewer → iterate
- **Security:** Gen=Safe, Socket=0 alerts, Snyk=Low Risk
- **Source:** Official Anthropic repo
- **Invoke:** `/skill-creator`

## shadcn (shadcn/ui)

- **Purpose:** Manage shadcn/ui components; design system guidance
- **Applies to:** React/Next.js projects with Tailwind CSS
- **Design tokens:** OKLCH color variables, semantic naming (--background, --primary, --muted, etc.)
- **Principles:** Compose don't reinvent; use semantic colors; built-in variants before custom styles
- **Security:** Gen=Safe, Socket=0 alerts, Snyk=Med Risk
- **Note:** CLI tools require React/Tailwind; design language applies to any CSS project
- **Invoke:** `/shadcn` (not user-invocable per SKILL.md)

## using-superpowers (obra/superpowers)

- **Purpose:** Meta-skill — teaches how to find and use skills
- **Key rule:** "If you think there is even a 1% chance a skill might apply, you ABSOLUTELY MUST invoke it"
- **Security:** Gen=Safe, Socket=1 alert, Snyk=Low Risk
- **Invoke:** `/using-superpowers`

## systematic-debugging (obra/superpowers)

- **Purpose:** Hypothesis-driven debugging with 4 mandatory phases
- **Phases:** Root Cause Investigation → Pattern Analysis → Hypothesis Testing → Implementation
- **Red flags:** Quick fixes, guessing, multiple fixes at once
- **Security:** Gen=Safe, Socket=0 alerts, Snyk=Low Risk
- **Invoke:** `/systematic-debugging`

## ui-ux-pro-max (nextlevelbuilder/ui-ux-pro-max-skill)

- **Purpose:** UX/visual design best practices
- **Security:** Gen=High Risk, Socket=0 alerts, Snyk=Low Risk
- **Note:** High Gen risk flag — review before heavy use
- **Invoke:** `/ui-ux-pro-max`

## karpathy-llm-wiki (astro-han/karpathy-llm-wiki)

- **Purpose:** Build and maintain a personal LLM-powered knowledge base (wiki)
- **Architecture:** raw/ (immutable sources) + wiki/ (compiled articles) + SKILL.md (schema)
- **Operations:** Ingest, Query, Lint
- **Core idea from Karpathy:** "The LLM writes and maintains the wiki; the human reads and asks questions."
- **Security:** Gen=Safe, Socket=0 alerts, Snyk=Med Risk
- **Invoke:** `/karpathy-llm-wiki`
