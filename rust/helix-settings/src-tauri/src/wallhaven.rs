//! Wallhaven in Settings: search, thumbnails, and downloading the one picture the person chose. The network is touched only here, only
//! for wallhaven's own hosts, and only for what the last search listed.

use std::collections::HashMap;
use std::io::Read;
use std::sync::Mutex;
use std::time::Duration;

use helix_shell_core::wallhaven::{self as model, Hit, Results, Search, MAX_DOWNLOAD};
use tauri::AppHandle;

/// What the last search listed, by id: nothing else can be fetched or saved.
static LISTED: Mutex<Option<HashMap<String, Hit>>> = Mutex::new(None);

fn agent() -> ureq::Agent {
    ureq::Agent::config_builder()
        .timeout_global(Some(Duration::from_secs(45)))
        .max_redirects(0)
        .user_agent("helix-settings/0.1 (wallpaper search)")
        .http_status_as_error(false)
        .build()
        .into()
}

fn fetch(url: &str, limit: u64) -> Result<Vec<u8>, String> {
    if !(url.starts_with(model::API) || model::trusted_picture_url(url)) {
        return Err("not a wallhaven address".into());
    }
    let mut response = agent().get(url).call().map_err(|_| "Could not reach wallhaven. Check the network.".to_string())?;
    match response.status().as_u16() {
        200 => {}
        429 => return Err("Wallhaven asks for a short pause. Try again in a minute.".into()),
        _ => return Err("Wallhaven could not answer that.".into()),
    }
    let mut bytes = Vec::new();
    response.body_mut().as_reader().take(limit + 1).read_to_end(&mut bytes).map_err(|_| "The download was interrupted.".to_string())?;
    if bytes.len() as u64 > limit {
        return Err("That picture is larger than allowed.".into());
    }
    Ok(bytes)
}

fn encode(value: &str) -> String {
    value.bytes().map(|b| if b.is_ascii_alphanumeric() || b"-_.~".contains(&b) { (b as char).to_string() } else { format!("%{b:02X}") }).collect()
}

#[tauri::command]
pub async fn wallhaven_search(app: AppHandle, query: String, sorting: String, minimum: String, page: u32) -> Result<serde_json::Value, String> {
    let search = Search::new(&query, &sorting, &minimum, page).map_err(|_| "Use letters and numbers to search.".to_string())?;
    let folder = crate::wallpaper_folder(&app);
    tauri::async_runtime::spawn_blocking(move || {
        let pairs: Vec<String> = search.parameters().into_iter().map(|(k, v)| format!("{k}={}", encode(&v))).collect();
        let bytes = fetch(&format!("{}?{}", model::API, pairs.join("&")), 2 * 1024 * 1024)?;
        let body: serde_json::Value = serde_json::from_slice(&bytes).map_err(|_| "Wallhaven sent something unexpected.".to_string())?;
        let results = model::parse_results(&body);
        let mut listed = LISTED.lock().expect("listed");
        // Only the page on screen can be fetched or saved.
        let map = listed.get_or_insert_with(HashMap::new);
        map.clear();
        for hit in &results.hits {
            map.insert(hit.id.clone(), hit.clone());
        }
        // Which of these are already in the wallpaper folder, so the page can say so.
        let mut view = serde_json::to_value(&results).map_err(|e| e.to_string())?;
        for (entry, hit) in view["hits"].as_array_mut().into_iter().flatten().zip(&results.hits) {
            entry["saved"] = folder.join(model::file_name(hit)).exists().into();
        }
        Ok(view)
    })
    .await
    .map_err(|e| e.to_string())?
}

fn listed(id: &str) -> Option<Hit> {
    LISTED.lock().expect("listed").as_ref()?.get(id).cloned()
}

/// The small preview of a listed picture, as a data URL.
#[tauri::command]
pub async fn wallhaven_thumb(id: String) -> Option<String> {
    let hit = listed(&id)?;
    tauri::async_runtime::spawn_blocking(move || {
        use base64::Engine;
        let bytes = fetch(&hit.thumb_url(), 2 * 1024 * 1024).ok()?;
        image::guess_format(&bytes).ok().filter(|f| *f == image::ImageFormat::Jpeg)?;
        Some(format!("data:image/jpeg;base64,{}", base64::engine::general_purpose::STANDARD.encode(bytes)))
    })
    .await
    .ok()
    .flatten()
}

/// Downloads a listed picture into the wallpaper folder (once) and makes it the wallpaper.
#[tauri::command]
pub async fn wallhaven_use(app: AppHandle, id: String) -> Result<(), String> {
    let hit = listed(&id).ok_or("That picture is not in the list any more. Search again.")?;
    let folder = crate::wallpaper_folder(&app);
    let target = folder.join(model::file_name(&hit));
    let saved = target.clone();
    if !target.exists() {
        tauri::async_runtime::spawn_blocking(move || -> Result<(), String> {
            let bytes = fetch(&hit.full_url(), MAX_DOWNLOAD)?;
            // It must really be the picture type it was listed as, and it must decode far enough to have a size.
            let format = image::guess_format(&bytes).map_err(|_| "That is not a picture.".to_string())?;
            let expected = if hit.extension == "png" { image::ImageFormat::Png } else { image::ImageFormat::Jpeg };
            if format != expected {
                return Err("That is not the picture that was listed.".into());
            }
            image::ImageReader::with_format(std::io::Cursor::new(&bytes), format).into_dimensions().map_err(|_| "That picture is damaged.".to_string())?;
            std::fs::create_dir_all(&folder).map_err(|e| e.to_string())?;
            let temporary = saved.with_extension("part");
            std::fs::write(&temporary, &bytes).map_err(|e| e.to_string())?;
            std::fs::rename(&temporary, &saved).map_err(|e| e.to_string())
        })
        .await
        .map_err(|e| e.to_string())??;
    }
    eprintln!("SETTINGS wallhaven saved");
    crate::write_choice(&app, helix_wallpaper::Source::Image(target))
}
