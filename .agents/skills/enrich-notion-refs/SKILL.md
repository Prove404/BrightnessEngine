---
name: enrich-notion-refs
description: Identify Notion reference entries missing key categories (like Summary or Authors), fetch their URLs, and use the LLM to extract the missing data.
---

# Enrich Notion References

Use this skill when the user asks to:
- Fill missing categories in the Notion reference database
- Extract metadata (authors, year, summary, tags) from reference URLs

## Principle
Leverage the agent's ability to read URLs and synthesize information, combined with standalone scripts to interact with the Notion API directly. 

## Behavior

### A. Identify Incomplete Entries
1. Run the script `python .agents/skills/enrich-notion-refs/scripts/check_database.py`.
2. This will query the Notion database directly and output a list of incomplete entries (missing Summary, Authors, Year, or Tags) that have a valid URL. The results are printed to the console and saved to `artifacts/incomplete_refs.json`.

### B. Fetch and Extract
1. Read the incomplete entries.
2. For each entry, use your web fetching tools (like `read_url_content`) to read the content of the `url`.
3. Extract the missing metadata. Be concise (e.g., abstract-level summaries, straightforward author lists, precise publication year).
4. Determine the most appropriate `Tag` based on the context. If possible, pick one that already exists in the database. If none fit well, invent a new, concise Tag to assign, but try to avoid overcrowding the database with too many unique tags.

### C. Update Data
1. Use `python .agents/skills/enrich-notion-refs/scripts/update_notion_page.py` to patch the actual Notion page.
   The script expects the `--page_id`, and then optional flags for `--summary`, `--authors`, `--year`, `--tag`, etc.

## Limitations
- Do not make up information without seeing it on the target URL.
- Some URLs may block access (e.g., paywalls or bot protection). If so, add a note to the summary indicating that extraction failed due to access restrictions.
