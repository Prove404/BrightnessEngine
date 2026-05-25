---
name: build-section-packet
description: Use this skill when the task is to assemble a bounded writing packet for one thesis or paper section from existing method cards, outline structure, and research notes. Trigger when the user wants to prepare material for Prism or LaTeX drafting, decide what evidence belongs in a section, compress technical notes into a section-ready context, identify missing support, or bridge code-derived documentation with manuscript writing. Do not use this skill for raw code interpretation unless method cards already exist or the task is explicitly about packet assembly.
---

# Purpose

This skill prepares a **section packet**: a compact, section-specific context bundle that a writing system such as Prism can use to draft or revise one manuscript section.

A section packet is not the final prose.
It is the curated bridge between:

- thesis outline
- code-derived method cards
- literature or note summaries
- figure ideas
- open questions
- missing evidence

The goal is to reduce noise and give the writer only the material that belongs in one section.

# When to use this skill

Use this skill when the user wants to:

- prepare one chapter or subsection for drafting
- connect codebase documentation to thesis writing
- decide which method cards belong in a manuscript section
- merge notes and technical summaries into a clean writing packet
- identify what is still missing before drafting
- create bounded context for Prism
- map evidence to a thesis outline

Typical prompts:

- “build a packet for the Methods subsection on forcing preprocessing”
- “assemble the material needed to draft the render target pipeline section”
- “prepare a section packet from these method cards and notes”
- “compress all relevant material for the Results comparison with HORAYZON”

# When not to use this skill

Do not use this skill when the main task is:

- reading raw code and inferring methodology from scratch
- fixing or modifying code
- drafting full chapters directly from the codebase
- generic summarization without a section target
- bibliography management
- formatting LaTeX output itself

If method cards do not yet exist and the task requires code interpretation, prefer the `codebase-method-cards` skill first.

# Core principle

A good packet is not a dump of everything related to a topic.

A good packet is:

- selective
- section-oriented
- explicit about uncertainty
- structured for writing
- small enough to keep context clean
- rich enough to support drafting without reopening the whole project

# Inputs

A section packet may be built from some or all of the following:

- thesis outline or section identifier
- existing method cards
- note summaries
- literature notes
- meeting notes
- figure notes
- result summaries
- open questions
- prior draft fragments
- manuscript context from neighboring sections

# Required workflow

## Step 1 — Identify the target section clearly

Determine the exact manuscript target.

Prefer one of:

- chapter
- section
- subsection
- result block
- discussion theme

Always anchor the packet to a specific writing target such as:

- Methods > Forcing data postprocessing
- Methods > UE radiation-index workflow
- Results > SWdir factor comparison with HORAYZON
- Discussion > limitations of snow redistribution spatialisation

If the target is broad, narrow it before assembling the packet.

## Step 2 — Gather candidate materials

Collect all inputs that might support that section.

Candidates may include:

- method cards tagged to the target section
- notes or summaries mentioning the same method or comparison
- figure ideas tied to the section
- outputs or diagnostics linked to the relevant workflow
- open issues that affect wording or interpretation

Do not assume all gathered material belongs in the final packet.

## Step 3 — Filter by section intent

Decide what belongs based on the rhetorical function of the section.

Examples:

### Methods
Include:
- implementation logic
- workflow steps
- inputs/outputs
- assumptions
- parameterizations
- reproducibility details

Exclude:
- strong evaluative claims
- interpretation of performance unless needed to justify design

### Results
Include:
- what was compared
- what was found
- metrics or outputs
- figure/table references
- observed patterns

Exclude:
- long implementation detail unless needed to understand the comparison

### Discussion
Include:
- limitations
- implications
- uncertainty
- interpretation
- what could be improved

Exclude:
- detailed implementation narration already covered in Methods

## Step 4 — Prioritize the most relevant material

Rank items into:

- primary
- secondary
- background
- exclude

Primary material should be sufficient for first drafting.
Secondary material can remain in a “use if needed” block.
Excluded material should not be copied into the packet.

## Step 5 — Identify missing support

Look for missing elements such as:

- absent literature grounding
- unclear terminology
- unsupported claims
- unresolved implementation ambiguity
- figure or table not yet defined
- missing validation details
- open choice between Methods and Discussion placement

Explicitly flag these instead of filling gaps with speculation.

## Step 6 — Write the packet in the required format

Use the section packet template below.

# Section packet format

Use this exact structure.

## Section Packet: <section title>

**Target placement**
- Chapter: <chapter name>
- Section: <section name>
- Subsection: <subsection name if applicable>
- Section ID: <optional manuscript ID>

**Section purpose**
- <1–3 sentences stating what this section must accomplish in the manuscript>

**Drafting guidance**
- <tone, scope, exclusions, and rhetorical constraints>
- <e.g. descriptive not evaluative, concise, methods-focused, no unsupported claims>

**Primary source materials**
- <list of method cards, notes, summaries, or outputs that should drive the draft>

**Core points that must appear**
- <bullet list of essential content>
- <only include points that are truly central>

**Useful secondary points**
- <details worth mentioning if space or flow allows>

**What to avoid**
- <things that do not belong in this section>
- <implementation noise, repeated theory, unsupported claims, etc.>

**Known assumptions**
- <assumptions that shape wording or interpretation>

**Known limitations or uncertainties**
- <points that may need cautious phrasing or later confirmation>

**Linked figures / tables / outputs**
- <figure ideas, diagnostics, exported rasters, CSVs, plots, tables>

**Open questions**
- <unresolved issues that the writer should not silently invent around>

**Packet body**
- <2–8 short subsections of curated content, organized for drafting>
- <these can be titled such as Workflow, Inputs, Assumptions, Validation setup>

**Suggested subsection skeleton**
- <very short proposed prose structure for the eventual section>

**Readiness**
- Draftable: yes/no
- Why: <brief reason>

# What belongs in the packet body

The packet body should contain compressed, section-ready material such as:

- distilled prose from method cards
- short paraphrases of relevant notes
- careful synthesis across multiple inputs
- explicit mention of uncertainties
- local terminology that should remain consistent in drafting

Do not paste huge blocks of source material unless necessary.
Compress aggressively, but do not erase nuance.

# Output modes

## Mode A — Standard packet
For one section with clear inputs and moderate complexity.

## Mode B — Comparative packet
For results/discussion sections involving comparison across models, datasets, or references.

Include extra headings:
- Comparison target
- Metrics / diagnostics
- What counts as agreement or mismatch
- Caveats in comparability

## Mode C — Methods packet
For implementation-heavy methodological sections.

Emphasize:
- workflow
- assumptions
- parameterizations
- reproducibility
- output definitions

## Mode D — Discussion packet
For interpretation-heavy sections.

Emphasize:
- limitations
- consequences
- alternative explanations
- links to broader literature
- future work

# Selection rules

## Prefer section logic over source logic
Organize the packet according to manuscript needs, not according to folder or file structure.

## Keep packet size bounded
Default target:
- 600 to 1800 words for normal sections
- smaller if the section is narrow
- larger only for dense methods sections

If there is too much material, prioritize and defer rather than dumping everything.

## Be explicit about uncertainty
If something is unclear, write:
- “unclear from current materials”
- “needs confirmation”
- “belongs in discussion more than methods”
- “literature support still missing”

Do not fabricate coherence.

## Preserve provenance lightly
When listing source materials, name the card/note/output clearly enough that the user can trace it back.

# Relationship to other skills

## Upstream
This skill often depends on:
- `codebase-method-cards`

## Downstream
Its outputs are intended for:
- Prism drafting
- LaTeX section drafting
- manual chapter writing
- section planning and revision

# File output rules

When saving output files:

- save one packet per target section
- write to `workspace/section_packets/`
- use manuscript-oriented filenames
- do not overwrite unrelated packets
- update existing packets only when new source material materially changes the section context

Recommended filename pattern:

`<chapter>_<section>_<subsection>.md`

Examples:

- `methods_forcing_postprocessing.md`
- `methods_ue_radiation_indices.md`
- `results_horayzon_swfactor_comparison.md`
- `discussion_snow_redistribution_limitations.md`

# Recommended packet assembly process

When possible, follow this order:

1. read the target outline section
2. collect matching method cards
3. collect matching notes or summaries
4. classify material by relevance
5. draft the packet
6. flag gaps
7. save packet to `workspace/section_packets/`

# Quality bar

A good packet should make the following possible:

A writer or AI assistant should be able to draft the target section without:
- reopening the whole repo
- rereading all notes
- guessing what matters
- silently inventing missing support

# Definition of done

This skill is complete when:

- the packet targets one clear manuscript section
- central source materials have been selected
- irrelevant material has been excluded
- core points are explicit
- limitations and missing support are flagged
- the packet is ready to hand off to Prism or to a human writer

# Optional closing section

When useful, end with:

## Suggested next action
- draft in Prism
- request more method cards
- add missing literature note
- split the section into two packets
- defer unresolved claim to Discussion