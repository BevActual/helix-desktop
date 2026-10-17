//! Choosing one of a few named routes by how close a message is to each route's example sentences.

use serde::{Deserialize, Serialize};

/// A route and the sentences that say what belongs to it.
#[derive(Clone, Debug, Serialize, Deserialize)]
pub struct Route {
    pub name: String,
    pub examples: Vec<String>,
}

/// The answer: the best route, every route's similarity (best first), and the gap between the best and the second, which is the
/// confidence. A small gap means "not sure"; the caller decides what that is worth.
#[derive(Clone, Debug, PartialEq)]
pub struct Choice {
    pub route: String,
    pub scores: Vec<(String, f32)>,
    pub margin: f32,
}

/// One unit vector per route (the normalised mean of its examples' vectors), so a decision is a few dot products.
#[derive(Clone, Debug)]
pub struct Decider {
    names: Vec<String>,
    centroids: Vec<Vec<f32>>,
}

fn normalise(v: &mut [f32]) {
    let norm = v.iter().map(|x| x * x).sum::<f32>().sqrt();
    if norm > 0.0 {
        v.iter_mut().for_each(|x| *x /= norm);
    }
}

impl Decider {
    /// `examples[i]` holds the vectors of `routes[i]`'s examples (all the same width). Routes without examples are left out.
    pub fn new(routes: &[Route], examples: &[Vec<Vec<f32>>]) -> Self {
        let mut names = Vec::new();
        let mut centroids = Vec::new();
        for (route, vectors) in routes.iter().zip(examples) {
            let Some(first) = vectors.first() else {
                continue;
            };
            let mut mean = vec![0.0f32; first.len()];
            for v in vectors {
                for (m, x) in mean.iter_mut().zip(v) {
                    *m += x;
                }
            }
            normalise(&mut mean);
            names.push(route.name.clone());
            centroids.push(mean);
        }
        Self { names, centroids }
    }

    /// `None` when there are no routes.
    pub fn decide(&self, vector: &[f32]) -> Option<Choice> {
        let mut scores: Vec<(String, f32)> = self
            .names
            .iter()
            .zip(&self.centroids)
            .map(|(name, c)| (name.clone(), c.iter().zip(vector).map(|(a, b)| a * b).sum()))
            .collect();
        scores.sort_by(|a, b| b.1.total_cmp(&a.1));
        let best = scores.first()?.clone();
        let margin = best.1 - scores.get(1).map_or(0.0, |s| s.1);
        Some(Choice {
            route: best.0,
            scores,
            margin,
        })
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn picks_the_nearest_route_and_reports_the_gap() {
        let routes = [
            Route {
                name: "left".into(),
                examples: vec!["a".into()],
            },
            Route {
                name: "up".into(),
                examples: vec!["b".into()],
            },
        ];
        let decider = Decider::new(&routes, &[vec![vec![1.0, 0.0]], vec![vec![0.0, 1.0]]]);
        let choice = decider.decide(&[0.9, 0.1]).unwrap();
        assert_eq!(choice.route, "left");
        assert!((choice.margin - 0.8).abs() < 1e-6);
        let unsure = decider.decide(&[0.7071, 0.7071]).unwrap();
        assert!(unsure.margin < 0.01);
    }

    #[test]
    fn no_routes_means_no_choice() {
        assert!(Decider::new(&[], &[]).decide(&[1.0]).is_none());
    }
}
