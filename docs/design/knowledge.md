# Knowledge store

Status (2026-10-18): the store, hybrid search and a measurement are built, and Mobius now reads folders into it (Settings → Knowledge) and can search them. Using it in chat answers, with cited sources, is next.

## Why a database

Notes, project files, saved pages and reference packs only compound if they can be queried together: by words, by meaning, by link,
by where they came from. Files alone make each of those a scan. `core/helix-knowledge` keeps everything in one SQLite file (bundled,
no server). The store is the source of truth; Markdown export and import (an Obsidian-style vault) is planned so nothing is locked in.

## What is built

| Part | Notes |
|---|---|
| `pages` | Markdown body, a stable `key` (path, URL, `note:<id>`), `kind`, `tier`, `pack`, `url`, `license`, created and updated times |
| `chunks` | Passages that follow headings (240 to 1200 characters), each with its title and heading path, an FTS5 row and an optional vector (f32, normalised, tagged with the model that made it) |
| `links` | `[[Title]]`, `[[Title\|label]]` and `[[Title#section]]` in a body. Resolved in either order. Backlinks and outlinks are queries |
| Search | Words (FTS5, porter stemming, title weighted over heading over text) and meaning (cosine over stored vectors), fused by reciprocal rank (k = 60). One passage per page is returned, best first |
| Related | Other pages ranked by the closest pair of passages |
| Provenance | `tier` is `yours`, `reference` or `web`; `pack` groups what one install brought. `delete_pack` and `delete_page` remove passages, vectors and links with them |
| Re-embedding | Saving a page again embeds only passages whose text changed (a hash per passage and model) |

The store holds no model. Callers pass an `Embed` (Mobius will wrap `helix-embed`), and vectors from different models are never compared.

## Measured

`cargo run --release -p helix-knowledge --example measure -- <model dir>` indexes `docs/` (88 pages, 985 passages) and scores 24
questions in `tools/bench/knowledge/questions.json`, written in the asker's words, not the headings'. A question is found at k when
an expected page is among the first k returned. bge-small-en-v1.5, CPU, 4 threads.

| Search | k = 1 | k = 3 | k = 5 |
|---|---|---|---|
| Words only | 13 | 15 | 18 |
| Meaning only | 6 | 13 | 16 |
| **Both, with the bge query prefix** | 12 | **19** | **21** |
| Both, no prefix | 12 | 18 | 19 |

Of 24: hybrid finds 79% in the top 3 and 88% in the top 5, against 63% and 75% for words alone. A query takes under 50 ms including
embedding the question.

What this says:

* **Hybrid is worth having, and neither half is enough.** Words win at rank 1 (exact terms), meaning rescues paraphrases, and the
  fusion gives the best top 3 and top 5. At rank 1 fusion is slightly behind words, so a later pass can weight the title match higher.
* **The query prefix helps a little** (`Represent this sentence for searching relevant passages: `), so the Mobius wrapper uses it.
* **The misses are mostly honest.** Several expected pages lose to `docs/user/*`, which still describes the previous shell (the
  roadmap already lists that audit). The set measures the corpus as it is.
* **Indexing is the cost.** 985 passages took about 3 minutes (about 180 ms each, passages are around 300 tokens), once; unchanged
  passages are free afterwards. That is fine for a project folder and wrong for a reference pack of millions of passages. Before any
  large pack: shorter passages, length-sorted batches, and a smaller or quantised model, each measured.
* **A plain scan is fast enough for now.** Vectors are read once and kept in memory; at 985 passages a scan is far below the time
  to embed the question. `sqlite-vec` is the option when a store passes some hundreds of thousands of passages.

## In Mobius (Settings → Knowledge)

`knowledge.rs` wraps the store. The index is `~/.local/share/helix/knowledge.db`; the folder list is `mobius/knowledge-folders.json`.

* **Add a folder** (system folder picker): text and code files under 300 KB, up to 3000 per folder; hidden folders, `node_modules`, `target`, `dist` and the like, and symlinks are skipped; `/`, the home folder itself, `/tmp`, `/var` and system folders are refused. Reading runs in the background with progress and **Stop**.
* A page is a file (title = its path inside the folder, pack = `folder:<path>`, tier = yours). Reading again costs only changed passages, and pages whose files are gone are dropped.
* **Remove** a folder deletes its pack: pages, passages, vectors and links. The person's files are never touched.
* The same embedding model as the Act hint reads passages and questions (`decide::embedder`); without it files are searchable by words and the page offers the Library download. The first read of a folder before the model exists is words only, and a later read adds the vectors.
* **Try a search** shows what Mobius would find, marking results found by meaning. Everything is local.

Tests: `knowledge::` unit tests (what is collected and skipped, a symlink loop, updating and forgetting, stopping, refused folders) and the nested `mobius-knowledge` (a fake picker, the real embedding model when present).

## Not built

* Using it in chat: retrieved passages given to a local model, and cited-source chips under the reply (next).
* Markdown export and import.
* Facts (a short statement with its source, a confidence and whether the person confirmed it) replacing the memory file.
* An events table (what was retrieved, used and kept) to rank by use.
* Reference packs and fetched pages: each a download the person approves, with licence and size shown. Fetched text is untrusted
  material, quoted and never followed as instructions.
* Proposed links, merges and summaries as reviewed changes.
* The graph view.
