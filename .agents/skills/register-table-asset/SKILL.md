---
name: register-table-asset
description: Use this skill when the task is to register, normalize, and link a thesis or paper table as a tracked manuscript object. Trigger when the user provides a CSV, spreadsheet export, markdown table, LaTeX tabular, summary metrics, or a short description of a planned table and wants it indexed with a stable table ID, stored as a table card, linked to relevant method cards and references, and made available for section packets and Prism drafting. Do not use this skill for full manuscript drafting, raw code interpretation, or packet assembly unless the main task is table registration and linkage.
---

# Purpose

This skill turns a table into a **tracked manuscript table object**.

A tracked table object has:

- a stable table ID
- a clear manuscript role
- a likely chapter/section placement
- a LaTeX label seed
- a table card
- an entry in the central table registry
- links to related method cards when justified
- links to relevant references
- optional links to section packets

The goal is to make tables first-class thesis artifacts rather than loose CSVs, spreadsheet fragments, or ad hoc tabular blocks.

# When to use this skill

Use this skill when the user provides any of the following:

- a CSV path
- a spreadsheet export
- a markdown table
- a LaTeX `tabular` or `table` block
- a set of summary metrics
- a short description of what the table should contain
- optional related method card IDs
- optional target chapter or section

Typical prompts:

- “register this CSV as a tracked table”
- “turn these validation metrics into a table card”
- “index this planned parameter table and link it to the melt-model method cards”
- “I need a table for model comparison in Results”
- “this should become a tracked table for Prism packets”

# When not to use this skill

Do not use this skill when the main task is:

- writing manuscript prose
- assembling a section packet
- interpreting raw code into method cards
- editing table styling or final typography only
- building the final publication-ready LaTeX table layout unless table registration is the main task
- general spreadsheet analysis unrelated to manuscript tracking

If the user mainly needs packet assembly, prefer `build-section-packet`.
If the user mainly needs code interpretation, prefer `codebase-method-cards`.

# Core principle

A table is not just a file or a block of rows and columns.

A table should be treated as a structured manuscript object with:

- provenance
- rhetorical function
- manuscript placement
- traceable links to methods or results
- future reusability in packet building and drafting

# Accepted inputs

This skill can start from any of these:

- local CSV path
- local spreadsheet export path
- markdown table
- LaTeX table or tabular block
- plain-text description of a planned table
- optional target chapter/section
- optional related method card IDs
- optional related reference keys
- optional preferred label or short name

Examples:

- `outputs/analysis/meltout_metrics.csv`
- `tables/drafts/fsm2_validation_summary.md`
- description: “summary of agreement metrics between UE-FSM2 and original Fortran FSM2”
- related method cards: `MC-004`, `MC-012`
- related references: `Hock1999`, `Pellicciotti2005`

# Required workflow

## Step 1 — Inspect the table input

Determine:

- what the source appears to be
- whether the path or provided content is sufficient to reference
- whether the user description is sufficient
- whether this is likely:
  - descriptive
  - parameter
  - analytical
  - comparison
  - appendix_support

Do not overclaim what the table contains if the input is sparse.

## Step 2 — Classify table role

Assign the table one primary category.

Use these categories:

- `descriptive`
  - dataset summaries
  - station lists
  - study-area overviews
- `parameter`
  - model parameters
  - constants
  - calibration choices
- `analytical`
  - validation metrics
  - performance summaries
  - numerical result summaries
- `comparison`
  - model-versus-model comparison
  - feature comparison
  - method comparison
- `appendix_support`
  - long parameter lists
  - extended diagnostics
  - supplementary result summaries

If uncertain, state the uncertainty and choose the most defensible category.

## Step 3 — Infer likely manuscript placement

Suggest the most likely placement:

- Chapter
- Section
- optional subsection

Examples:

- Methods > Study area and dataset
- Methods > Melt models
- Results > FSM2 validation
- Results > Melt-out-date comparison
- Appendix > Extended diagnostics

Placement should follow manuscript logic, not just source file location.

## Step 4 — Assign a stable table ID

Generate a persistent table ID using this pattern:

- `TAB-METH-<NAME>-01`
- `TAB-RES-<NAME>-01`
- `TAB-DISC-<NAME>-01`
- `TAB-APP-<NAME>-01`

Rules:

- use `METH`, `RES`, `DISC`, `INTRO`, or `APP` where appropriate
- use a short descriptive token
- add a two-digit serial suffix
- keep IDs stable once created

Examples:

- `TAB-METH-DATASETS-01`
- `TAB-METH-MODEL-PARAMS-01`
- `TAB-RES-FSM2-VAL-01`
- `TAB-RES-MELTOUT-METRICS-01`

Before creating a new ID, check whether the same table concept and role already appear to be registered. Prefer updating instead of duplicating.

## Step 5 — Create a LaTeX label seed

Generate a manuscript label like:

- `tab:datasets`
- `tab:model_parameters`
- `tab:fsm2_validation`
- `tab:meltout_metrics`

Keep labels concise, stable, and manuscript-friendly.

## Step 6 — Determine table-to-method-card links

If method cards are supplied, or if the table purpose clearly implies a relation, assign one or more relation types:

- `explains`
- `produces`
- `validates`
- `summarizes`
- `supports`

Meaning:

- `explains`: the table clarifies a method or setup
- `produces`: the method card’s workflow generates the table or its data
- `validates`: the table is used to evaluate that method/component
- `summarizes`: the table condenses outputs from the method
- `supports`: the table provides context without being central to validation

Only create links that are defensible from the table description and context.
Do not force links when uncertain.

## Step 7 — Update the registry and cards

Create or update:

- the central table registry
- the individual table card
- linked method cards, if applicable

# Required output files

This skill should write or update the following files.

## 1. Central registry

Preferred path:

`workspace/indexes/tables_index.yaml`

This is the machine-readable source of truth.

## 2. Table card

Preferred path:

`workspace/table_cards/<TABLE_ID>.md`

This is the human-readable table object.

## 3. Method card links

Update relevant files in:

`workspace/method_cards/`

Add or update the `Linked tables` section.

# Table registry schema

Use this structure in `tables_index.yaml`.

```yaml
tables:
  - id: TAB-RES-MELTOUT-METRICS-01
    short_name: meltout-metrics
    manuscript_label: tab:meltout_metrics
    category: analytical
    chapter: Results
    section: Melt-out-date comparison
    subsection: null
    status: registered
    purpose: Summarize agreement metrics between modeled and satellite-derived melt-out dates across model variants.
    source_asset: outputs/analysis/meltout_metrics.csv
    source_type: csv
    source_description: aggregated validation metrics by model and site
    related_method_cards:
      - id: MC-012
        relation: produces
      - id: MC-004
        relation: validates
    related_references:
      - Hock1999
      - Pellicciotti2005
    caption_seed: Summary metrics comparing modeled and remotely sensed melt-out dates across model variants and sites.