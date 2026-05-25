---
name: register-equation
description: Use this skill when the task is to register, normalize, and link a thesis or paper equation as a tracked manuscript object. Trigger when the user provides an equation, a LaTeX expression, a short description, or an implementation formula and wants it stored as an equation card, indexed with a stable equation ID, linked to relevant method cards and references, and made available for section packets and Prism drafting. Do not use this skill for full manuscript drafting, raw code interpretation, or packet assembly unless the main task is equation registration and linkage.
---

# Purpose

This skill turns an equation into a **tracked manuscript equation object**.

A tracked equation object has:

- a stable equation ID
- a canonical LaTeX form
- a clear manuscript role
- symbol definitions
- links to relevant method cards
- links to relevant references
- optional links to section packets
- notes about implementation form versus manuscript form

The goal is to keep equations reusable and consistent across the thesis instead of rewriting them ad hoc in multiple places.

# When to use this skill

Use this skill when the user wants to:

- register a new equation for the thesis
- normalize an equation into canonical LaTeX
- connect an implementation formula to a manuscript-safe form
- link an equation to one or more method cards
- attach references to an equation
- make an equation available for section packets and Prism drafting
- avoid notation drift across sections

Typical prompts:

- “register this Hock melt equation”
- “turn this formula into an equation card”
- “link this equation to the melt-model method card”
- “store the Rec.709 luminance conversion as a tracked equation”
- “normalize this implementation expression for thesis writing”

# When not to use this skill

Do not use this skill when the main task is:

- drafting the full Methods or Results section
- interpreting raw code into method cards from scratch
- deciding which equations belong in a section packet
- proving or deriving equations in full unless registration is the main goal
- solving equations symbolically for analysis unrelated to manuscript tracking

If the main task is section-level rhetorical selection, prefer `build-section-packet`.
If the main task is code interpretation, prefer `codebase-method-cards`.

# Core principle

An equation is not just a string of math.

A useful thesis equation should have:

- one canonical representation
- stable notation
- clear meaning
- traceable provenance
- explicit links to methods and sources
- a clear decision about whether it belongs in the main text, appendix, or only in notes

# Accepted inputs

This skill can start from any of these:

- LaTeX equation text
- plain-text formula
- equation screenshot or transcription supplied by the user
- short verbal description of the equation
- implementation expression from code
- optional method card IDs
- optional reference keys
- optional target chapter or section

Examples:

- `M = (MF + a_{snow/ice} I) T`
- `Y = 0.2126 R + 0.7152 G + 0.0722 B`
- “Pellicciotti-style albedo decay parameterization”
- “UE normalized radiation index as local over reference luminance”
- related method card: `MC-006`
- related reference: `Hock1999`

# Required workflow

## Step 1 — Identify the equation role

Determine what kind of equation this is.

Use one primary type:

- `defining`
- `parameterization`
- `diagnostic`
- `implementation`
- `derived_metric`

Guidance:

- `defining`: core model equation that defines the method
- `parameterization`: empirical or semi-empirical closure or fitted relation
- `diagnostic`: equation used mainly for analysis or reporting
- `implementation`: equation or transform mainly tied to software execution
- `derived_metric`: index or normalized quantity derived from other terms

If uncertain, choose the most defensible type and note the uncertainty.

## Step 2 — Infer likely manuscript placement

Suggest the most likely placement:

- Chapter
- Section
- optional subsection

Examples:

- Methods > Melt models > Hock Model 3
- Methods > UE radiation-index workflow
- Theoretical background > Degree-day models
- Appendix > Diagnostic metrics

Placement should follow manuscript logic, not just where the formula first appeared.

## Step 3 — Assign a stable equation ID

Generate a persistent equation ID using this pattern:

- `EQ-METH-<NAME>-01`
- `EQ-THEORY-<NAME>-01`
- `EQ-RES-<NAME>-01`
- `EQ-APP-<NAME>-01`

Rules:

- use `METH`, `THEORY`, `RES`, `DISC`, or `APP` where appropriate
- use a short descriptive token
- add a two-digit serial suffix
- keep IDs stable once created

Examples:

- `EQ-METH-HOCK-M3-01`
- `EQ-METH-PEL-ALB-01`
- `EQ-METH-LUM-REC709-01`
- `EQ-METH-RUE-NORM-01`

Before creating a new ID, check whether the same conceptual equation is already registered. Prefer updating rather than duplicating.

## Step 4 — Normalize to a canonical LaTeX form

Create a clean manuscript-facing LaTeX version of the equation.

Rules:

- preserve the mathematical meaning
- prefer conventional notation when known
- avoid implementation-only variable names if a cleaner manuscript notation exists
- keep the code-facing notation in notes if it matters
- do not silently change the scientific meaning
- if the formula is ambiguous, preserve the ambiguity in notes rather than inventing a polished but false expression

## Step 5 — Define symbols

For each important symbol, add a short definition.

Include:

- symbol
- meaning
- optional unit if relevant
- any notation ambiguity that matters for writing

Do not over-document trivial symbols if they are obvious from context.

## Step 6 — Link to references and method cards

When relevant information is available, update or create links to:

- method cards in `workspace/method_cards/`
- references in `workspace/reference_cards/` or `refs.bib`
- section packets in `workspace/section_packets/` if explicitly requested or clearly useful

Only create links that are defensible.

## Step 7 — Create or update the equation card and index

Create or update:

- the equation card
- the equation registry/index
- relevant method cards

# Required output files

This skill should create or update:

## 1. Equation card

Preferred path:

`workspace/equation_cards/<EQUATION_ID>.md`

This is the human-readable equation object.

## 2. Equation registry

Preferred path:

`workspace/indexes/equations_index.yaml`

This is the machine-readable source of truth.

## 3. Linked method cards

When appropriate, update:

`workspace/method_cards/*.md`

Add or update the `Relevant equations` section.

## 4. Optional linked section packets

When explicitly requested or clearly useful, update:

`workspace/section_packets/*.md`

Use equation IDs, not duplicated full equations, unless the packet specifically benefits from the full display form.

# Equation registry schema

Use this structure in `workspace/indexes/equations_index.yaml`.

```yaml
equations:
  - id: EQ-METH-LUM-REC709-01
    short_name: rec709-luminance
    latex_label: eq:rec709_luminance
    type: implementation
    chapter: Methods
    section: UE radiation-index workflow
    subsection: RGB to relative luminance
    status: registered
    purpose: Convert RGB scene-capture values to relative luminance using Rec.709 coefficients.
    canonical_latex: "Y = 0.2126R + 0.7152G + 0.0722B"
    related_method_cards:
      - id: MC-006
        relation: used_by
    related_references: []