//! Which model answers a request, and why. One function takes what is known for free (the slot, whether the content is private, how long the request is,
//! what is installed, what the person set) and returns a decision with a sentence saying why, which Mobius shows and the audit line records.
//! No model is asked and nothing is calibrated: the rules are hard rules first, then the person's choice for the slot. See `docs/design/model-routing.md`.
//!
//!   1. Acting for the desktop (the `act` slot) only ever runs on a model on this computer.
//!   2. Private content (a picture, or anything the caller marks private: a file, the clipboard, the screen) never leaves this computer.
//!   3. A caller that names a backend gets it (except for 1 and 2); asking for a stronger model uses the slot's stronger-model choice.
//!   4. Otherwise the slot's route decides; and a request too long for the local model goes to the slot's stronger-model choice if the person set one.

use crate::backend::ModelInfo;
use crate::routing::RoutingTable;

pub struct Input<'a> {
    pub task: &'a str,
    /// A model the caller named (without a backend): that is a model on this computer.
    pub requested_model: Option<&'a str>,
    /// A backend the caller named.
    pub backend: Option<&'a str>,
    /// "Ask a stronger model": use the slot's escalation target.
    pub escalate: bool,
    /// Content that must stay on this computer.
    pub private: bool,
    pub has_images: bool,
    /// Rough size of the request plus its answer, in tokens.
    pub estimated_tokens: u32,
    /// What the local engine can hold at once; 0 when not known.
    pub local_context: u32,
    pub local_models: &'a [ModelInfo],
    pub routing: &'a RoutingTable,
}

#[derive(Debug, PartialEq, Eq)]
pub struct Decision {
    /// Empty for this computer.
    pub backend: String,
    /// The model; empty means the engine's first installed.
    pub model: String,
    pub reason: String,
}

fn label(routing: &RoutingTable, id: &str) -> String {
    routing
        .backend(id)
        .map(|b| {
            if b.label.is_empty() {
                b.id.clone()
            } else {
                b.label.clone()
            }
        })
        .unwrap_or_else(|| id.to_string())
}

/// The remote backend a slot may use, if it exists and speaks the protocol.
fn usable<'a>(
    routing: &'a RoutingTable,
    id: &str,
    what: &str,
) -> Result<&'a crate::routing::BackendInfo, String> {
    let backend = routing
        .backend(id)
        .ok_or_else(|| format!("{what} is a backend that is not set up: {id}"))?;
    if backend.kind != "openai" {
        return Err(format!(
            "backend {} is of a kind this service does not speak",
            backend.id
        ));
    }
    Ok(backend)
}

/// The local model for private content: one that reads pictures when there are pictures.
fn local_for_private(input: &Input<'_>, preferred: &str) -> String {
    if !input.has_images {
        return preferred.to_string();
    }
    let reads_pictures = |id: &str| input.local_models.iter().any(|m| m.id == id && m.vision);
    if !preferred.is_empty() && reads_pictures(preferred) {
        return preferred.to_string();
    }
    input
        .local_models
        .iter()
        .find(|m| m.vision)
        .map(|m| m.id.clone())
        .unwrap_or_else(|| preferred.to_string())
}

pub fn decide(input: &Input<'_>) -> Result<Decision, String> {
    let private = input.private || input.has_images;
    let why_private = if input.has_images {
        "it contains a picture"
    } else {
        "it is marked private"
    };
    let route = input.routing.route(input.task);
    let local = |model: &str, reason: String| {
        Ok(Decision {
            backend: String::new(),
            model: model.to_string(),
            reason,
        })
    };

    // 1. Acting for the desktop.
    if input.task == "act" {
        if input.backend.is_some() || input.escalate || !input.routing.is_local("act") {
            return Err("the act task only runs on a local model".into());
        }
        return local(
            input.requested_model.unwrap_or(&route.model),
            "Acting for the desktop only runs on this computer.".into(),
        );
    }

    // 3. "Ask a stronger model".
    if input.escalate {
        if private {
            return Err(format!("This stays on this computer because {why_private}, so it cannot be sent to a stronger model elsewhere."));
        }
        if route.escalate_backend.is_empty() {
            return Err(
                "No stronger model is set for this slot. Choose one in Helix Models → Routing."
                    .into(),
            );
        }
        let backend = usable(input.routing, &route.escalate_backend, "the stronger model")?;
        return Ok(Decision {
            backend: backend.id.clone(),
            model: route.escalate_model.clone(),
            reason: format!(
                "You asked for a stronger model: {}.",
                label(input.routing, &backend.id)
            ),
        });
    }

    // A caller that names a backend.
    if let Some(id) = input.backend {
        if private {
            return Err(format!(
                "This stays on this computer because {why_private}, so it was not sent to {}.",
                label(input.routing, id)
            ));
        }
        let backend = usable(input.routing, id, "the chosen backend")?;
        let model = input
            .requested_model
            .map(str::to_string)
            .unwrap_or_else(|| {
                if route.backend == backend.id {
                    route.model.clone()
                } else {
                    "default".into()
                }
            });
        return Ok(Decision {
            backend: backend.id.clone(),
            model,
            reason: format!(
                "Chosen for this conversation: {}.",
                label(input.routing, &backend.id)
            ),
        });
    }

    // A model on this computer named by the caller.
    if let Some(model) = input.requested_model.filter(|m| !m.is_empty()) {
        return local(model, "A model on this computer was chosen.".into());
    }

    let routed_local = input.routing.is_local(input.task);
    // 2. Private content stays, whatever the slot says.
    if private && !routed_local {
        let model = local_for_private(input, "");
        return local(
            &model,
            format!(
                "Kept on this computer because {why_private}; it did not go to {}.",
                label(input.routing, &route.backend)
            ),
        );
    }
    if routed_local {
        let model = if private {
            local_for_private(input, &route.model)
        } else {
            route.model.clone()
        };
        // 4. Too long for the local model: the slot's stronger-model choice, if the person set one and the content may leave.
        let too_long = input.local_context > 0 && input.estimated_tokens > input.local_context;
        if too_long && !private && !route.escalate_backend.is_empty() {
            let backend = usable(input.routing, &route.escalate_backend, "the stronger model")?;
            return Ok(Decision {
                backend: backend.id.clone(),
                model: route.escalate_model.clone(),
                reason: format!(
                    "Too long for the model on this computer (about {} tokens; it holds {}): used {}.",
                    input.estimated_tokens,
                    input.local_context,
                    label(input.routing, &backend.id)
                ),
            });
        }
        let reason = if private {
            format!("Kept on this computer because {why_private}.")
        } else {
            "This slot runs on this computer.".into()
        };
        return local(&model, reason);
    }
    let backend = usable(
        input.routing,
        &route.backend,
        &format!("the {} slot's route", input.task),
    )?;
    Ok(Decision {
        backend: backend.id.clone(),
        model: route.model.clone(),
        reason: format!(
            "The {} slot is set to use {}.",
            input.task,
            label(input.routing, &backend.id)
        ),
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    fn table() -> RoutingTable {
        RoutingTable::parse(
            "[[backends]]\nid = \"lan\"\nkind = \"openai\"\nbase_url = \"http://studio:8080/v1\"\nlabel = \"Studio\"\n\
             [[backends]]\nid = \"odd\"\nkind = \"pigeon\"\n\
             [profiles.chat]\nbackend = \"local\"\nmodel = \"small.gguf\"\nescalate_backend = \"lan\"\nescalate_model = \"big\"\n\
             [profiles.code]\nbackend = \"lan\"\nmodel = \"big\"\n\
             [profiles.summarize]\nbackend = \"local\"\nmodel = \"small.gguf\"\n\
             [profiles.act]\nbackend = \"local\"\nmodel = \"act.gguf\"\n\
             [profiles.vision]\nbackend = \"lan\"\nmodel = \"big\"\n",
        )
    }

    fn models() -> Vec<ModelInfo> {
        vec![
            ModelInfo {
                id: "small.gguf".into(),
                bytes: 1,
                loaded: false,
                vision: false,
            },
            ModelInfo {
                id: "eyes.gguf".into(),
                bytes: 1,
                loaded: false,
                vision: true,
            },
        ]
    }

    fn input<'a>(
        task: &'a str,
        routing: &'a RoutingTable,
        local_models: &'a [ModelInfo],
    ) -> Input<'a> {
        Input {
            task,
            requested_model: None,
            backend: None,
            escalate: false,
            private: false,
            has_images: false,
            estimated_tokens: 100,
            local_context: 4096,
            local_models,
            routing,
        }
    }

    #[test]
    fn a_slot_goes_where_the_person_set_it_and_says_so() {
        let (r, m) = (table(), models());
        let d = decide(&input("chat", &r, &m)).unwrap();
        assert_eq!((d.backend.as_str(), d.model.as_str()), ("", "small.gguf"));
        assert!(d.reason.contains("this computer"));
        let d = decide(&input("code", &r, &m)).unwrap();
        assert_eq!((d.backend.as_str(), d.model.as_str()), ("lan", "big"));
        assert!(
            d.reason.contains("code slot") && d.reason.contains("Studio"),
            "{}",
            d.reason
        );
    }

    #[test]
    fn acting_for_the_desktop_never_leaves_this_computer() {
        let (r, m) = (table(), models());
        let mut i = input("act", &r, &m);
        assert_eq!(decide(&i).unwrap().model, "act.gguf");
        i.backend = Some("lan");
        assert!(decide(&i).is_err());
        let mut j = input("act", &r, &m);
        j.escalate = true;
        assert!(decide(&j).is_err());
    }

    #[test]
    fn private_content_stays_even_when_the_slot_is_routed_away() {
        let (r, m) = (table(), models());
        let mut i = input("vision", &r, &m);
        i.has_images = true;
        let d = decide(&i).unwrap();
        assert_eq!(
            (d.backend.as_str(), d.model.as_str()),
            ("", "eyes.gguf"),
            "a local model that reads pictures"
        );
        assert!(
            d.reason.contains("picture") && d.reason.contains("did not go to Studio"),
            "{}",
            d.reason
        );
        let mut p = input("code", &r, &m);
        p.private = true;
        assert_eq!(decide(&p).unwrap().backend, "");
        // Naming a remote backend, or asking for a stronger model, with private content is refused, with the reason.
        p.backend = Some("lan");
        assert!(decide(&p).unwrap_err().contains("stays on this computer"));
        let mut e = input("chat", &r, &m);
        e.private = true;
        e.escalate = true;
        assert!(decide(&e).unwrap_err().contains("cannot be sent"));
    }

    #[test]
    fn a_stronger_model_is_asked_for_by_name_and_must_be_set() {
        let (r, m) = (table(), models());
        let mut i = input("chat", &r, &m);
        i.escalate = true;
        let d = decide(&i).unwrap();
        assert_eq!((d.backend.as_str(), d.model.as_str()), ("lan", "big"));
        assert!(d.reason.starts_with("You asked for a stronger model"));
        let mut none = input("summarize", &r, &m);
        none.escalate = true;
        assert!(decide(&none)
            .unwrap_err()
            .contains("No stronger model is set"));
    }

    #[test]
    fn a_request_too_long_for_the_local_model_uses_the_stronger_one_only_if_set_and_allowed() {
        let (r, m) = (table(), models());
        let mut long = input("chat", &r, &m);
        long.estimated_tokens = 9000;
        let d = decide(&long).unwrap();
        assert_eq!(d.backend, "lan");
        assert!(
            d.reason.contains("Too long") && d.reason.contains("9000"),
            "{}",
            d.reason
        );
        // No stronger model set for the slot: it stays, and the engine says it does not fit.
        let mut plain = input("summarize", &r, &m);
        plain.estimated_tokens = 9000;
        assert_eq!(decide(&plain).unwrap().backend, "");
        // Private content never goes, however long.
        let mut private = input("chat", &r, &m);
        private.estimated_tokens = 9000;
        private.private = true;
        assert_eq!(decide(&private).unwrap().backend, "");
        // An unknown context size means no length rule.
        let mut unknown = input("chat", &r, &m);
        unknown.estimated_tokens = 90_000;
        unknown.local_context = 0;
        assert_eq!(decide(&unknown).unwrap().backend, "");
    }

    #[test]
    fn a_named_backend_or_model_is_honoured_and_a_broken_route_is_an_error() {
        let (r, m) = (table(), models());
        let mut named = input("chat", &r, &m);
        named.backend = Some("lan");
        named.requested_model = Some("mine");
        let d = decide(&named).unwrap();
        assert_eq!((d.backend.as_str(), d.model.as_str()), ("lan", "mine"));
        named.backend = Some("nowhere");
        assert!(decide(&named).is_err());
        named.backend = Some("odd");
        assert!(decide(&named).is_err());
        let mut local = input("chat", &r, &m);
        local.requested_model = Some("eyes.gguf");
        assert_eq!(decide(&local).unwrap().model, "eyes.gguf");
        let broken = RoutingTable::parse("[profiles.chat]\nbackend = \"missing\"\nmodel = \"x\"\n");
        assert!(decide(&input("chat", &broken, &m)).is_err());
    }
}
