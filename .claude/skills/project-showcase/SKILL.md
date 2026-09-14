---
name: project-showcase
description: Turn a GitHub repository into a polished, standalone, product-style website that presents the project as a real product — for use as a portfolio piece or resume artifact. Use this skill whenever the user wants to showcase a GitHub project, turn a repo into a landing page or portfolio site, make a "product page" for something they built, or wants a project to look professional for recruiters/hiring managers instead of just linking a repo. Trigger even if they don't say "skill" or name a file format — phrases like "make this repo look professional," "I need a site for my resume," "showcase this project," "turn this into a portfolio piece," or pasting a GitHub URL and asking for a landing page all qualify.
---

# Project Showcase

## What this does

Turns something the user built — usually a GitHub repo — into a standalone, professional website that presents it *as a product*, not as a repo. The site itself is the resume artifact: instead of a bullet point on a resume or a link to source code, a hiring manager or recruiter lands on a real, polished page for something the user built.

## Core idea — reframe, don't just document

A README documents how something works. A product page sells why it matters. The job here is translation: take the technical facts (what the code does, what stack it uses, what problem it solves) and reframe them the way a real product marketing site would — problem/solution framing, features stated as benefits, screenshots or demos front and center, tech stack shown as a craft signal rather than a dependency list.

## Workflow

### 1. Gather the source material

- If given a GitHub URL, start with the README: `https://raw.githubusercontent.com/{owner}/{repo}/{branch}/README.md` (try `main`, then `master` if that 404s). Also fetch the normal repo page for the short description, topics, and primary language if that context is missing from the README.
- **When the README is thin, missing a features/tech-stack section, clearly stale, or the user asks for deeper analysis — don't guess, look at the actual code.** Clone it (`git clone --depth 1 <url> /tmp/<repo-slug>`) and pull the real signal rather than reading every file:
  - Manifest files (`package.json`, `requirements.txt`, `Cargo.toml`, `go.mod`, `pyproject.toml`) — the confirmed dependency list and tech stack, since the README can lag behind what's actually in the code.
  - The directory structure — a shallow listing is usually enough to infer real architecture. An `/api`, `/firmware`, `/workers`, or `/components` folder says more about what this actually is than a paragraph of prose.
  - Entry-point files (`main.*`, `index.*`, `src/lib.rs`, etc.) for a fast read on what the code actually does.
  - Screenshots or images already committed to the repo (`/screenshots`, `/docs`, `/assets`, or images the README embeds) — pull these directly instead of asking the user for something that already exists.
  - Infra config (`wrangler.toml`, `docker-compose.yml`, `.github/workflows/`) if relevant to the craft-signal section.
  - This is reconnaissance for a landing page, not a code review — manifest + structure + entry points is usually enough; don't read every source file end to end.
- If it's private and can't be cloned, or the user just wants to hand-feed content: ask for what's missing rather than guessing — the description, key features, tech stack, and any screenshots, GIFs, or live demo link they have. **Never invent metrics, user counts, or results that weren't provided.**
- Notice who this needs to impress, if it's clear from context (a specific role or company vs. general portfolio use) — it shapes emphasis. A backend-heavy audience cares about architecture; a product-management audience cares more about the "why" and the outcome.

### 2. Distill into a product narrative

Before writing any code, work out:

- **The one-line pitch** — what it does and for whom, in plain language, no jargon dump.
- **The problem it solves** — why this exists at all, told briefly enough to fit on a page, not a dissertation.
- **3–5 features, framed as benefits** — see `references/copy-patterns.md` for how to convert a technical capability into feature copy people who aren't engineers will still find compelling.
- **The craft signal** — tech stack, notable technical decisions, anything that shows engineering judgment. This is resume content, so don't hide the technical depth — just don't lead with it over the "so what."
- **Proof, if it exists** — screenshots, a live demo link, usage stats, or context like "used daily in production." Skip this section entirely if nothing concrete exists; an empty "results" section with vague claims undercuts the page more than leaving it out.

If the user hands over already-written copy, use it as-is rather than rewriting it unprompted — they may want to write it themselves for a given project.

### 3. Design it like a real product page, not a template

Read `/mnt/skills/public/frontend-design/SKILL.md` before writing any HTML or CSS — this skill's entire value depends on the result not looking like a generic template. Each project should get a look suited to *that* project: a CLI tool, a piece of hardware firmware, and a consumer PWA shouldn't share a visual identity. Let the project's own personality (its name, its domain, an existing logo/icon, its actual tone) suggest the direction, rather than reusing one fixed palette and layout across every showcase site.

### 4. Build it

- Output a single self-contained `index.html` (inline `<style>`/`<script>`) by default — zero build step, deployable anywhere (GitHub Pages, Netlify, any static host) — unless the project specifically calls for something heavier.
- Standard sections, roughly in this order: hero (name + one-line pitch + primary call-to-action), the problem/why, features, screenshots or demo (if available), tech stack, proof/results (if available), links (live demo, GitHub repo), footer.
- Leave a small, clearly-marked spot in the footer/nav for a future "part of my project collection" link back to a portfolio hub. These sites are standalone for now — leave the link inert (commented out, or pointing nowhere yet) but structured so wiring several of them together later is a find-and-replace, not a rebuild.
- Real links only. Use the actual GitHub repo URL and any live demo URL the user gave you — never fabricate one.

### 5. Deliver

Save to `/mnt/user-data/outputs/{project-slug}/index.html` and present it with `present_files`. Mention, only if it's relevant, that it can be dropped straight onto GitHub Pages or any static host with no build step.

## Content guidance

`references/copy-patterns.md` has before/after examples of turning README-speak into product copy — read it before drafting the narrative in step 2.
