//! Settings → Users: the signed-in account's name and picture, kept by the account service (`org.freedesktop.Accounts`), which is where the lock
//! screen, the login screen and every other program look for them. Only the person's own account is touched, and the service's own policy decides
//! whether a change is allowed (changing your own name and picture is allowed for you without a password).

use base64::Engine;
use serde::Serialize;
use std::os::unix::fs::MetadataExt;

const SERVICE: &str = "org.freedesktop.Accounts";

#[derive(Serialize)]
pub struct UserView {
    pub name: String,
    pub username: String,
    /// "Standard" or "Administrator".
    pub kind: String,
    /// The picture as a small `data:` image, when the account has one.
    pub picture: Option<String>,
    /// Whether the account service is there to change anything.
    pub editable: bool,
}

async fn proxy() -> Result<zbus::Proxy<'static>, String> {
    let uid = std::fs::metadata("/proc/self")
        .map_err(|e| e.to_string())?
        .uid();
    let connection = zbus::Connection::system()
        .await
        .map_err(|_| "The account service is not available.".to_string())?;
    zbus::Proxy::new(
        &connection,
        SERVICE,
        format!("/org/freedesktop/Accounts/User{uid}"),
        "org.freedesktop.Accounts.User",
    )
    .await
    .map_err(|_| "The account service does not know this account.".to_string())
}

/// Letters, numbers, spaces and the usual punctuation of a name: no control characters, and nothing the passwd file treats as a separator.
pub fn valid_name(name: &str) -> Result<String, String> {
    let name = name.split_whitespace().collect::<Vec<_>>().join(" ");
    if name.is_empty() || name.chars().count() > 64 {
        return Err("A name is 1 to 64 characters.".into());
    }
    if name
        .chars()
        .any(|c| c.is_control() || matches!(c, ':' | ',' | '\n' | '='))
    {
        return Err("A name cannot contain : , = or control characters.".into());
    }
    Ok(name)
}

/// A picture as a 128-pixel PNG `data:` URI, centre-cropped to a square.
fn data_uri(path: &std::path::Path) -> Option<String> {
    let image = image::open(path).ok()?;
    let side = image.width().min(image.height());
    let square = image
        .crop_imm(
            (image.width() - side) / 2,
            (image.height() - side) / 2,
            side,
            side,
        )
        .thumbnail(128, 128);
    let mut bytes = Vec::new();
    square
        .write_to(
            &mut std::io::Cursor::new(&mut bytes),
            image::ImageFormat::Png,
        )
        .ok()?;
    Some(format!(
        "data:image/png;base64,{}",
        base64::engine::general_purpose::STANDARD.encode(bytes)
    ))
}

#[tauri::command]
pub async fn users_view() -> UserView {
    let fallback = UserView {
        name: std::env::var("USER").unwrap_or_default(),
        username: std::env::var("USER").unwrap_or_default(),
        kind: String::new(),
        picture: None,
        editable: false,
    };
    let Ok(user) = proxy().await else {
        return fallback;
    };
    let text = |p: Result<String, zbus::Error>| p.unwrap_or_default();
    let name = text(user.get_property::<String>("RealName").await);
    let username = text(user.get_property::<String>("UserName").await);
    let icon = text(user.get_property::<String>("IconFile").await);
    let kind = match user.get_property::<i32>("AccountType").await {
        Ok(1) => "Administrator",
        Ok(_) => "Standard",
        Err(_) => "",
    };
    UserView {
        name: if name.is_empty() {
            username.clone()
        } else {
            name
        },
        username,
        kind: kind.into(),
        picture: (!icon.is_empty())
            .then(|| data_uri(std::path::Path::new(&icon)))
            .flatten(),
        editable: true,
    }
}

#[tauri::command]
pub async fn users_set_name(name: String) -> Result<UserView, String> {
    let name = valid_name(&name)?;
    proxy()
        .await?
        .call::<_, _, ()>("SetRealName", &(name,))
        .await
        .map_err(|e| format!("The name was not changed: {e}"))?;
    eprintln!("SETTINGS users name changed");
    Ok(users_view().await)
}

/// Picks a picture with the system's file chooser, so the person's files are chosen by them and never typed.
#[tauri::command]
pub async fn users_choose_picture() -> Result<UserView, String> {
    let output = tokio::task::spawn_blocking(|| {
        std::process::Command::new("zenity")
            .args([
                "--file-selection",
                "--title=Choose a picture",
                "--file-filter=Pictures | *.png *.jpg *.jpeg *.webp",
            ])
            .stdin(std::process::Stdio::null())
            .output()
    })
    .await
    .map_err(|e| e.to_string())?
    .map_err(|_| "The file chooser (zenity) is not installed.".to_string())?;
    let path = String::from_utf8_lossy(&output.stdout).trim().to_string();
    if !output.status.success() || path.is_empty() {
        return Ok(users_view().await);
    }
    let file = std::path::Path::new(&path);
    let meta = std::fs::metadata(file).map_err(|_| "That file cannot be read.".to_string())?;
    if !meta.is_file()
        || image::ImageReader::open(file)
            .and_then(|r| r.with_guessed_format())
            .ok()
            .and_then(|r| r.format())
            .is_none()
    {
        return Err("That is not a picture.".into());
    }
    // The account service keeps its own copy and accepts only small pictures, so a large one is made smaller first.
    let prepared = if meta.len() > 900_000 {
        let image = image::open(file).map_err(|_| "That picture cannot be read.".to_string())?;
        let side = image.width().min(image.height());
        let small = image
            .crop_imm(
                (image.width() - side) / 2,
                (image.height() - side) / 2,
                side,
                side,
            )
            .thumbnail(512, 512);
        let temporary =
            std::env::temp_dir().join(format!("helix-avatar-{}.png", std::process::id()));
        small.save(&temporary).map_err(|e| e.to_string())?;
        temporary
    } else {
        file.to_path_buf()
    };
    let result = proxy()
        .await?
        .call::<_, _, ()>("SetIconFile", &(prepared.to_string_lossy().to_string(),))
        .await;
    if prepared != file {
        let _ = std::fs::remove_file(&prepared);
    }
    result.map_err(|e| format!("The picture was not changed: {e}"))?;
    eprintln!("SETTINGS users picture changed");
    Ok(users_view().await)
}

#[tauri::command]
pub async fn users_clear_picture() -> Result<UserView, String> {
    proxy()
        .await?
        .call::<_, _, ()>("SetIconFile", &("",))
        .await
        .map_err(|e| format!("The picture was not removed: {e}"))?;
    eprintln!("SETTINGS users picture removed");
    Ok(users_view().await)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn a_name_is_plain_text_the_passwd_file_can_hold() {
        assert_eq!(valid_name("  Ada   Lovelace ").unwrap(), "Ada Lovelace");
        assert_eq!(
            valid_name("Zoë O'Brien-Smith").unwrap(),
            "Zoë O'Brien-Smith"
        );
        for bad in ["", "   ", "a:b", "a,b", "a=b", "bell\u{7}", &"x".repeat(65)] {
            assert!(valid_name(bad).is_err(), "{bad:?}");
        }
    }
}
