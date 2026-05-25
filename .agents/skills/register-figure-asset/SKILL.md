---
name: register-figure-asset
description: Use this skill when the task is to register a thesis or paper figure from an image path or link plus a short description, then create or update the figure registry, generate a figure card, prepare the figure with crop, composite, label, and format-conversion operations when needed, normalize the asset into a manuscript-compatible format, copy it into the manuscript figures tree, and link the figure to relevant method cards. Trigger when the user wants to ingest a new figure asset, assign a stable figure ID, decide likely manuscript placement, create caption seeds, preserve provenance, or connect figures to method cards so packet-building and Prism drafting can reference them. Do not use this skill for drafting manuscript prose, packet assembly, or raw code interpretation unless figure registration is the main task.
---

# Purpose

This skill turns a visual asset into a tracked manuscript figure object.

A tracked figure object has:

- a stable figure ID
- a manuscript-oriented role
- a likely chapter or section placement
- a LaTeX label seed
- a manuscript-local copied asset in a manuscript-compatible format
- a figure card
- an entry in the central figure registry
- links to related method cards when justified

The goal is to make figures first-class thesis artifacts rather than loose image files.

# When to use this skill

Use this skill when the user provides any of the following:

- an image path
- an image link
- a plot file
- a schematic file
- a render or map
- a brief description of what the figure shows
- optional related method card IDs
- optional target chapter or section

Typical prompts:

- "register this image and link it to the right method cards"
- "take this plot and create a figure card"
- "index this figure and attach it to the relevant method cards"
- "I have a validation figure for HORAYZON comparison"
- "this image should become a tracked figure for Prism packets"

# When not to use this skill

Do not use this skill when the main task is:

- writing the Methods or Results prose
- assembling a section packet
- interpreting raw code into method cards
- editing or redesigning the image itself
- creating the final publication graphic layout
- bibliography management

If the user mainly needs packet assembly, prefer `build-section-packet`.
If the user mainly needs code interpretation, prefer `codebase-method-cards`.

# Core principle

A figure is not just a file.

A figure should be treated as a structured manuscript object with:

- provenance
- a manuscript-ready copied asset path
- a manuscript-compatible delivery format
- rhetorical function
- manuscript placement
- traceable links to methods or results
- future reusability in packet building and drafting

# Accepted inputs

This skill can start from any of these:

- local image path
- local figure file path
- URL or shared link
- relative path inside the thesis project
- brief description written by the user
- optional edit instructions such as crop, trim, composite, or label requests
- optional target chapter or section
- optional related method card IDs
- optional preferred label or short name

Examples:

- `figures/drafts/horayzon_svf_comp.png`
- `figures/final/render_target_pipeline.pdf`
- `https://.../plot.png`
- description: "comparison between UE-derived sky view factor and HORAYZON on 19 Feb"
- edit request: "crop the legend whitespace, place the map and histogram side by side, and add panel labels A and B"
- related method cards: `MC-006`, `MC-010`

# Figure preparation support

This skill can prepare images before registration when the manuscript needs a cleaner or more structured derivative.

Supported preparation operations:

- crop to a specified region
- trim empty margins or whitespace when appropriate
- resize one or more panels for a composite
- create horizontal, vertical, or grid composites from multiple inputs
- add simple panel labels such as `A`, `B`, `C`
- convert the prepared result to a manuscript-compatible format such as `PNG` or `PDF`

Scientific guardrails:

- do not alter the scientific meaning of the figure
- do not remove scale bars, legends, axes, or captions unless the user explicitly asks and the result remains interpretable
- preserve the original source asset as provenance even when the manuscript derivative is cropped or composited
- record non-trivial edits in the figure card notes and, when useful, in registry metadata

Operational helper:

- use [prepare_manuscript_figure.ps1](f:/UnrealEngine/Unreal%20Projects/00_UiO/2025_MasterThesis/BrightnessEngine/.agents/skills/register-figure-asset/scripts/prepare_manuscript_figure.ps1) for common crop, composite, label, and conversion tasks
- helper notes live in [README.md](f:/UnrealEngine/Unreal%20Projects/00_UiO/2025_MasterThesis/BrightnessEngine/.agents/skills/register-figure-asset/scripts/README.md)

# Required workflow

## Step 1 - Inspect the figure input

Determine:

- what the asset appears to be
- whether the path or link is valid enough to reference
- whether the user description is sufficient
- what the current source format is
- whether the source format is already manuscript-compatible
- whether this is likely:
  - explanatory
  - analytical
  - visual_output
  - discussion or conceptual

Do not overclaim what the figure shows if the description is sparse.

## Step 2 - Classify figure role

Assign the figure one primary category.

Use these categories:

- `explanatory`
  - workflow diagrams
  - method schematics
  - architecture figures
  - normalization sketches
- `analytical`
  - validation plots
  - comparison plots
  - metric summaries
  - result maps used as evidence
- `visual_output`
  - landscape renders
  - snow cover images
  - scenario visualizations
  - aesthetic but still thesis-relevant outputs
- `discussion`
  - conceptual uncertainty diagrams
  - limitations schematics
  - future-work concept figures

If uncertain, state the uncertainty and choose the most defensible category.

## Step 3 - Prepare, normalize, and copy into the manuscript figures tree

Every registered figure should also have a manuscript-local copy of the image.

The manuscript copy must be in a format that is safe for Prism and common LaTeX editors such as Overleaf.

Before copying into the manuscript tree, decide whether the source needs preparation beyond format conversion.

Typical preparation cases:

- crop away empty borders or irrelevant UI chrome from screenshots
- combine multiple related source panels into one manuscript figure
- add panel labels such as `A`, `B`, and `C`
- resize mismatched panels so the final composite reads cleanly

When these edits are needed, create the prepared derivative first and only then register the manuscript copy.
Prefer using the helper script:

- [prepare_manuscript_figure.ps1](f:/UnrealEngine/Unreal%20Projects/00_UiO/2025_MasterThesis/BrightnessEngine/.agents/skills/register-figure-asset/scripts/prepare_manuscript_figure.ps1)

Preferred manuscript formats:

- `PDF` for vector figures, line art, and schematic figures when a true vector export is available
- `PNG` for rasters, screenshots, rendered scenes, maps, and diagnostic plots
- `JPG` only for photographic or screenshot-style content where lossy compression is acceptable

Treat these as source formats that usually need conversion before manuscript registration:

- `TIF` or `TIFF`
- `GeoTIFF`
- `SVG` when the downstream manuscript toolchain does not guarantee direct support
- raw analysis formats or specialized GIS outputs that are not editor-friendly

Rules:

- choose the manuscript-compatible derivative before writing `manuscript_asset`
- preserve the original source asset reference as provenance
- do not replace or overwrite the original scientific source file
- if multiple inputs are used for a composite, record all important sources in the figure card provenance notes
- if the source is already manuscript-compatible, copy it directly
- if the source is not manuscript-compatible, create a derivative in `PNG` or `PDF` first, then store that derivative as the manuscript copy
- if the source is a raster scientific product such as a GeoTIFF, do not treat the raw scientific file itself as the manuscript asset; export a visual derivative for the manuscript and keep the raw raster only as provenance
- keep the file extension aligned with the actual manuscript-compatible derivative
- record any important conversion note in the figure card when the derivative materially differs from the source representation
- record non-trivial crop, label, or composite operations in the figure card and, when useful, in a `manuscript_operations` registry field

Choose the destination folder from the primary category:

- `explanatory` -> `thesis/manuscript/figures/methods/`
- `analytical` -> `thesis/manuscript/figures/results/`
- `discussion` -> `thesis/manuscript/figures/discussion/`
- `visual_output` -> `thesis/manuscript/figures/visual_outputs/`

Additional copy rules:

- create the destination folder if it does not already exist
- copy the compatible derivative rather than moving it
- give the manuscript copy a stable manuscript-facing filename derived from the figure ID or short name
- if the asset is remote, download or otherwise materialize a local compatible manuscript copy when feasible; if not feasible, state that clearly in the figure card and registry notes

Suggested filename style:

- `FIG-METH-RTPIPE-01.png`
- `FIG-RES-HORAYZON-SVF-01.pdf`

Record the copied manuscript path in figure metadata as `manuscript_asset`.

## Step 4 - Infer likely manuscript placement

Suggest the most likely placement:

- Chapter
- Section
- optional subsection

Examples:

- Methods > UE radiation-index workflow
- Results > SVF comparison with HORAYZON
- Results > Melt-out-date comparison
- Discussion > limitations of snow redistribution

Placement should follow manuscript logic, not folder location.

## Step 5 - Assign a stable figure ID

Generate a persistent figure ID using this pattern:

- `FIG-METH-<NAME>-01`
- `FIG-RES-<NAME>-01`
- `FIG-DISC-<NAME>-01`

Rules:

- use `METH`, `RES`, `DISC`, `VIS`, or `INTRO` where appropriate
- use a short descriptive token in uppercase kebab-like compact form
- add a two-digit serial suffix
- keep IDs stable once created

Examples:

- `FIG-METH-RTPIPE-01`
- `FIG-METH-REFSTRIP-01`
- `FIG-RES-HORAYZON-SVF-01`
- `FIG-RES-MELTOUT-COMP-01`
- `FIG-VIS-TOTALP-SCENE-01`

Before creating a new ID, check whether the same figure asset and role already appear to be registered. Prefer updating instead of duplicating.

## Step 6 - Create a LaTeX label seed

Generate a manuscript label like:

- `fig:rt_pipeline`
- `fig:horayzon_svf_comparison`
- `fig:meltout_comparison`

Keep labels concise, stable, and manuscript-friendly.

## Step 7 - Determine figure-to-method-card links

If method cards are supplied, or if figure purpose clearly implies a relation, assign one or more relation types:

- `explains`
- `produces`
- `validates`
- `discusses`

Meaning:

- `explains`: the figure helps explain the method
- `produces`: the method card's workflow generates the figure or its data
- `validates`: the figure is used to evaluate that method or component
- `discusses`: the figure supports interpretation or limitations discussion

Only create links that are defensible from the asset description and context.
Do not force links when uncertain.

## Step 8 - Update the registry and cards

Create or update:

- the manuscript-compatible derivative when needed
- the manuscript-local copied asset
- the central figure registry
- the individual figure card
- linked method cards, if applicable

# Required output files

This skill should write or update the following files.

## 1. Central registry

Preferred path:

`workspace/indexes/figures_index.yaml`

This is the machine-readable source of truth.

It should preserve both:

- `source_asset` for provenance
- `manuscript_asset` for the compatible copy used by the manuscript

Optional metadata is encouraged when preparation is substantial:

- `source_assets` for multi-input composites
- `manuscript_operations` for crop, composite, resize, or label operations

## 2. Figure card

Preferred path:

`workspace/figure_cards/<FIGURE_ID>.md`

This is the human-readable figure object.

The card should explicitly record both the original source asset and the compatible copied manuscript asset.
If a conversion was required, note that clearly in the provenance or source notes.
If the manuscript figure was cropped, composited, or labeled, note those edits clearly.

## 3. Method card links

Update relevant files in:

`workspace/method_cards/`

Add or update the `Linked figures` section.

# Figure registry schema

Use this structure in `figures_index.yaml`.

```yaml
figures:
  - id: FIG-RES-HORAYZON-SVF-01
    short_name: horayzon-svf-comparison
    manuscript_label: fig:horayzon_svf_comparison
    category: analytical
    chapter: Results
    section: SVF comparison with HORAYZON
    subsection: null
    status: draft
    purpose: Compare UE-derived sky view factor against HORAYZON reference estimates.
    source_asset: figures/drafts/horayzon_svf_comp.tif
    source_assets:
      - figures/drafts/horayzon_svf_comp.tif
      - figures/drafts/horayzon_histogram.png
    manuscript_asset: thesis/manuscript/figures/results/FIG-RES-HORAYZON-SVF-01.png
    manuscript_operations:
      - crop map panel to AOI frame
      - crop histogram panel to plotting region
      - create horizontal two-panel composite
      - add panel labels A and B
    source_type: plot
    source_description: comparison between UE-derived sky view factor and HORAYZON on 19 Feb
    related_method_cards:
      - id: MC-006
        relation: validates
      - id: MC-010
        relation: produces
    caption_seed: Comparison between UE-derived sky view factor against HORAYZON reference estimates for the selected timestep.
```
