---
name: register-reference
description: Use this skill when the task is to register, normalize, and link a bibliographic reference for a thesis or paper. Trigger when the user provides a DOI, URL, title, BibTeX entry, partial citation, or manual metadata and wants it added to refs.bib, assigned a stable citation key, and linked into method cards, figure cards, or section packets. Do not use this skill for broad literature review, paper summarization, or manuscript drafting unless the main task is citation registration and propagation.
---

# Purpose

This skill turns a source into a **tracked manuscript reference object**.

A tracked reference object has:

- a stable citation key
- a valid or at least usable BibTeX entry
- a clear path into `refs.bib`
- optional reference card metadata
- links to relevant method cards
- links to relevant figure cards
- links to relevant section packets

The goal is to make references reusable across the whole thesis pipeline instead of leaving them as loose author-year mentions.

# When to use this skill

Use this skill when the user wants to:

- add a paper, book, report, thesis, or website citation to the project
- turn a DOI, URL, title, or partial citation into a `.bib` entry
- normalize inconsistent citation keys
- link sources to method cards, figure cards, or section packets
- register references that Prism should later see in packets and LaTeX drafting
- clean up citation infrastructure for thesis writing

Typical prompts:

- “register this DOI in my refs.bib”
- “add this paper and link it to the melt-model method cards”
- “turn this citation into a BibTeX entry and connect it to the Methods packet”
- “normalize these Hock/Pellicciotti references and insert the keys into the right cards”

# When not to use this skill

Do not use this skill when the main task is:

- summarizing a paper in depth
- comparing the scientific claims of multiple papers
- writing the literature review section
- deciding the truth of a claim
- broad bibliography cleanup unrelated to a specific registration task

If the user needs interpretation of what a source means for the thesis, that can be done separately. This skill focuses on **registration, normalization, and linking**.

# Core principle

A reference is not just a line in `refs.bib`.

A useful thesis reference should be:

- stable in key naming
- traceable in the bibliography
- easy to cite in LaTeX
- linked to the cards and packets that rely on it
- reusable in Prism drafting without ambiguity

# Accepted inputs

This skill can start from any of these:

- DOI
- URL
- title
- author-year string
- BibTeX snippet
- ISBN
- manually provided metadata
- existing citation key that needs normalization
- optional target files to link

Examples:

- DOI: `10.3189/S0022143000003074`
- URL: `https://...`
- title: `Radiation balance of snow-covered slopes`
- partial citation: `Hock 1999`
- BibTeX: `@article{...}`
- target cards: `MC-003`, `MC-004`
- target packet: `workspace/section_packets/methods_melt_models.md`

# Required workflow

## Step 1 — Resolve or verify bibliographic identity

From the input, determine the reference identity as reliably as possible.

Prefer, in order:

1. existing BibTeX provided by the user
2. DOI
3. ISBN
4. authoritative URL
5. full title + author/year
6. partial citation only if nothing else is available

If metadata is incomplete, do not invent precision. Create the cleanest provisional entry possible and clearly note what is missing.

## Step 2 — Check for an existing citation key

Before creating a new entry, inspect the current bibliography and any reference cards for duplicates.

Look for:
- same DOI
- same title
- same authors/year
- same URL
- near-duplicate existing key

If the reference already exists, prefer updating and linking the existing entry over creating a duplicate.

## Step 3 — Assign or normalize the citation key

Use a stable, readable citation key.

Preferred pattern:

- `SurnameYear`
- `SurnameYearShortToken` when needed for collisions
- `OrgYearShortToken` for institutional reports
- `SurnameEtAlYear` only if needed for consistency with the existing bibliography

Examples:

- `Hock1999`
- `Pellicciotti2005`
- `Chen2006Terrain`
- `Steger2022Horayzon`

Rules:
- keep keys concise
- avoid spaces and punctuation except internal capitalization
- preserve an existing stable key if it is already widely used in the manuscript
- do not create unnecessary variants

## Step 4 — Create or update the BibTeX entry

Write or update the reference in:

`refs.bib`

Use the most appropriate BibTeX type available, for example:

- `@article`
- `@book`
- `@inproceedings`
- `@techreport`
- `@phdthesis`
- `@misc`
- `@online` only if your BibLaTeX setup expects it

Populate as much reliable metadata as possible:
- author
- title
- year
- journal / booktitle / publisher / institution
- volume / number / pages
- doi
- url
- isbn
- note
- urldate if appropriate

Do not pad entries with guessed metadata.

## Step 5 — Create or update a reference card

Preferred path:

`workspace/reference_cards/<CITATION_KEY>.md`

The reference card is a human-readable object that explains why the source matters in this project.

## Step 6 — Link the citation key into project artifacts

When relevant targets are provided, or when the manuscript role is clear, update:

- method cards in `workspace/method_cards/`
- figure cards in `workspace/figure_cards/`
- section packets in `workspace/section_packets/`

Use the citation key, not loose author-year prose.

Only link the reference where it is genuinely relevant.

# Required output files

This skill should create or update:

## 1. Bibliography file

Preferred path:

`refs.bib`

This is the LaTeX source of truth for citations.

## 2. Reference card

Preferred path:

`workspace/reference_cards/<CITATION_KEY>.md`

This is the manuscript-facing note about why the source matters.

## 3. Linked project artifacts

When appropriate, update:
- `workspace/method_cards/*.md`
- `workspace/figure_cards/*.md`
- `workspace/section_packets/*.md`

# Reference card format

Use this exact structure.

## Reference Card: <CITATION_KEY>

**Citation key**
- `<CITATION_KEY>`

**Reference type**
- `<article / book / report / thesis / website / other>`

**Bibliographic identity**
- Authors: <authors if known>
- Year: <year if known>
- Title: <title>
- Venue / Publisher: <journal, publisher, institution, or equivalent>
- DOI: <doi or none>
- URL: <url or none>

**Why it matters here**
- <1–3 sentences on why this source is relevant to the thesis>

**Likely manuscript use**
- <chapter/section guesses if known>

**Linked method cards**
- <MC-ID list or none>

**Linked figure cards**
- <FIG-ID list or none>

**Linked section packets**
- <packet filenames or none>

**Notes**
- <ambiguities, incomplete metadata, or cautions>

# Linking rules

## Method cards

When updating a method card, use a section like:

```markdown
**Relevant references**
- `Hock1999` — temperature-index melt formulation with topographic radiation factor
- `Pellicciotti2005` — albedo parameterization and shortwave factor calibration