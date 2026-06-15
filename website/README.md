# CDC Badge OS — Documentation Website

Source for the CDC Badge OS documentation site, built with [Astro Starlight](https://starlight.astro.build/).
The output is a fully static site that deploys to GitHub Pages, Codeberg Pages and GitLab Pages.

## Develop

```bash
npm ci
npm run dev          # local dev server
npm run build        # static build into dist/
npm run preview      # serve the built site
```

## Host-agnostic deployment

The build is parameterised by two environment variables consumed in `astro.config.mjs`:

| Variable | Meaning | Default |
|----------|---------|---------|
| `SITE_BASE` | Subpath the site is served from (project Pages serve at `/<repo>/`; a custom domain serves at `/`). Must start and end with `/`. | `/cdc-badge-os/` |
| `SITE_URL` | Absolute origin for canonical URLs and the sitemap. | `https://krim.codeberg.page` |

```bash
SITE_BASE=/ SITE_URL=https://docs.example.org npm run build   # custom domain at root
```

The published site is assembled by the repository CI alongside two sibling directories that are **not**
built by Astro:

- `/flasher/` — the ESP Web Tools flasher plus firmware binaries and manifests.
- `/api/` — the Doxygen code reference.

Authoring rules and the page tree live in [`SPEC.md`](./SPEC.md).
