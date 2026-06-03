# VPD Controller UI Refactor Plan

> Source: /Users/liat/.claude/plans/bubbly-weaving-flask.md
> Collected: 2026-05-21
> Published: 2026-05-21

## Goal

Visual polish pass on data/index.html — better design tokens, typography, spacing, and component styles — without touching any JS logic, API calls, WebSocket handling, localStorage keys, HTML IDs, or JS-referenced class names. No commits or pushes.

## Guiding Skills

- **shadcn** — design token system, semantic color variables, component visual language
- **ui-ux-pro-max** — UX/visual best practices (installed 2026-05-21)

## Constraints

- Only data/index.html changes — specifically its style block and minor HTML semantic improvements
- Zero breaking changes — all JS selectors, IDs, and class names must remain intact
- No external dependencies added — stays a self-contained file
- File size must stay reasonable — gzip-embedded into ESP32 firmware

## Current Tech Stack

- Vanilla CSS with CSS custom properties (dark navy theme)
- Chart.js 4.4.4 + hammer.js + chartjs-plugin-zoom (CDN)
- WebSocket (native browser API)
- 4481 lines, single HTML file
- Build: scripts/build_html.py → src/ui_html.h (gzip embedded)

## Current Color Tokens

```css
--bg:       #07111d  (dark navy)
--surface:  #0d1c2b
--surface2: #12243a
--border:   #1c3048
--text:     #ddeeff
--muted:    #5d7e9a
--green:    #00e676
--yellow:   #ffc107
--red:      #ff5252
--blue:     #40c4ff
--accent:   #6d28d9  (purple)
--radius:   12px
```

## Planned Improvements

1. Redesign CSS custom properties using OKLCH color tokens (shadcn-inspired)
2. Add Inter font via Google Fonts CDN
3. Card component polish — inner highlight border, box-shadow, better spacing
4. Button improvements — consistent height, focus-visible ring, micro-animations
5. Tab & nav polish — pill-style active indicator, segmented control for mode tabs
6. Input & form improvements — focus ring, consistent height
7. Badge/status indicators — proper pill badges replacing raw spans
8. Table improvements — zebra rows, sticky header, better column alignment
9. Metric cards — dominant value display, color-coded border-left accents
10. Animation improvements — shadcn easing curve, prefers-reduced-motion support

## Status

Plan sent to Ultraplan for remote refinement. Pending implementation after plan approval.
