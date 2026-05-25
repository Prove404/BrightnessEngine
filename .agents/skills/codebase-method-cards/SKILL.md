---
name: codebase-method-cards
description: Use this skill when the task is to read a scientific or technical codebase and convert relevant modules, scripts, classes, configs, and pipelines into structured method cards for thesis writing, documentation, or section drafting. Trigger when the user asks to explain what the code does scientifically, map code to manuscript methods, summarize modules into thesis-ready notes, extract assumptions/inputs/outputs/limitations, or prepare material for LaTeX or papers. Do not use this skill for bug fixing, feature implementation, refactoring, or generic code review unless the main goal is documentation of scientific or methodological behavior.
---

# Purpose

This skill turns code into **method cards** that can be reused for thesis writing, paper drafting, figure captions, methods sections, appendix documentation, and discussion of assumptions or limitations.

A method card is a compact, structured description of one meaningful code unit such as:

- a module
- a class
- a script
- a pipeline stage
- a model component
- a figure-generation routine
- a preprocessing step
- a validation or export workflow

The skill is optimized for scientific and technical projects where the code embodies the actual methodology.

# When to use this skill

Use this skill when the user wants any of the following:

- “turn the codebase into method cards”
- “summarize the methodology from the repo”
- “explain what this module does for the thesis”
- “map the codebase to my Methods chapter”
- “extract assumptions, inputs, outputs, and limitations”
- “prepare section packets for writing”
- “describe how figures/results are produced”
- “help me document the plugin / pipeline / simulation framework”

Do not use this skill when the main task is:

- fixing bugs
- adding features
- improving performance
- linting/style cleanup
- generic architecture review
- producing end-user tutorials unrelated to scientific method extraction

# Core principle

Do not paraphrase code vaguely.

Always aim to reconstruct:

1. the **scientific or technical role** of the code,
2. the **place of the code in the larger workflow**,
3. the **assumptions and simplifications** embedded in it,
4. the **inputs, outputs, and diagnostics** it exposes,
5. the **most natural manuscript section** where it belongs.

# Units of analysis

Prefer documenting units that are meaningful for the manuscript, not necessarily only by file boundary.

Good units:

- “direct/diffuse forcing preprocessing”
- “terrain radiation index capture pipeline”
- “snow redistribution parametrisation”
- “FSM2 wrapper and runtime coupling”
- “melt-out date export logic”
- “reference-strip normalization workflow”

Less useful units:

- tiny helpers with no methodological significance
- boilerplate engine setup
- trivial getters/setters
- low-level utilities unless they materially affect scientific behavior

# Reference structure

If you need a reference for the manuscript structure (e.g., to accurately map cards to specific methods or results sections), you can use the **MCP Notion tool** to access the **Thesis/Index** page in Notion. This index contains the live outline and chapter headings to help you determine the "Likely manuscript placement".

# Required workflow

## Step 1 — Identify the relevant code units

Read the user task and inspect the codebase to identify the modules or routines that matter most for methodological understanding.

Prioritize units that:

- implement core model logic
- transform scientific inputs
- compute physically meaningful quantities
- generate outputs used in analysis
- drive figures/tables/diagnostics
- encode assumptions or approximations
- connect external data to simulation/runtime systems

## Step 2 — Group code by methodological role

Before writing cards, organize findings into categories such as:

- data ingestion
- preprocessing
- model core
- radiation calculations
- rendering/capture workflow
- spatialisation
- validation/comparison
- export/diagnostics
- visualization outputs

Use these categories only as internal scaffolding. The output should still be individual method cards.

## Step 3 — Read for behavior, not only syntax

For each candidate unit, determine:

- what problem it solves
- when it runs
- what data it consumes
- what it produces
- what assumptions are hard-coded or implied
- what external dependencies matter
- whether the implementation is conceptual, empirical, approximate, or physically based
- whether the code is central, supportive, or optional for the thesis narrative

Do not infer behavior recklessly. If uncertain, say so clearly.

## Step 4 — Write one method card per meaningful unit

Each method card must use the format below.

# Method card format

Use this exact structure.

## Method Card: <short title>

**Code unit**
- File(s): <path(s)>
- Main symbol(s): <class/function/script names if applicable>

**Role in workflow**
- <1–3 sentences explaining where this sits in the pipeline>

**What it does**
- <clear prose description of the implemented behavior>
- <focus on scientific or methodological function, not line-by-line code commentary>

**Inputs**
- <list of primary inputs>
- <include units/formats if obvious and relevant>

**Outputs**
- <list of primary outputs>
- <include derived fields, maps, tables, exports, diagnostics>

**Assumptions and simplifications**
- <explicit assumptions>
- <approximations, parameterizations, defaults, omitted processes>

**Important implementation details**
- <details that materially affect interpretation>
- <normalization, thresholds, coordinate handling, interpolation, pass structure, etc.>

**Sources of uncertainty or limitation**
- <what could bias results or reduce generality>

**Validation relevance**
- <how this unit could be checked, benchmarked, or compared to reference data/models>

**Likely manuscript placement**
- Chapter: <e.g. Methods / Results / Discussion / Appendix>
- Section: <best-fit section title>

**Thesis-ready summary**
- <one paragraph that could be reused or lightly edited for the thesis>

**Open questions**
- <points that remain ambiguous after reading the code>

# Quality bar for cards

Each card should be:

- specific enough to be reusable in writing
- short enough to scan quickly
- honest about uncertainty
- tied to actual files and symbols
- framed in terms of method, not just software engineering

Avoid:

- fluff
- generic “this class manages data” language
- repeating variable names without interpretation
- pretending the code proves something it only approximates

# Two-layer output structure

The skill outputs must be structured in two layers to create bounded context bundles for the manuscript:

## Layer 1: Raw skill products (Method Cards)

These are the direct outputs of the skill, representing atomic code units.

Example folder structure:
```text
thesis/
  workspace/
    method_cards/
      01_forcing_preprocessing.md
      02_hock_model3.md
      03_pellicciotti_modeld.md
      04_fsm2_wrapper.md
      05_render_target_pipeline.md
      06_reference_strip_normalization.md
      07_snow_redistribution.md
      08_output_exports.md
```

- Each file should describe **one meaningful unit only**.
- Do not mix multiple major systems into one card.

## Layer 2: Section Packets

Then construct a second layer that groups those method cards by manuscript section. These are assembled files, not primary files.

Example folder structure:
```text
thesis/
  workspace/
    section_packets/
      methods_study_area_and_data.md
      methods_forcing_preprocessing.md
      methods_melt_models.md
      methods_ue_radiation_pipeline.md
      methods_spatialisation_and_redistribution.md
      methods_outputs_and_diagnostics.md
      results_validation_design.md
```

Each section packet should contain:
- Section title
- Relevant method cards
- Linked literature notes from Notion
- Missing pieces
- Questions needing manual writing

Concept summary:
- `method_cards/` = atomic units
- `section_packets/` = writing context bundles

This two-layer approach ensures bounded contexts and makes the later stages of writing much more effective.

# Special rules for scientific codebases

## Distinguish implementation from scientific claim
Code may implement a method without validating it. Do not confuse:
- “the code computes X”
with
- “X is accurate”

## Separate physics from rendering shortcuts
If the project mixes physical modeling and graphics/engine systems, identify clearly:
- physically motivated components
- engine-dependent approximations
- visualization-only logic
- analysis-grade outputs vs presentation outputs

## Flag hidden assumptions
Pay special attention to:
- constants
- thresholds
- empirical coefficients
- interpolation choices
- clamping
- normalization references
- coordinate/projection conversions
- missing-data fallbacks
- temporal aggregation choices
- default material/render settings if they affect interpretation

## Track provenance of figures and diagnostics
If a unit contributes directly to a figure, raster, table, or CSV used in analysis, say so explicitly.

# Recommended process when the repo is large

If the codebase is large:

1. identify top-level folders and major modules
2. shortlist only thesis-relevant units
3. write cards for the top 5–10 units first
4. mark lower-priority areas as deferred

Do not try to document everything uniformly.

# Output style

Write in precise technical prose.

Prefer:
- “computes”
- “transforms”
- “parameterizes”
- “normalizes”
- “exports”
- “couples”
- “approximates”
- “propagates”

Avoid:
- “basically”
- “just”
- “simply” unless truly warranted
- inflated claims
- vague software jargon

# Definition of done

This skill is complete when:

- the major thesis-relevant code units have method cards,
- each card explains role, inputs, outputs, assumptions, and limitations,
- each card maps to a likely manuscript section,
- the output is useful for drafting methods/results/discussion text without rereading the whole codebase.

# Optional closing section

When helpful, end with:

## Suggested next writing actions
- <which card to turn into prose first>
- <which section lacks literature support>
- <which code unit likely needs manual explanation from the user>
