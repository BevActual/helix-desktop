use smithay::backend::renderer::gles::Uniform;

/// Resolved liquid-glass lens parameters for one effect element.
#[derive(Debug, Default, Clone, Copy, PartialEq)]
pub struct LiquidGlassOptions {
    pub refraction_strength: f64,
    pub bezel_width: f64,
    pub ior: f64,
    pub fringing: f64,
    pub specular: f64,
    pub light_angle: f64,
    pub edge_shadow: f64,
    pub edge_padding: f64,
}

impl From<niri_config::LiquidGlass> for LiquidGlassOptions {
    fn from(config: niri_config::LiquidGlass) -> Self {
        Self {
            refraction_strength: config.refraction_strength,
            bezel_width: config.bezel_width,
            ior: config.ior,
            fringing: config.fringing,
            specular: config.specular,
            light_angle: config.light_angle,
            edge_shadow: config.edge_shadow,
            edge_padding: config.edge_padding,
        }
    }
}

/// Uniform names the liquid-glass block of `clipped_surface.frag` reads.
///
/// Every program built from that shader registers these, and every element emits them, so
/// the shader compiles and runs whether or not a `liquid-glass {}` child is present.
pub const UNIFORM_NAMES: &[&str] = &[
    "lg_strength",
    "lg_bezel",
    "lg_ior",
    "lg_fringing",
    "lg_specular",
    "lg_light",
    "lg_edge_shadow",
    "lg_padding_pixels",
];

/// Uniforms for `clipped_surface.frag`. `None` disables the effect (`lg_strength` 0 and
/// `lg_specular` 0 short-circuit the glass block).
pub fn liquid_glass_uniforms(
    lg: Option<&LiquidGlassOptions>,
    padding_pixels: f32,
) -> Vec<Uniform<'static>> {
    let Some(lg) = lg else {
        return vec![
            Uniform::new("lg_strength", 0.0f32),
            Uniform::new("lg_bezel", 0.0f32),
            Uniform::new("lg_ior", 1.5f32),
            Uniform::new("lg_fringing", 0.0f32),
            Uniform::new("lg_specular", 0.0f32),
            Uniform::new("lg_light", [0.0f32, -1.0]),
            Uniform::new("lg_edge_shadow", 0.0f32),
            Uniform::new("lg_padding_pixels", padding_pixels),
        ];
    };

    // Unit vector pointing from the shape toward the light, screen space (y down).
    // 0° is straight up; positive turns clockwise.
    let angle = lg.light_angle.to_radians();
    let light = [angle.sin() as f32, -(angle.cos() as f32)];

    vec![
        Uniform::new("lg_strength", lg.refraction_strength as f32),
        Uniform::new("lg_bezel", lg.bezel_width as f32),
        Uniform::new("lg_ior", lg.ior.max(1.0) as f32),
        Uniform::new("lg_fringing", lg.fringing as f32),
        Uniform::new("lg_specular", lg.specular as f32),
        Uniform::new("lg_light", light),
        Uniform::new("lg_edge_shadow", lg.edge_shadow as f32),
        Uniform::new("lg_padding_pixels", padding_pixels),
    ]
}
