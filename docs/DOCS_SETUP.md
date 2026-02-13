# Documentation Setup Guide

This guide explains the frontmatter pattern for all documentation pages.

## What Was Fixed

1. **Dark Theme**: Added `color_scheme: dark` to `_config.yml`
2. **Mermaid Support**: Created `_includes/head_custom.html` with Mermaid.js integration (dark theme)
3. **Markdown Rendering**: Added frontmatter to key pages for proper Jekyll processing
4. **Navigation**: Configured Just the Docs navigation hierarchy

## Frontmatter Pattern

Every markdown file needs frontmatter at the top. Here are the patterns:

### Top-Level Section Index (e.g., `getting-started/index.md`)

```yaml
---
layout: default
title: Getting Started
nav_order: 2
has_children: true
permalink: /getting-started
---
```

### Child Page (e.g., `getting-started/installation.md`)

```yaml
---
layout: default
title: Installation
parent: Getting Started
nav_order: 1
---
```

### Grandchild Page (if you have sub-sections)

```yaml
---
layout: default
title: Advanced Setup
parent: Installation
grand_parent: Getting Started
nav_order: 1
---
```

## Navigation Order

Current top-level sections (nav_order):
1. Home (index.md)
2. Getting Started
3. Architecture
4. API Reference
5. Platform Support
6. Development
7. Hardware
8. Resources

## Remaining Pages to Update

You'll need to add frontmatter to all remaining `.md` files in:
- `getting-started/` (first-app.md, troubleshooting.md)
- `architecture/` (connectivity.md, data-flow.md, runtime.md, security.md)
- `api-reference/` (error-codes.md, manifest-format.md, native-api.md)
- `platform/` (esp32-s3.md, native-sim.md, nrf54l15.md, stm32.md)
- `development/` (best-practices.md, building-apps.md, debugging.md, ota-updates.md, sdk-api-reference.md, sdk-troubleshooting.md)
- `hardware/` (any child pages)
- `resources/` (faq.md, glossary.md, performance.md)

## Mermaid Diagrams

Mermaid diagrams will now render automatically. Just use:

\`\`\`mermaid
graph TD
    A[Start] --> B[End]
\`\`\`

The dark theme is already configured in `_includes/head_custom.html`.

## Testing Locally

To test the site locally before pushing:

```bash
cd docs
bundle install
bundle exec jekyll serve
```

Then visit `http://localhost:4000`

## Next Steps

1. Add frontmatter to all remaining pages using the patterns above
2. Adjust `nav_order` within each section as needed
3. Push to GitHub - pages will rebuild automatically
4. Check for any broken links or formatting issues
