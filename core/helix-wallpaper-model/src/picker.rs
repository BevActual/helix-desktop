//! The wallpaper picker's model: which images a folder offers, which one is on screen, and the only way a choice is made. A choice names
//! an image by its place in the last listing (or a swatch by its place in the fixed list), so a path that was never listed cannot be set.

use std::path::{Path, PathBuf};

use serde::Serialize;

use crate::Source;

const MAX_IMAGES: usize = 300;
const EXTENSIONS: [&str; 4] = ["jpg", "jpeg", "png", "webp"];

/// Quiet, dark-to-light solid colours.
pub const SWATCHES: [[u8; 3]; 8] = [
    [0x1c, 0x1c, 0x22],
    [0x00, 0x00, 0x00],
    [0x2b, 0x3a, 0x55],
    [0x1f, 0x4d, 0x3a],
    [0x5a, 0x2d, 0x4f],
    [0x7a, 0x4b, 0x2a],
    [0x8e, 0x8e, 0x93],
    [0xf2, 0xf2, 0xf7],
];

#[derive(Clone, Debug, PartialEq, Eq)]
pub enum ChoiceError {
    NotListed,
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize)]
pub struct ImageRow {
    pub index: usize,
    pub name: String,
    pub current: bool,
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize)]
pub struct SwatchRow {
    pub index: usize,
    pub hex: String,
    pub current: bool,
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize)]
pub struct PickerView {
    pub folder: String,
    pub images: Vec<ImageRow>,
    pub swatches: Vec<SwatchRow>,
}

#[derive(Clone, Debug, Default)]
pub struct Picker {
    folder: PathBuf,
    files: Vec<PathBuf>,
    current: Option<Source>,
}

fn is_image(path: &Path) -> bool {
    path.extension()
        .and_then(|e| e.to_str())
        .is_some_and(|e| EXTENSIONS.contains(&e.to_ascii_lowercase().as_str()))
}

fn clean(text: &str) -> String {
    text.chars()
        .filter(|c| {
            !c.is_control()
                && !('\u{202a}'..='\u{202e}').contains(c)
                && !('\u{2066}'..='\u{2069}').contains(c)
        })
        .take(80)
        .collect()
}

impl Picker {
    /// The images directly inside `folder`, sorted by name; anything that is not a picture file is left out.
    pub fn set_folder(&mut self, folder: &Path, files: Vec<PathBuf>) {
        let mut files: Vec<PathBuf> = files
            .into_iter()
            .filter(|f| is_image(f) && f.parent() == Some(folder) && f.file_name().is_some())
            .collect();
        files.sort_by_key(|f| f.file_name().map(|n| n.to_string_lossy().to_lowercase()));
        files.dedup();
        files.truncate(MAX_IMAGES);
        self.folder = folder.to_path_buf();
        self.files = files;
    }

    pub fn set_current(&mut self, current: Option<Source>) {
        self.current = current;
    }

    pub fn view(&self) -> PickerView {
        PickerView {
            folder: clean(&self.folder.to_string_lossy()),
            images: self
                .files
                .iter()
                .enumerate()
                .map(|(index, file)| ImageRow {
                    index,
                    name: clean(
                        &file
                            .file_stem()
                            .map(|n| n.to_string_lossy().replace(['-', '_'], " "))
                            .unwrap_or_default(),
                    ),
                    current: self.current == Some(Source::Image(file.clone())),
                })
                .collect(),
            swatches: SWATCHES
                .iter()
                .enumerate()
                .map(|(index, rgb)| SwatchRow {
                    index,
                    hex: format!("#{:02x}{:02x}{:02x}", rgb[0], rgb[1], rgb[2]),
                    current: self.current == Some(Source::Color(*rgb)),
                })
                .collect(),
        }
    }

    pub fn file(&self, index: usize) -> Option<&Path> {
        self.files.get(index).map(PathBuf::as_path)
    }

    pub fn choose_image(&self, index: usize) -> Result<Source, ChoiceError> {
        self.files
            .get(index)
            .map(|file| Source::Image(file.clone()))
            .ok_or(ChoiceError::NotListed)
    }

    pub fn choose_swatch(&self, index: usize) -> Result<Source, ChoiceError> {
        SWATCHES
            .get(index)
            .map(|rgb| Source::Color(*rgb))
            .ok_or(ChoiceError::NotListed)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn picker() -> Picker {
        let mut p = Picker::default();
        let dir = Path::new("/pics");
        p.set_folder(
            dir,
            vec![
                "/pics/b-two.JPG".into(),
                "/pics/A_one.png".into(),
                "/pics/notes.txt".into(),
                "/pics/c.webp".into(),
                "/pics/sub/deep.png".into(),
                "/elsewhere/x.png".into(),
                "/pics/noext".into(),
            ],
        );
        p
    }

    #[test]
    fn only_pictures_directly_in_the_folder_are_listed_in_name_order() {
        let names: Vec<String> = picker().view().images.into_iter().map(|i| i.name).collect();
        assert_eq!(names, ["A one", "b two", "c"]);
    }

    #[test]
    fn a_choice_is_a_listed_image_or_a_swatch_and_nothing_else() {
        let p = picker();
        assert_eq!(
            p.choose_image(0),
            Ok(Source::Image("/pics/A_one.png".into()))
        );
        assert_eq!(p.choose_image(2), Ok(Source::Image("/pics/c.webp".into())));
        assert_eq!(p.choose_image(3), Err(ChoiceError::NotListed));
        assert_eq!(p.choose_image(usize::MAX), Err(ChoiceError::NotListed));
        assert_eq!(p.choose_swatch(1), Ok(Source::Color([0, 0, 0])));
        assert_eq!(p.choose_swatch(SWATCHES.len()), Err(ChoiceError::NotListed));
    }

    #[test]
    fn the_current_wallpaper_is_marked_whether_image_or_colour() {
        let mut p = picker();
        p.set_current(Some(Source::Image("/pics/b-two.JPG".into())));
        let v = p.view();
        assert_eq!(
            v.images
                .iter()
                .filter(|i| i.current)
                .map(|i| i.index)
                .collect::<Vec<_>>(),
            [1]
        );
        assert!(v.swatches.iter().all(|s| !s.current));
        p.set_current(Some(Source::Color(SWATCHES[2])));
        let v = p.view();
        assert!(v.images.iter().all(|i| !i.current));
        assert_eq!(
            v.swatches
                .iter()
                .filter(|s| s.current)
                .map(|s| s.index)
                .collect::<Vec<_>>(),
            [2]
        );
        p.set_current(None);
        assert!(p.view().images.iter().all(|i| !i.current));
    }

    #[test]
    fn a_new_folder_replaces_the_listing_so_old_indexes_stop_working() {
        let mut p = picker();
        p.set_folder(Path::new("/other"), vec!["/other/z.png".into()]);
        assert_eq!(p.choose_image(0), Ok(Source::Image("/other/z.png".into())));
        assert_eq!(p.choose_image(1), Err(ChoiceError::NotListed));
        p.set_folder(Path::new("/empty"), vec![]);
        assert!(p.view().images.is_empty());
    }

    #[test]
    fn the_listing_is_bounded_and_names_are_clean() {
        let mut p = Picker::default();
        let files: Vec<PathBuf> = (0..1000)
            .map(|i| PathBuf::from(format!("/p/{i:04}.png")))
            .collect();
        p.set_folder(Path::new("/p"), files);
        assert_eq!(p.view().images.len(), MAX_IMAGES);
        p.set_folder(Path::new("/p"), vec!["/p/a\u{202e}b\nc.png".into()]);
        let name = &p.view().images[0].name;
        assert!(
            !name.contains('\u{202e}') && !name.contains('\n'),
            "{name:?}"
        );
    }
}
