# LlamaBoss Documentation Site

This folder is a static Cloudflare Pages-ready documentation page for LlamaBoss.

Generated from source snapshot:

`LlamaBoss_Source_h_cpp_20260523_131552.zip`

## Local preview

Open `index.html` directly in a browser, or serve the folder:

```powershell
cd docs-site
py -3 -m http.server 8080
```

Then open:

```text
http://localhost:8080
```

## Recommended repo layout

Place this folder in the LlamaBoss GitHub repo as either:

```text
website/
```

or:

```text
docs-site/
```

For Cloudflare Pages Git integration:

```text
Build command: leave blank / none
Build output directory: docs-site
```

If you use `website/` instead, set the output directory to `website`.

## Direct Cloudflare upload

From the folder containing `index.html`:

```powershell
npx wrangler pages deploy . --project-name llamaboss-docs
```

## Suggested custom domain

Good options:

```text
docs.llamaboss.com
llamaboss.com/docs
```

`docs.llamaboss.com` is cleaner for Cloudflare Pages because it can be attached as a project custom domain without needing app routing.

## Hermes task prompt

Paste this to Hermes when you are ready:

```text
Please add the attached LlamaBoss documentation static site to my LlamaBoss repository under docs-site/.

Steps:
1. Copy index.html, styles.css, _headers, and README_DEPLOY.md into docs-site/.
2. Commit with message: Add LlamaBoss documentation site.
3. Push to the main branch.
4. If the Cloudflare Pages project is Git-connected, stop after the push and report the deployment status.
5. If it is not Git-connected and Wrangler is logged in, run:
   npx wrangler pages deploy docs-site --project-name llamaboss-docs
6. Do not modify LlamaBoss application source files for this docs-only change.
```

## Next documentation pages to add

- Installation and first-run model setup
- Model download/catalog guide
- Tool safety and approval behavior
- Projects and skills guide
- Excel/PDF/DOCX workflows
- Developer architecture notes
