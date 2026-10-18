# Editing the guide

The public guide is published at https://kennethassogba.github.io/buildray/.
English pages live in `website/`; French pages live in `website/fr/`.
Keep explanations short and define a term before relying on it. Keep both
languages consistent when changing commands, limits or results.
French headings use the English heading's anchor explicitly, so changing
language keeps the reader in the same section. The link check verifies this.

```bash
npm ci
npm run docs:dev
npm run docs:build
npm run docs:check
npm run docs:preview
```

Use Node 22 or newer. Preview serves the production build at
`http://127.0.0.1:4173/buildray/`. The website uses VitePress with local search;
Node and npm are documentation tools, not CLI runtime requirements.
The Vite override keeps the stable VitePress theme on a patched development
server. Check dev, build and preview when updating either dependency.

`docs.yml` requires a guide update when a PR adds or removes CLI commands,
options or Action inputs/outputs. It builds and checks local links on PRs. On changes to the website on
`main`, it publishes the checked output to GitHub Pages. Repository Pages must
use GitHub Actions as its build source. Deployments use the `github-pages`
environment; pull requests only build and check the site.

Technical references and raw evidence remain in `docs/`. Link to those for
reproduction details rather than copying long reports into the reading guide.
When publishing measurements, state the revision, configuration, measured scope
and uncertainty. Do not combine results from different experiments.
