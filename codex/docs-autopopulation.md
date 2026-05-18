# Docs Site Autopopulation

This note parks the docs-site publishing plan for later execution.

## Goal

Publish user-facing documentation from the repo to a dedicated docs
endpoint, likely `docs.erbos.me`, with:

- expandable navigation
- full-text search
- low-friction browsing
- clear separation between public docs and internal agent docs

## Source Of Truth

The repo remains the source of truth.

- `docs/` = public language / compiler / runtime / examples docs
- `std/` = stdlib-specific internal or semi-internal docs
- `codex/` = internal agent / audit / maintenance docs

The public docs site should render from `docs/`, not become a second
manual authoring surface.

## Recommended Publishing Model

Use docs-as-code with two public tracks:

- `docs.erbos.me/` = stable
- `docs.erbos.me/next/` = current main / canary

If there are still effectively no real users, a simpler temporary model
is acceptable:

- root site = current main
- preview deploys for every PR / branch

Once people start depending on the docs, split stable vs next.

## Suggested Stack

- Astro
- Starlight
- Cloudflare Pages
- built-in Pagefind search first
- optional Algolia DocSearch later if search scale or relevance needs it

Why this stack:

- docs-first UI/UX
- filesystem-driven sidebar generation
- good static performance
- easy PR preview deploys on Cloudflare Pages
- low operational complexity

## Publishing Boundary

Publish:

- `README`-level getting started material when duplicated or curated for docs
- `docs/language-guide.md`
- `docs/language-law.md`
- `docs/builtins.md`
- `docs/keywords.md`
- `docs/runtime.md`
- `docs/examples.md`
- selected stable reference pages

Do not publish:

- `codex/`
- audit ledgers
- session handoff docs
- agent-only operational notes
- raw review workflow docs

## Site Structure Recommendation

```text
repo/
  docs/                  # canonical public prose
  std/                   # stdlib implementation docs
  codex/                 # internal only
  website/               # docs site implementation
```

For the site:

```text
website/
  src/content/docs/      # rendered content tree
```

## Sync Model

Preferred first step:

- keep repo `docs/` canonical
- sync `docs/` into `website/src/content/docs/` at build time

That avoids creating a second living docs tree too early.

Later, if it proves cleaner, the website content tree can become the
canonical source, but not before the repo docs structure is stable.

## Update Policy While Potato Evolves

No language change is complete until:

1. code is updated
2. tests are updated
3. docs are updated

If code and docs disagree:

- code + tests are truth
- docs must be fixed before merge

## Branch / Release Strategy

- `main` = current development truth
- `release/*` or tags = stable snapshots

Deployments:

- `main` -> docs preview or `/next`
- releases / tags -> stable root

## CI / Automation Ideas

1. Docs sync step
   - mirror `docs/` into the site content tree during build

2. Drift checks
   - `make test`
   - dead-link checks
   - grep for banned old syntax in active docs
   - optional compile/verify selected code snippets later

3. Preview deploys
   - every docs PR gets a preview URL

## Governance Rule

Adopt this sentence repo-wide:

> No language change is complete until docs, tests, and examples match
> the shipped behavior.

## Later Execution Checklist

- [ ] finish docs cleanup / reconciliation in repo
- [ ] decide stable vs next policy
- [ ] scaffold docs site, likely under `website/`
- [ ] wire repo `docs/` into site content
- [ ] deploy to Cloudflare Pages
- [ ] attach `docs.erbos.me`
- [ ] enable PR preview deploys
- [ ] optionally protect previews with Cloudflare Access
