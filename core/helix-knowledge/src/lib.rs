//! A local knowledge store in one SQLite file.
//!
//! Pages (Markdown, with a title and provenance) are split into passages. Each passage is searchable by words (FTS5) and, when an
//! [`Embed`] is supplied, by meaning (a stored vector). [`Store::search`] fuses both rankings. Links between pages are data:
//! `[[Title]]` in a page's body becomes a link, and backlinks and "related by meaning" come from queries, not parsing.
//!
//! Every page records where it came from: its [`Tier`] (yours, reference, web), a `pack` it was installed with, a URL and a licence.
//! Deleting a page or a whole pack removes its passages, vectors and links with it.

mod chunk;

use std::collections::HashMap;
use std::sync::Mutex;
use std::time::{SystemTime, UNIX_EPOCH};

use rusqlite::{params, params_from_iter, Connection, OptionalExtension};

pub use chunk::{chunk, Passage, MAX_CHARS, MIN_CHARS};

pub type Result<T> = std::result::Result<T, String>;

fn sql(error: rusqlite::Error) -> String {
    error.to_string()
}

const SCHEMA_VERSION: i64 = 1;

const SCHEMA: &str = "
CREATE TABLE pages(
    id      INTEGER PRIMARY KEY,
    key     TEXT NOT NULL UNIQUE,
    title   TEXT NOT NULL,
    body    TEXT NOT NULL,
    kind    TEXT NOT NULL DEFAULT 'note',
    tier    TEXT NOT NULL DEFAULT 'yours',
    pack    TEXT,
    url     TEXT,
    license TEXT,
    hash    TEXT NOT NULL,
    created INTEGER NOT NULL,
    updated INTEGER NOT NULL
);
CREATE INDEX pages_pack ON pages(pack);
CREATE INDEX pages_title ON pages(lower(title));
CREATE TABLE chunks(
    id        INTEGER PRIMARY KEY,
    page_id   INTEGER NOT NULL REFERENCES pages(id) ON DELETE CASCADE,
    seq       INTEGER NOT NULL,
    title     TEXT NOT NULL,
    heading   TEXT NOT NULL,
    text      TEXT NOT NULL,
    hash      TEXT NOT NULL,
    model     TEXT,
    embedding BLOB
);
CREATE INDEX chunks_page ON chunks(page_id);
CREATE VIRTUAL TABLE chunks_fts USING fts5(
    title, heading, text, content='chunks', content_rowid='id', tokenize='porter unicode61'
);
CREATE TRIGGER chunks_ai AFTER INSERT ON chunks BEGIN
    INSERT INTO chunks_fts(rowid, title, heading, text) VALUES (new.id, new.title, new.heading, new.text);
END;
CREATE TRIGGER chunks_ad AFTER DELETE ON chunks BEGIN
    INSERT INTO chunks_fts(chunks_fts, rowid, title, heading, text) VALUES ('delete', old.id, old.title, old.heading, old.text);
END;
CREATE TABLE links(
    from_id INTEGER NOT NULL REFERENCES pages(id) ON DELETE CASCADE,
    target  TEXT NOT NULL,
    to_id   INTEGER REFERENCES pages(id) ON DELETE SET NULL,
    origin  TEXT NOT NULL DEFAULT 'written',
    PRIMARY KEY (from_id, target)
);
CREATE INDEX links_to ON links(to_id);
";

/// Where a page's knowledge comes from, and so how much to trust and how to show it.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum Tier {
    /// Written or saved by the person: notes, chats, their files.
    Yours,
    /// An installed reference pack, such as an offline encyclopedia or documentation.
    Reference,
    /// A page fetched from the web. Untrusted text: quoted as material, never followed as instructions.
    Web,
}

impl Tier {
    pub fn as_str(self) -> &'static str {
        match self {
            Tier::Yours => "yours",
            Tier::Reference => "reference",
            Tier::Web => "web",
        }
    }

    fn parse(text: &str) -> Tier {
        match text {
            "reference" => Tier::Reference,
            "web" => Tier::Web,
            _ => Tier::Yours,
        }
    }
}

/// Turns text into vectors. The store holds no model itself.
pub trait Embed {
    /// Names the model, so vectors from different models are never compared.
    fn model_id(&self) -> &str;
    fn embed(&self, texts: &[&str]) -> Result<Vec<Vec<f32>>>;
    /// A search query may need a different framing than a passage does.
    fn embed_query(&self, query: &str) -> Result<Vec<f32>> {
        self.embed(&[query])?
            .into_iter()
            .next()
            .ok_or_else(|| "no vector".to_string())
    }
}

#[derive(Debug, Clone)]
pub struct NewPage {
    /// A stable identity (a path, a URL, `note:<id>`), so saving the same thing again updates it.
    pub key: String,
    pub title: String,
    pub body: String,
    pub kind: String,
    pub tier: Tier,
    pub pack: Option<String>,
    pub url: Option<String>,
    pub license: Option<String>,
}

impl NewPage {
    pub fn note(key: &str, title: &str, body: &str) -> NewPage {
        NewPage {
            key: key.to_string(),
            title: title.to_string(),
            body: body.to_string(),
            kind: "note".to_string(),
            tier: Tier::Yours,
            pack: None,
            url: None,
            license: None,
        }
    }
}

#[derive(Debug, Clone)]
pub struct Page {
    pub id: i64,
    pub key: String,
    pub title: String,
    pub body: String,
    pub kind: String,
    pub tier: Tier,
    pub pack: Option<String>,
    pub url: Option<String>,
    pub license: Option<String>,
    pub created: i64,
    pub updated: i64,
}

/// What [`Store::put_page`] did.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Put {
    pub id: i64,
    pub changed: bool,
    /// Passages that needed a new vector. Unchanged passages keep theirs.
    pub embedded: usize,
}

#[derive(Debug, Clone)]
pub struct Hit {
    pub page_id: i64,
    pub key: String,
    pub title: String,
    pub tier: Tier,
    pub heading: String,
    pub text: String,
    pub score: f32,
    /// Position in the word ranking and in the meaning ranking (0 is best), when the passage was in each.
    pub text_rank: Option<usize>,
    pub vector_rank: Option<usize>,
    /// Cosine similarity to the query, when the meaning search ran.
    pub similarity: Option<f32>,
}

#[derive(Debug, Clone)]
pub struct SearchOptions {
    pub limit: usize,
    /// Empty means every tier.
    pub tiers: Vec<Tier>,
    /// How many passages each ranking contributes before they are fused.
    pub candidates: usize,
}

impl Default for SearchOptions {
    fn default() -> Self {
        SearchOptions {
            limit: 8,
            tiers: Vec::new(),
            candidates: 50,
        }
    }
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct LinkRef {
    pub id: i64,
    pub title: String,
}

#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct Stats {
    pub pages: usize,
    pub chunks: usize,
    pub embedded: usize,
    pub yours: usize,
    pub reference: usize,
    pub web: usize,
}

struct VectorEntry {
    chunk: i64,
    page: i64,
    tier: Tier,
    vector: Vec<f32>,
}

pub struct Store {
    conn: Mutex<Connection>,
    /// Every vector for one model, read once and kept until a write changes any passage.
    vectors: Mutex<Option<(String, Vec<VectorEntry>)>>,
}

impl Store {
    pub fn open(path: &std::path::Path) -> Result<Store> {
        if let Some(parent) = path.parent() {
            std::fs::create_dir_all(parent).map_err(|e| e.to_string())?;
        }
        Store::with_connection(Connection::open(path).map_err(sql)?)
    }

    pub fn in_memory() -> Result<Store> {
        Store::with_connection(Connection::open_in_memory().map_err(sql)?)
    }

    fn with_connection(conn: Connection) -> Result<Store> {
        conn.execute_batch(
            "PRAGMA foreign_keys = ON; PRAGMA journal_mode = WAL; PRAGMA synchronous = NORMAL;",
        )
        .map_err(sql)?;
        let version: i64 = conn
            .query_row("PRAGMA user_version", [], |row| row.get(0))
            .map_err(sql)?;
        if version == 0 {
            conn.execute_batch(SCHEMA).map_err(sql)?;
            conn.execute_batch(&format!("PRAGMA user_version = {SCHEMA_VERSION}"))
                .map_err(sql)?;
        } else if version != SCHEMA_VERSION {
            return Err(format!(
                "knowledge store is version {version}, this build reads {SCHEMA_VERSION}"
            ));
        }
        Ok(Store {
            conn: Mutex::new(conn),
            vectors: Mutex::new(None),
        })
    }

    fn lock(&self) -> Result<std::sync::MutexGuard<'_, Connection>> {
        self.conn
            .lock()
            .map_err(|_| "store lock poisoned".to_string())
    }

    fn forget_vectors(&self) {
        if let Ok(mut cache) = self.vectors.lock() {
            *cache = None;
        }
    }

    /// Saves a page, replacing the one with the same key. Only passages whose text changed are embedded again.
    pub fn put_page(&self, page: &NewPage, embedder: Option<&dyn Embed>) -> Result<Put> {
        let hash = fingerprint(&[&page.title, &page.body, &page.kind, page.tier.as_str()]);
        let (existing, previous) = {
            let conn = self.lock()?;
            let existing: Option<(i64, String)> = conn
                .query_row(
                    "SELECT id, hash FROM pages WHERE key = ?1",
                    [&page.key],
                    |row| Ok((row.get(0)?, row.get(1)?)),
                )
                .optional()
                .map_err(sql)?;
            if let Some((id, old)) = &existing {
                if *old == hash {
                    let unchanged_meta: bool = conn
                        .query_row(
                            "SELECT pack IS ?2 AND url IS ?3 AND license IS ?4 FROM pages WHERE id = ?1",
                            params![id, page.pack, page.url, page.license],
                            |row| row.get(0),
                        )
                        .map_err(sql)?;
                    let wanted = embedder.map(|e| e.model_id().to_string());
                    let embedded_enough = match &wanted {
                        None => true,
                        Some(model) => {
                            let missing: i64 = conn
                                .query_row(
                                    "SELECT COUNT(*) FROM chunks WHERE page_id = ?1 AND (model IS NOT ?2 OR embedding IS NULL)",
                                    params![id, model],
                                    |row| row.get(0),
                                )
                                .map_err(sql)?;
                            missing == 0
                        }
                    };
                    if unchanged_meta && embedded_enough {
                        return Ok(Put {
                            id: *id,
                            changed: false,
                            embedded: 0,
                        });
                    }
                }
            }
            // vectors already stored for this page's passages, by passage hash, so unchanged passages are not embedded again
            let mut previous: HashMap<(String, String), Vec<u8>> = HashMap::new();
            if let Some((id, _)) = &existing {
                let mut stmt = conn
                    .prepare("SELECT hash, model, embedding FROM chunks WHERE page_id = ?1 AND embedding IS NOT NULL")
                    .map_err(sql)?;
                let rows = stmt
                    .query_map([id], |row| {
                        Ok((
                            row.get::<_, String>(0)?,
                            row.get::<_, String>(1)?,
                            row.get::<_, Vec<u8>>(2)?,
                        ))
                    })
                    .map_err(sql)?;
                for row in rows {
                    let (hash, model, blob) = row.map_err(sql)?;
                    previous.insert((hash, model), blob);
                }
            }
            (existing.map(|(id, _)| id), previous)
        };

        let passages = chunk(&page.body);
        let hashes: Vec<String> = passages
            .iter()
            .map(|p| fingerprint(&[&page.title, &p.heading, &p.text]))
            .collect();
        let model = embedder.map(|e| e.model_id().to_string());
        let mut blobs: Vec<Option<Vec<u8>>> = vec![None; passages.len()];
        let mut embedded = 0;
        if let (Some(embedder), Some(model)) = (embedder, &model) {
            let mut missing = Vec::new();
            for (index, hash) in hashes.iter().enumerate() {
                match previous.get(&(hash.clone(), model.clone())) {
                    Some(blob) => blobs[index] = Some(blob.clone()),
                    None => missing.push(index),
                }
            }
            for batch in missing.chunks(16) {
                let inputs: Vec<String> = batch
                    .iter()
                    .map(|i| passage_input(&page.title, &passages[*i]))
                    .collect();
                let refs: Vec<&str> = inputs.iter().map(String::as_str).collect();
                let vectors = embedder.embed(&refs)?;
                if vectors.len() != batch.len() {
                    return Err("embedder returned the wrong number of vectors".to_string());
                }
                for (slot, vector) in batch.iter().zip(vectors) {
                    blobs[*slot] = Some(encode(&normalized(vector)));
                    embedded += 1;
                }
            }
        }

        let mut conn = self.lock()?;
        let tx = conn.transaction().map_err(sql)?;
        let now = now();
        let id = match existing {
            Some(id) => {
                tx.execute(
                    "UPDATE pages SET title=?2, body=?3, kind=?4, tier=?5, pack=?6, url=?7, license=?8, hash=?9, updated=?10 WHERE id=?1",
                    params![id, page.title, page.body, page.kind, page.tier.as_str(), page.pack, page.url, page.license, hash, now],
                )
                .map_err(sql)?;
                tx.execute("DELETE FROM chunks WHERE page_id = ?1", [id])
                    .map_err(sql)?;
                tx.execute(
                    "DELETE FROM links WHERE from_id = ?1 AND origin = 'written'",
                    [id],
                )
                .map_err(sql)?;
                id
            }
            None => {
                tx.execute(
                    "INSERT INTO pages(key, title, body, kind, tier, pack, url, license, hash, created, updated)
                     VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?10)",
                    params![page.key, page.title, page.body, page.kind, page.tier.as_str(), page.pack, page.url, page.license, hash, now],
                )
                .map_err(sql)?;
                tx.last_insert_rowid()
            }
        };
        for (index, passage) in passages.iter().enumerate() {
            tx.execute(
                "INSERT INTO chunks(page_id, seq, title, heading, text, hash, model, embedding) VALUES (?1,?2,?3,?4,?5,?6,?7,?8)",
                params![
                    id,
                    index as i64,
                    page.title,
                    passage.heading,
                    passage.text,
                    hashes[index],
                    blobs[index].as_ref().and(model.as_deref()),
                    blobs[index],
                ],
            )
            .map_err(sql)?;
        }
        for target in wikilinks(&page.body) {
            tx.execute(
                "INSERT OR IGNORE INTO links(from_id, target, to_id, origin)
                 VALUES (?1, ?2, (SELECT id FROM pages WHERE lower(title) = lower(?2) AND id != ?1 LIMIT 1), 'written')",
                params![id, target],
            )
            .map_err(sql)?;
        }
        // links written earlier to a title this page now answers to
        tx.execute(
            "UPDATE links SET to_id = ?1 WHERE to_id IS NULL AND lower(target) = lower(?2) AND from_id != ?1",
            params![id, page.title],
        )
        .map_err(sql)?;
        tx.commit().map_err(sql)?;
        drop(conn);
        self.forget_vectors();
        Ok(Put {
            id,
            changed: true,
            embedded,
        })
    }

    pub fn page_by_key(&self, key: &str) -> Result<Option<Page>> {
        let conn = self.lock()?;
        conn.query_row(PAGE_SELECT_KEY, [key], page_row)
            .optional()
            .map_err(sql)
    }

    pub fn page(&self, id: i64) -> Result<Option<Page>> {
        let conn = self.lock()?;
        conn.query_row(PAGE_SELECT_ID, [id], page_row)
            .optional()
            .map_err(sql)
    }

    /// Looks a page up by title, ignoring case, the way a `[[link]]` does.
    pub fn page_by_title(&self, title: &str) -> Result<Option<Page>> {
        let conn = self.lock()?;
        conn.query_row(
            "SELECT id, key, title, body, kind, tier, pack, url, license, created, updated FROM pages WHERE lower(title) = lower(?1) LIMIT 1",
            [title],
            page_row,
        )
        .optional()
        .map_err(sql)
    }

    pub fn delete_page(&self, key: &str) -> Result<bool> {
        let conn = self.lock()?;
        let removed = conn
            .execute("DELETE FROM pages WHERE key = ?1", [key])
            .map_err(sql)?;
        drop(conn);
        self.forget_vectors();
        Ok(removed > 0)
    }

    /// Removes everything that came with a pack: its pages, passages, vectors and links.
    pub fn delete_pack(&self, pack: &str) -> Result<usize> {
        let conn = self.lock()?;
        let removed = conn
            .execute("DELETE FROM pages WHERE pack = ?1", [pack])
            .map_err(sql)?;
        drop(conn);
        self.forget_vectors();
        Ok(removed)
    }

    /// Pages that link to this one.
    pub fn backlinks(&self, id: i64) -> Result<Vec<LinkRef>> {
        let conn = self.lock()?;
        let mut stmt = conn
            .prepare(
                "SELECT DISTINCT p.id, p.title FROM links l JOIN pages p ON p.id = l.from_id WHERE l.to_id = ?1 ORDER BY lower(p.title)",
            )
            .map_err(sql)?;
        let rows = stmt
            .query_map([id], |row| {
                Ok(LinkRef {
                    id: row.get(0)?,
                    title: row.get(1)?,
                })
            })
            .map_err(sql)?;
        rows.collect::<rusqlite::Result<_>>().map_err(sql)
    }

    /// Pages this one links to. A link whose page does not exist yet has `id` 0 and the title as written.
    pub fn outlinks(&self, id: i64) -> Result<Vec<LinkRef>> {
        let conn = self.lock()?;
        let mut stmt = conn
            .prepare(
                "SELECT COALESCE(l.to_id, 0), COALESCE(p.title, l.target) FROM links l LEFT JOIN pages p ON p.id = l.to_id
                 WHERE l.from_id = ?1 ORDER BY lower(l.target)",
            )
            .map_err(sql)?;
        let rows = stmt
            .query_map([id], |row| {
                Ok(LinkRef {
                    id: row.get(0)?,
                    title: row.get(1)?,
                })
            })
            .map_err(sql)?;
        rows.collect::<rusqlite::Result<_>>().map_err(sql)
    }

    pub fn stats(&self) -> Result<Stats> {
        let conn = self.lock()?;
        let count = |query: &str| -> Result<usize> {
            conn.query_row(query, [], |row| row.get::<_, i64>(0))
                .map(|n| n as usize)
                .map_err(sql)
        };
        Ok(Stats {
            pages: count("SELECT COUNT(*) FROM pages")?,
            chunks: count("SELECT COUNT(*) FROM chunks")?,
            embedded: count("SELECT COUNT(*) FROM chunks WHERE embedding IS NOT NULL")?,
            yours: count("SELECT COUNT(*) FROM pages WHERE tier = 'yours'")?,
            reference: count("SELECT COUNT(*) FROM pages WHERE tier = 'reference'")?,
            web: count("SELECT COUNT(*) FROM pages WHERE tier = 'web'")?,
        })
    }

    /// Finds passages by words, by meaning, or both. `vector` is the query's vector with the model that made it; without it only
    /// the word search runs. The two rankings are fused by reciprocal rank, so neither needs its scores scaled to the other's.
    pub fn search(
        &self,
        text: &str,
        vector: Option<(&str, &[f32])>,
        options: &SearchOptions,
    ) -> Result<Vec<Hit>> {
        let words = fts_query(text);
        let by_words = match &words {
            Some(query) => self.word_ranking(query, options)?,
            None => Vec::new(),
        };
        let by_meaning = match vector {
            Some((model, vector)) => self.vector_ranking(model, vector, options)?,
            None => Vec::new(),
        };
        let mut fused: HashMap<i64, Fused> = HashMap::new();
        for (rank, chunk) in by_words.iter().enumerate() {
            let entry = fused.entry(*chunk).or_default();
            entry.score += 1.0 / (RRF_K + rank as f32);
            entry.text_rank = Some(rank);
        }
        for (rank, (chunk, similarity)) in by_meaning.iter().enumerate() {
            let entry = fused.entry(*chunk).or_default();
            entry.score += 1.0 / (RRF_K + rank as f32);
            entry.vector_rank = Some(rank);
            entry.similarity = Some(*similarity);
        }
        let mut ranked: Vec<(i64, Fused)> = fused.into_iter().collect();
        ranked.sort_by(|a, b| b.1.score.total_cmp(&a.1.score).then(a.0.cmp(&b.0)));

        let conn = self.lock()?;
        let mut seen = std::collections::HashSet::new();
        let mut hits = Vec::new();
        for (chunk, fused) in ranked {
            let row = conn
                .query_row(
                    "SELECT p.id, p.key, p.title, p.tier, c.heading, c.text FROM chunks c JOIN pages p ON p.id = c.page_id WHERE c.id = ?1",
                    [chunk],
                    |row| {
                        Ok((
                            row.get::<_, i64>(0)?,
                            row.get::<_, String>(1)?,
                            row.get::<_, String>(2)?,
                            row.get::<_, String>(3)?,
                            row.get::<_, String>(4)?,
                            row.get::<_, String>(5)?,
                        ))
                    },
                )
                .map_err(sql)?;
            if !seen.insert(row.0) {
                continue;
            }
            hits.push(Hit {
                page_id: row.0,
                key: row.1,
                title: row.2,
                tier: Tier::parse(&row.3),
                heading: row.4,
                text: row.5,
                score: fused.score,
                text_rank: fused.text_rank,
                vector_rank: fused.vector_rank,
                similarity: fused.similarity,
            });
            if hits.len() >= options.limit {
                break;
            }
        }
        Ok(hits)
    }

    fn word_ranking(&self, query: &str, options: &SearchOptions) -> Result<Vec<i64>> {
        let conn = self.lock()?;
        let mut statement = String::from(
            "SELECT c.id FROM chunks_fts f JOIN chunks c ON c.id = f.rowid JOIN pages p ON p.id = c.page_id WHERE chunks_fts MATCH ?1",
        );
        let mut values: Vec<String> = vec![query.to_string()];
        if !options.tiers.is_empty() {
            let marks = vec!["?"; options.tiers.len()].join(",");
            statement.push_str(&format!(" AND p.tier IN ({marks})"));
            values.extend(options.tiers.iter().map(|t| t.as_str().to_string()));
        }
        // the title weighs most, then the heading, then the passage
        statement.push_str(&format!(
            " ORDER BY bm25(chunks_fts, 4.0, 2.0, 1.0) LIMIT {}",
            options.candidates
        ));
        let mut stmt = conn.prepare(&statement).map_err(sql)?;
        let rows = stmt
            .query_map(params_from_iter(values.iter()), |row| row.get::<_, i64>(0))
            .map_err(sql)?;
        rows.collect::<rusqlite::Result<_>>().map_err(sql)
    }

    fn vector_ranking(
        &self,
        model: &str,
        query: &[f32],
        options: &SearchOptions,
    ) -> Result<Vec<(i64, f32)>> {
        let query = normalized(query.to_vec());
        let mut cache = self
            .vectors
            .lock()
            .map_err(|_| "vector cache poisoned".to_string())?;
        if cache.as_ref().is_none_or(|(loaded, _)| loaded != model) {
            *cache = Some((model.to_string(), self.load_vectors(model)?));
        }
        let entries = &cache.as_ref().expect("loaded above").1;
        let mut scored: Vec<(i64, f32)> = entries
            .iter()
            .filter(|e| options.tiers.is_empty() || options.tiers.contains(&e.tier))
            .filter(|e| e.vector.len() == query.len())
            .map(|e| (e.chunk, dot(&e.vector, &query)))
            .collect();
        scored.sort_by(|a, b| b.1.total_cmp(&a.1).then(a.0.cmp(&b.0)));
        scored.truncate(options.candidates);
        Ok(scored)
    }

    fn load_vectors(&self, model: &str) -> Result<Vec<VectorEntry>> {
        let conn = self.lock()?;
        let mut stmt = conn
            .prepare(
                "SELECT c.id, c.page_id, p.tier, c.embedding FROM chunks c JOIN pages p ON p.id = c.page_id WHERE c.model = ?1 AND c.embedding IS NOT NULL",
            )
            .map_err(sql)?;
        let rows = stmt
            .query_map([model], |row| {
                Ok(VectorEntry {
                    chunk: row.get(0)?,
                    page: row.get(1)?,
                    tier: Tier::parse(&row.get::<_, String>(2)?),
                    vector: decode(&row.get::<_, Vec<u8>>(3)?),
                })
            })
            .map_err(sql)?;
        rows.collect::<rusqlite::Result<_>>().map_err(sql)
    }

    /// Other pages whose passages are closest in meaning to this page's, best first: the "related notes" list.
    pub fn related(&self, id: i64, model: &str, limit: usize) -> Result<Vec<(LinkRef, f32)>> {
        let mut cache = self
            .vectors
            .lock()
            .map_err(|_| "vector cache poisoned".to_string())?;
        if cache.as_ref().is_none_or(|(loaded, _)| loaded != model) {
            *cache = Some((model.to_string(), self.load_vectors(model)?));
        }
        let entries = &cache.as_ref().expect("loaded above").1;
        let own: Vec<&VectorEntry> = entries.iter().filter(|e| e.page == id).collect();
        let mut best: HashMap<i64, f32> = HashMap::new();
        for other in entries.iter().filter(|e| e.page != id) {
            let top = own
                .iter()
                .filter(|mine| mine.vector.len() == other.vector.len())
                .map(|mine| dot(&mine.vector, &other.vector))
                .fold(f32::MIN, f32::max);
            if top > f32::MIN {
                let slot = best.entry(other.page).or_insert(f32::MIN);
                *slot = slot.max(top);
            }
        }
        drop(cache);
        let mut ranked: Vec<(i64, f32)> = best.into_iter().collect();
        ranked.sort_by(|a, b| b.1.total_cmp(&a.1).then(a.0.cmp(&b.0)));
        ranked.truncate(limit);
        let conn = self.lock()?;
        let mut out = Vec::new();
        for (page, similarity) in ranked {
            let title: String = conn
                .query_row("SELECT title FROM pages WHERE id = ?1", [page], |row| {
                    row.get(0)
                })
                .map_err(sql)?;
            out.push((LinkRef { id: page, title }, similarity));
        }
        Ok(out)
    }
}

const RRF_K: f32 = 60.0;

/// One passage's standing across the two rankings.
#[derive(Default)]
struct Fused {
    score: f32,
    text_rank: Option<usize>,
    vector_rank: Option<usize>,
    similarity: Option<f32>,
}

const PAGE_SELECT_KEY: &str =
    "SELECT id, key, title, body, kind, tier, pack, url, license, created, updated FROM pages WHERE key = ?1";
const PAGE_SELECT_ID: &str =
    "SELECT id, key, title, body, kind, tier, pack, url, license, created, updated FROM pages WHERE id = ?1";

fn page_row(row: &rusqlite::Row<'_>) -> rusqlite::Result<Page> {
    Ok(Page {
        id: row.get(0)?,
        key: row.get(1)?,
        title: row.get(2)?,
        body: row.get(3)?,
        kind: row.get(4)?,
        tier: Tier::parse(&row.get::<_, String>(5)?),
        pack: row.get(6)?,
        url: row.get(7)?,
        license: row.get(8)?,
        created: row.get(9)?,
        updated: row.get(10)?,
    })
}

/// What the embedder reads for a passage: the page and section it belongs to come first, so a bare paragraph keeps its context.
pub fn passage_input(title: &str, passage: &Passage) -> String {
    if passage.heading.is_empty() {
        format!("{title}\n{}", passage.text)
    } else {
        format!("{title} > {}\n{}", passage.heading, passage.text)
    }
}

/// Words of a free-text query as an FTS5 expression: each is quoted (so punctuation cannot break the syntax) and any one may match.
fn fts_query(text: &str) -> Option<String> {
    let mut words: Vec<String> = Vec::new();
    for word in text
        .split(|c: char| !c.is_alphanumeric())
        .filter(|w| w.chars().count() > 1)
    {
        let lower = word.to_lowercase();
        if !STOP_WORDS.contains(&lower.as_str()) && !words.contains(&lower) {
            words.push(lower);
        }
    }
    if words.is_empty() {
        return None;
    }
    Some(
        words
            .iter()
            .map(|w| format!("\"{w}\""))
            .collect::<Vec<_>>()
            .join(" OR "),
    )
}

const STOP_WORDS: &[&str] = &[
    "the", "is", "are", "was", "were", "of", "to", "in", "on", "for", "and", "or", "it", "its",
    "be", "as", "at", "by", "an", "do", "does", "did", "how", "what", "why", "when", "where",
    "which", "who", "with", "from", "that", "this", "can", "we", "you", "our", "my", "me", "if",
    "so", "not", "no", "has", "have", "had", "than", "then", "there", "about", "into",
];

/// `[[Target]]`, `[[Target|label]]` and `[[Target#section]]` in a page body, as the page titles they point at.
pub fn wikilinks(body: &str) -> Vec<String> {
    let mut found: Vec<String> = Vec::new();
    let mut rest = body;
    while let Some(start) = rest.find("[[") {
        let after = &rest[start + 2..];
        let Some(end) = after.find("]]") else { break };
        let inner = &after[..end];
        if !inner.contains('\n') {
            let target = inner
                .split(['|', '#'])
                .next()
                .unwrap_or("")
                .trim()
                .to_string();
            if !target.is_empty() && !found.iter().any(|t| t.eq_ignore_ascii_case(&target)) {
                found.push(target);
            }
        }
        rest = &after[end + 2..];
    }
    found
}

fn normalized(mut vector: Vec<f32>) -> Vec<f32> {
    let norm = vector.iter().map(|x| x * x).sum::<f32>().sqrt();
    if norm > 0.0 {
        for x in &mut vector {
            *x /= norm;
        }
    }
    vector
}

fn dot(a: &[f32], b: &[f32]) -> f32 {
    a.iter().zip(b).map(|(x, y)| x * y).sum()
}

fn encode(vector: &[f32]) -> Vec<u8> {
    vector.iter().flat_map(|x| x.to_le_bytes()).collect()
}

fn decode(bytes: &[u8]) -> Vec<f32> {
    bytes
        .as_chunks::<4>()
        .0
        .iter()
        .map(|b| f32::from_le_bytes(*b))
        .collect()
}

/// FNV-1a over the parts, kept by hand so the stored hashes do not change with the Rust version.
fn fingerprint(parts: &[&str]) -> String {
    let mut hash: u64 = 0xcbf29ce484222325;
    for part in parts {
        for byte in part.bytes().chain(std::iter::once(0x1f)) {
            hash ^= u64::from(byte);
            hash = hash.wrapping_mul(0x100000001b3);
        }
    }
    format!("{hash:016x}")
}

fn now() -> i64 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map(|d| d.as_secs() as i64)
        .unwrap_or(0)
}

#[cfg(test)]
mod tests {
    use super::*;

    /// Embeds a text as counts of a few words, enough to give "meaning" a shape in tests.
    struct Toy;

    const AXES: &[&str] = &["cat", "dog", "ship", "sea"];

    impl Embed for Toy {
        fn model_id(&self) -> &str {
            "toy"
        }
        fn embed(&self, texts: &[&str]) -> Result<Vec<Vec<f32>>> {
            Ok(texts
                .iter()
                .map(|t| {
                    let lower = t.to_lowercase();
                    AXES.iter()
                        .map(|a| lower.matches(a).count() as f32)
                        .collect()
                })
                .collect())
        }
    }

    fn store() -> Store {
        Store::in_memory().unwrap()
    }

    #[test]
    fn finds_by_words_and_ranks_the_title() {
        let store = store();
        store
            .put_page(&NewPage::note("a", "Sailing", "Ships cross the sea."), None)
            .unwrap();
        store
            .put_page(&NewPage::note("b", "Pets", "A cat sat on the mat."), None)
            .unwrap();
        let hits = store
            .search("how do ships sail", None, &SearchOptions::default())
            .unwrap();
        assert_eq!(hits[0].title, "Sailing");
        assert!(hits.iter().all(|h| h.vector_rank.is_none()));
    }

    #[test]
    fn meaning_search_finds_what_words_miss() {
        let store = store();
        let toy = Toy;
        store
            .put_page(
                &NewPage::note("a", "Boats", "Every ship needs the sea."),
                Some(&toy),
            )
            .unwrap();
        store
            .put_page(&NewPage::note("b", "Pets", "A dog and a cat."), Some(&toy))
            .unwrap();
        let query = toy.embed_query("sea").unwrap();
        let hits = store
            .search("zzz", Some(("toy", &query)), &SearchOptions::default())
            .unwrap();
        assert_eq!(hits[0].title, "Boats");
        assert!(hits[0].similarity.unwrap() > 0.5);
    }

    #[test]
    fn saving_again_embeds_only_what_changed() {
        let store = store();
        let toy = Toy;
        let body = format!(
            "# One\n\n{}\n\n# Two\n\n{}",
            "cat ".repeat(80),
            "dog ".repeat(80)
        );
        let first = store
            .put_page(&NewPage::note("a", "T", &body), Some(&toy))
            .unwrap();
        assert_eq!(first.embedded, 2);
        let same = store
            .put_page(&NewPage::note("a", "T", &body), Some(&toy))
            .unwrap();
        assert!(!same.changed);
        let edited = body.replace("dog dog", "ship ship");
        let second = store
            .put_page(&NewPage::note("a", "T", &edited), Some(&toy))
            .unwrap();
        assert!(second.changed);
        assert_eq!(second.embedded, 1);
        assert_eq!(store.stats().unwrap().chunks, 2);
    }

    #[test]
    fn a_page_without_vectors_gets_them_when_an_embedder_arrives() {
        let store = store();
        store
            .put_page(&NewPage::note("a", "T", "cat"), None)
            .unwrap();
        let put = store
            .put_page(&NewPage::note("a", "T", "cat"), Some(&Toy))
            .unwrap();
        assert!(put.changed);
        assert_eq!(put.embedded, 1);
    }

    #[test]
    fn links_resolve_in_either_order_and_backlinks_follow() {
        let store = store();
        let first = store
            .put_page(
                &NewPage::note("a", "Alpha", "see [[Beta]] and [[Missing|x]]"),
                None,
            )
            .unwrap();
        let beta = store
            .put_page(&NewPage::note("b", "beta", "plain"), None)
            .unwrap();
        let back = store.backlinks(beta.id).unwrap();
        assert_eq!(
            back,
            vec![LinkRef {
                id: first.id,
                title: "Alpha".into()
            }]
        );
        let out = store.outlinks(first.id).unwrap();
        assert_eq!(out.len(), 2);
        assert!(out.iter().any(|l| l.id == 0 && l.title == "Missing"));
        // editing the link away removes it
        store
            .put_page(&NewPage::note("a", "Alpha", "no links"), None)
            .unwrap();
        assert!(store.backlinks(beta.id).unwrap().is_empty());
    }

    #[test]
    fn deleting_a_pack_removes_pages_passages_and_search_hits() {
        let store = store();
        let mut page = NewPage::note("wiki:cat", "Cat", "Cats purr.");
        page.tier = Tier::Reference;
        page.pack = Some("wiki-small".into());
        store.put_page(&page, Some(&Toy)).unwrap();
        store
            .put_page(&NewPage::note("n", "Mine", "I like cats."), Some(&Toy))
            .unwrap();
        assert_eq!(
            store
                .search("cats", None, &SearchOptions::default())
                .unwrap()
                .len(),
            2
        );
        assert_eq!(store.delete_pack("wiki-small").unwrap(), 1);
        let hits = store
            .search("cats", None, &SearchOptions::default())
            .unwrap();
        assert_eq!(hits.len(), 1);
        assert_eq!(hits[0].tier, Tier::Yours);
        let stats = store.stats().unwrap();
        assert_eq!((stats.pages, stats.chunks), (1, 1));
    }

    #[test]
    fn tiers_filter_both_rankings() {
        let store = store();
        let mut web = NewPage::note("w", "Web cat", "cat");
        web.tier = Tier::Web;
        store.put_page(&web, Some(&Toy)).unwrap();
        store
            .put_page(&NewPage::note("n", "My cat", "cat"), Some(&Toy))
            .unwrap();
        let query = Toy.embed_query("cat").unwrap();
        let only_mine = SearchOptions {
            tiers: vec![Tier::Yours],
            ..Default::default()
        };
        let hits = store
            .search("cat", Some(("toy", &query)), &only_mine)
            .unwrap();
        assert_eq!(hits.len(), 1);
        assert_eq!(hits[0].title, "My cat");
    }

    #[test]
    fn related_pages_are_ranked_by_meaning() {
        let store = store();
        let a = store
            .put_page(&NewPage::note("a", "A", "cat cat dog"), Some(&Toy))
            .unwrap();
        store
            .put_page(&NewPage::note("b", "B", "cat dog"), Some(&Toy))
            .unwrap();
        store
            .put_page(&NewPage::note("c", "C", "ship sea"), Some(&Toy))
            .unwrap();
        let related = store.related(a.id, "toy", 5).unwrap();
        assert_eq!(related[0].0.title, "B");
        assert!(related[0].1 > related[1].1);
    }

    #[test]
    fn query_text_cannot_break_the_search_syntax() {
        let store = store();
        store
            .put_page(&NewPage::note("a", "T", "plain words"), None)
            .unwrap();
        for query in ["\"", "AND OR NOT", "col:word (", "***", "", "the of"] {
            store
                .search(query, None, &SearchOptions::default())
                .unwrap();
        }
    }

    #[test]
    fn wikilinks_take_the_target_before_a_label_or_section() {
        assert_eq!(
            wikilinks("[[A]] [[B|b]] [[C#sec]] [[a]] [[ ]] [[open"),
            vec!["A", "B", "C"]
        );
    }
}
