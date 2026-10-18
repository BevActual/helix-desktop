//! The models Mobius can offer to download: a short, curated list with pinned checksums.
//!
//! Nothing here is fetched at run time to decide what is trusted: each file's size and SHA-256 are written down below (taken from the
//! publisher's own listing), the host is fixed, and a download that does not match is thrown away. A model that is installed by
//! other means is still listed, from the models folder, just without a description.

use serde::Serialize;

#[derive(Clone, Copy, Debug, Serialize)]
pub struct CatalogFile {
    /// The name it is saved as in the models folder. A vision projector is named `mmproj-<model base name>…` so the
    /// inference service pairs it with its model.
    pub name: &'static str,
    pub url: &'static str,
    pub sha256: &'static str,
    pub bytes: u64,
}

#[derive(Clone, Copy, Debug, Serialize)]
pub struct CatalogModel {
    pub id: &'static str,
    pub title: &'static str,
    pub maker: &'static str,
    pub blurb: &'static str,
    /// What it is good for: "chat", "code", "vision", "speech".
    pub tasks: &'static [&'static str],
    pub vision: bool,
    pub license: &'static str,
    /// Roughly how much GPU memory it wants for a normal conversation, in GB (the model plus its working memory).
    pub memory_gb: f32,
    /// The model file first, then (for vision) its projector.
    pub files: &'static [CatalogFile],
}

impl CatalogModel {
    pub fn bytes(&self) -> u64 {
        self.files.iter().map(|f| f.bytes).sum()
    }
    /// The file name the inference service lists the model under (the first shard when it is split).
    pub fn model_file(&self) -> &'static str {
        self.files[0].name
    }
}

pub const HOST: &str = "https://huggingface.co/";

pub const CATALOG: &[CatalogModel] = &[
    CatalogModel {
        id: "sd-turbo",
        title: "SD-Turbo",
        maker: "Stability AI · GPUStack quantization",
        blurb: "Fast local image generation. Best at 512 × 512 with 1–4 steps. A starter model for illustrations and variations.",
        tasks: &["image"], vision: false,
        license: "Stability AI non-commercial research license",
        memory_gb: 5.0,
        files: &[CatalogFile {
            name: "image/stable-diffusion-v2-1-turbo-Q4_0.gguf",
            url: "https://huggingface.co/gpustack/stable-diffusion-v2-1-turbo-GGUF/resolve/1316499e70f0adaf26589a8012b26572cb9d13d5/stable-diffusion-v2-1-turbo-Q4_0.gguf",
            sha256: "901b22431d009148d95702c7b7a836788bd398450275e680302211e3755ca81b",
            bytes: 2_185_459_424,
        }],
    },
    CatalogModel {
        id: "whisper-small",
        title: "Whisper small",
        maker: "OpenAI",
        blurb: "Turns speech into text in about 99 languages. Powers the microphone in the composer; runs on the processor, not the graphics card.",
        tasks: &["speech"],
        vision: false,
        license: "MIT",
        memory_gb: 0.7,
        files: &[CatalogFile {
            name: "speech/ggml-small-q5_1.bin",
            url: "https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-small-q5_1.bin",
            sha256: "ae85e4a935d7a567bd102fe55afc16bb595bdb618e11b2fc7591bc08120411bb",
            bytes: 190_085_487,
        }],
    },
    CatalogModel {
        id: "bge-small-en",
        title: "BGE small (meaning search)",
        maker: "BAAI",
        blurb: "Reads a sentence and says what it means, in about 10 milliseconds on the processor. Mobius uses it to suggest the right mode for a message, and it is the model that will search your notes and project files by meaning.",
        tasks: &["embedding"],
        vision: false,
        license: "MIT",
        memory_gb: 0.3,
        files: &[
            CatalogFile {
                name: "embed/bge-small-en-v1.5/model.safetensors",
                url: "https://huggingface.co/BAAI/bge-small-en-v1.5/resolve/main/model.safetensors",
                sha256: "3c9f31665447c8911517620762200d2245a2518d6e7208acc78cd9db317e21ad",
                bytes: 133_466_304,
            },
            CatalogFile {
                name: "embed/bge-small-en-v1.5/config.json",
                url: "https://huggingface.co/BAAI/bge-small-en-v1.5/resolve/main/config.json",
                sha256: "094f8e891b932f2000c92cfc663bac4c62069f5d8af5b5278c4306aef3084750",
                bytes: 743,
            },
            CatalogFile {
                name: "embed/bge-small-en-v1.5/tokenizer.json",
                url: "https://huggingface.co/BAAI/bge-small-en-v1.5/resolve/main/tokenizer.json",
                sha256: "d241a60d5e8f04cc1b2b3e9ef7a4921b27bf526d9f6050ab90f9267a1f9e5c66",
                bytes: 711_396,
            },
        ],
    },
    CatalogModel {
        id: "qwen2.5-1.5b",
        title: "Qwen2.5 1.5B",
        maker: "Alibaba Qwen",
        blurb: "Small and quick. Good for short answers and as a fast fallback.",
        tasks: &["chat"],
        vision: false,
        license: "Apache-2.0",
        memory_gb: 2.0,
        files: &[CatalogFile {
            name: "qwen2.5-1.5b-instruct-q4_k_m.gguf",
            url: "https://huggingface.co/Qwen/Qwen2.5-1.5B-Instruct-GGUF/resolve/main/qwen2.5-1.5b-instruct-q4_k_m.gguf",
            sha256: "6a1a2eb6d15622bf3c96857206351ba97e1af16c30d7a74ee38970e434e9407e",
            bytes: 1_117_320_736,
        }],
    },
    CatalogModel {
        id: "qwen2.5-3b",
        title: "Qwen2.5 3B",
        maker: "Alibaba Qwen",
        blurb: "A step up from 1.5B while staying light; fits beside other apps.",
        tasks: &["chat"],
        vision: false,
        license: "Qwen research license",
        memory_gb: 3.0,
        files: &[CatalogFile {
            name: "qwen2.5-3b-instruct-q4_k_m.gguf",
            url: "https://huggingface.co/Qwen/Qwen2.5-3B-Instruct-GGUF/resolve/main/qwen2.5-3b-instruct-q4_k_m.gguf",
            sha256: "626b4a6678b86442240e33df819e00132d3ba7dddfe1cdc4fbb18e0a9615c62d",
            bytes: 2_104_932_768,
        }],
    },
    CatalogModel {
        id: "llama-3.2-3b",
        title: "Llama 3.2 3B",
        maker: "Meta",
        blurb: "A friendly, fast general assistant in a small package.",
        tasks: &["chat"],
        vision: false,
        license: "Llama 3.2 community license",
        memory_gb: 3.0,
        files: &[CatalogFile {
            name: "Llama-3.2-3B-Instruct-Q4_K_M.gguf",
            url: "https://huggingface.co/bartowski/Llama-3.2-3B-Instruct-GGUF/resolve/main/Llama-3.2-3B-Instruct-Q4_K_M.gguf",
            sha256: "6c1a2b41161032677be168d354123594c0e6e67d2b9227c84f296ad037c728ff",
            bytes: 2_019_377_696,
        }],
    },
    CatalogModel {
        id: "qwen3-4b",
        title: "Qwen3 4B (thinks first)",
        maker: "Alibaba Qwen",
        blurb: "Reasons before it answers, so it does better on puzzles, maths and planning than models of its size that answer straight away. Replies start later because it thinks first; add /no_think to a message to skip that.",
        tasks: &["chat", "reasoning"],
        vision: false,
        license: "Apache-2.0",
        memory_gb: 3.5,
        files: &[CatalogFile {
            name: "Qwen3-4B-Q4_K_M.gguf",
            url: "https://huggingface.co/Qwen/Qwen3-4B-GGUF/resolve/main/Qwen3-4B-Q4_K_M.gguf",
            sha256: "7485fe6f11af29433bc51cab58009521f205840f5b4ae3a32fa7f92e8534fdf5",
            bytes: 2_497_280_256,
        }],
    },
    CatalogModel {
        id: "qwen2.5-7b",
        title: "Qwen2.5 7B",
        maker: "Alibaba Qwen",
        blurb: "The all-round default: strong answers, writing and reasoning.",
        tasks: &["chat"],
        vision: false,
        license: "Apache-2.0",
        memory_gb: 6.5,
        files: &[
            CatalogFile {
                name: "qwen2.5-7b-instruct-q4_k_m-00001-of-00002.gguf",
                url: "https://huggingface.co/Qwen/Qwen2.5-7B-Instruct-GGUF/resolve/main/qwen2.5-7b-instruct-q4_k_m-00001-of-00002.gguf",
                sha256: "dfce12e3862a5283ccfb88221b48480e58745165de856439950d0f22590580db",
                bytes: 3_993_201_344,
            },
            CatalogFile {
                name: "qwen2.5-7b-instruct-q4_k_m-00002-of-00002.gguf",
                url: "https://huggingface.co/Qwen/Qwen2.5-7B-Instruct-GGUF/resolve/main/qwen2.5-7b-instruct-q4_k_m-00002-of-00002.gguf",
                sha256: "539cf93f78e887edea1c04e2d7d8cdaca9d01dae9c9025bcb8accbe29df3d72a",
                bytes: 689_872_288,
            },
        ],
    },
    CatalogModel {
        id: "qwen2.5-coder-7b",
        title: "Qwen2.5 Coder 7B",
        maker: "Alibaba Qwen",
        blurb: "Tuned for programming: writing, explaining and fixing code.",
        tasks: &["code", "chat"],
        vision: false,
        license: "Apache-2.0",
        memory_gb: 6.5,
        files: &[CatalogFile {
            name: "Qwen2.5-Coder-7B-Instruct-Q4_K_M.gguf",
            url: "https://huggingface.co/bartowski/Qwen2.5-Coder-7B-Instruct-GGUF/resolve/main/Qwen2.5-Coder-7B-Instruct-Q4_K_M.gguf",
            sha256: "1664fccab734674a50763490a8c6931b70e3f2f8ec10031b54806d30e5f956b6",
            bytes: 4_683_074_336,
        }],
    },
    CatalogModel {
        id: "gemma-3-4b",
        title: "Gemma 3 4B",
        maker: "Google",
        blurb: "Small model that reads pictures. A light choice for vision.",
        tasks: &["vision", "chat"],
        vision: true,
        license: "Gemma terms of use",
        memory_gb: 4.0,
        files: &[
            CatalogFile {
                name: "gemma-3-4b-it-Q4_K_M.gguf",
                url: "https://huggingface.co/ggml-org/gemma-3-4b-it-GGUF/resolve/main/gemma-3-4b-it-Q4_K_M.gguf",
                sha256: "882e8d2db44dc554fb0ea5077cb7e4bc49e7342a1f0da57901c0802ea21a0863",
                bytes: 2_489_757_856,
            },
            CatalogFile {
                name: "mmproj-gemma-3-4b-it-f16.gguf",
                url: "https://huggingface.co/ggml-org/gemma-3-4b-it-GGUF/resolve/main/mmproj-model-f16.gguf",
                sha256: "8c0fb064b019a6972856aaae2c7e4792858af3ca4561be2dbf649123ba6c40cb",
                bytes: 851_251_104,
            },
        ],
    },
    CatalogModel {
        id: "qwen2.5-vl-7b",
        title: "Qwen2.5 VL 7B",
        maker: "Alibaba Qwen",
        blurb: "Reads pictures, screenshots and documents, and chats as well.",
        tasks: &["vision", "chat"],
        vision: true,
        license: "Apache-2.0",
        memory_gb: 7.5,
        files: &[
            CatalogFile {
                name: "Qwen2.5-VL-7B-Instruct-Q4_K_M.gguf",
                url: "https://huggingface.co/ggml-org/Qwen2.5-VL-7B-Instruct-GGUF/resolve/main/Qwen2.5-VL-7B-Instruct-Q4_K_M.gguf",
                sha256: "9258bf05b12686d097ff3b6b18d968ab393649780aa2b3cd67fec43d50554392",
                bytes: 4_683_072_032,
            },
            CatalogFile {
                name: "mmproj-Qwen2.5-VL-7B-Instruct-Q8_0.gguf",
                url: "https://huggingface.co/ggml-org/Qwen2.5-VL-7B-Instruct-GGUF/resolve/main/mmproj-Qwen2.5-VL-7B-Instruct-Q8_0.gguf",
                sha256: "2ddb555391bae966e412deab9e07b58afa18bcc06930ba0f1c78a3695ab9e506",
                bytes: 853_119_712,
            },
        ],
    },
];

pub fn find(id: &str) -> Option<&'static CatalogModel> {
    CATALOG.iter().find(|m| m.id == id)
}

/// The catalog entry whose model file is `file` (the id the inference service lists).
pub fn by_file(file: &str) -> Option<&'static CatalogModel> {
    CATALOG.iter().find(|m| m.model_file() == file)
}

/// The three files an embedding model needs, which sit together in `embed/<model>/`.
const EMBED_FILES: &[&str] = &["model.safetensors", "config.json", "tokenizer.json"];

/// A file name that is safe to create inside the models folder: plain, a `.gguf`; a speech model (`.bin`) under `speech/`; an image
/// model (`.gguf`) under `image/`; or one of an embedding model's three files under `embed/<model>/`.
pub fn safe_file_name(name: &str) -> bool {
    let plain_chars = |text: &str| {
        !text.is_empty()
            && !text.starts_with('.')
            && text
                .bytes()
                .all(|b| b.is_ascii_alphanumeric() || matches!(b, b'.' | b'-' | b'_'))
    };
    if let Some(rest) = name.strip_prefix("embed/") {
        return name.len() <= 200
            && rest
                .split_once('/')
                .is_some_and(|(model, file)| plain_chars(model) && EMBED_FILES.contains(&file));
    }
    let (speech, plain) = match name.strip_prefix("speech/") {
        Some(rest) => (true, rest),
        None => (false, name.strip_prefix("image/").unwrap_or(name)),
    };
    let extension_ok = if speech {
        plain.ends_with(".bin")
    } else {
        plain.ends_with(".gguf")
    };
    name.len() <= 200 && extension_ok && plain_chars(plain)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn only_expected_names_are_safe() {
        for good in ["a-b_c.1.gguf", "speech/ggml-small-q5_1.bin", "image/x.gguf", "embed/bge-small-en-v1.5/model.safetensors", "embed/m/config.json"] {
            assert!(safe_file_name(good), "{good}");
        }
        for bad in ["", ".hidden.gguf", "../x.gguf", "a/b.gguf", "speech/x.gguf", "embed/m/evil.json", "embed/m/n/config.json", "embed//config.json", "embed/../config.json", "embed/m/model.safetensors/", "a b.gguf"] {
            assert!(!safe_file_name(bad), "{bad}");
        }
    }

    #[test]
    fn every_entry_is_pinned_and_safe() {
        let mut ids = std::collections::HashSet::new();
        for model in CATALOG {
            assert!(ids.insert(model.id), "duplicate id {}", model.id);
            assert!(!model.files.is_empty());
            assert_eq!(
                model.vision,
                model.files.len() > 1 && model.files[1].name.starts_with("mmproj-")
            );
            for file in model.files {
                assert!(safe_file_name(file.name), "{}", file.name);
                assert!(file.url.starts_with(HOST), "{}", file.url);
                assert_eq!(file.sha256.len(), 64);
                assert!(file
                    .sha256
                    .bytes()
                    .all(|b| b.is_ascii_digit() || (b'a'..=b'f').contains(&b)));
                // a model's small companion files (its configuration) are the only ones under a megabyte
                assert!(file.bytes > 1_000_000 || file.name.ends_with(".json"));
            }
            if model.vision {
                // The service pairs a projector with the model whose base name follows `mmproj-`.
                let base = model.files[0].name.trim_end_matches(".gguf");
                let base = base.rsplit_once('-').map_or(base, |(b, _)| b);
                assert!(
                    model.files[1].name.starts_with(&format!("mmproj-{base}")),
                    "{}",
                    model.id
                );
            }
        }
    }
}
