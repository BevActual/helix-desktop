//! The Helix lock screen, as tested pieces: what has been typed (`entry`), asking PAM (`pam`), the picture (`paint`) and the plain facts it shows
//! (`facts`). The Wayland side is in `main.rs`.

pub mod entry;
pub mod facts;
pub mod paint;
pub mod pam;
