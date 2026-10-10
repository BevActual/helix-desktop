#version 100

//_DEFINES_

#if defined(EXTERNAL)
#extension GL_OES_EGL_image_external : require
#endif

precision highp float;
#if defined(EXTERNAL)
uniform samplerExternalOES tex;
#else
uniform sampler2D tex;
#endif

uniform float alpha;
varying vec2 v_coords;

#if defined(DEBUG_FLAGS)
uniform float tint;
#endif

uniform float niri_scale;

uniform vec2 geo_size;
uniform vec4 corner_radius;
uniform mat3 input_to_geo;

// ============================================================================
// Liquid Glass (Helix)
//
// The surface is a glass slab. Its flat interior passes the backdrop through
// unchanged; its edge is a convex bezel `lg_bezel` px wide whose height follows
// a quarter circle, so the surface normal tilts outward toward the rim. A ray
// arriving straight from the viewer refracts at that tilted surface (Snell,
// `lg_ior`) and lands on the backdrop beyond the edge: the rim shows a
// compressed band of what lies just outside the shape. Red and blue refract a
// little differently (`lg_fringing`). A light at `lg_light` puts a specular
// highlight on the rim facing it, a fainter one opposite (the far wall of the
// slab), a hairline along the whole edge, and an inner shadow where the rim
// turns away (`lg_edge_shadow`).
//
// All distances are logical pixels of the shape. `geo_size` is the shape (or
// the padded area when `lg_padding_pixels` > 0); `corner_radius` is niri order
// [top-left, top-right, bottom-right, bottom-left].
// ============================================================================

uniform float lg_strength;
uniform float lg_bezel;
uniform float lg_ior;
uniform float lg_fringing;
uniform float lg_specular;
uniform vec2 lg_light;
uniform float lg_edge_shadow;
uniform float lg_padding_pixels;

float niri_rounding_alpha(vec2 coords, vec2 size, vec4 corner_radius);
vec4 postprocess(vec4 color);

// Signed distance to a rounded rectangle centred at the origin with half-size b.
// p is screen space (y down); r is [TL, TR, BR, BL].
float lg_sdf(vec2 p, vec2 b, vec4 r) {
    float radius = p.x < 0.0
        ? (p.y < 0.0 ? r.x : r.w)
        : (p.y < 0.0 ? r.y : r.z);
    vec2 q = abs(p) - b + radius;
    return min(max(q.x, q.y), 0.0) + length(max(q, 0.0)) - radius;
}

// Outward unit normal of the SDF at p.
vec2 lg_normal(vec2 p, vec2 b, vec4 r) {
    const float h = 0.75;
    vec2 g = vec2(
        lg_sdf(p + vec2(h, 0.0), b, r) - lg_sdf(p - vec2(h, 0.0), b, r),
        lg_sdf(p + vec2(0.0, h), b, r) - lg_sdf(p - vec2(0.0, h), b, r)
    );
    float len = length(g);
    return len > 1e-4 ? g / len : vec2(0.0, -1.0);
}

// Lateral displacement (px) of a straight-on ray through a slab of thickness t
// whose surface is tilted by theta, for index n. Snell: sin(theta') = sin(theta)/n.
float lg_snell_shift(float theta, float n, float t) {
    float sinIn = sin(theta);
    float sinOut = clamp(sinIn / n, -1.0, 1.0);
    float deviation = theta - asin(sinOut);
    return t * tan(deviation);
}

vec4 lg_sample(vec2 uv_tex, vec2 uvPerPx, vec2 dirPx) {
    return texture2D(tex, clamp(uv_tex + dirPx * uvPerPx, 0.0, 1.0));
}

// windowUV: 0..1 across the shape. uv_tex: backdrop UV for texture sampling.
vec4 lg_glass(vec2 uv_tex, vec2 windowUV, vec4 base, vec2 shapeSize, vec4 radii) {
    vec2 hs = shapeSize * 0.5;
    vec2 p = windowUV * shapeSize - hs;
    float d = lg_sdf(p, hs, radii);
    if (d >= 0.0) {
        return base;
    }

    // Bezel width follows the corner radius so capsules read as fully lensed and
    // large sheets keep a proportionate rim.
    float minR = min(min(radii.x, radii.y), min(radii.z, radii.w));
    float bezel = lg_bezel > 0.5 ? lg_bezel : clamp(minR * 1.25, 3.0, 48.0);
    bezel = min(bezel, min(hs.x, hs.y));

    // 0 at the rim, 1 where the bezel meets the flat interior.
    float u = clamp(-d / bezel, 0.0, 1.0);
    float rim = 1.0 - u;
    // Quarter-circle bezel: the normal tilts by asin(rim). Cap the tilt so the
    // outermost pixels stay finite and anti-aliasable.
    float theta = asin(min(rim, 0.985));
    theta = min(theta, radians(78.0));

    vec2 nrm = lg_normal(p, hs, radii);

    // Backdrop UVs per shape pixel: input_to_geo maps texture UV to geometry UV.
    vec2 texScale = vec2(input_to_geo[0][0], input_to_geo[1][1]);
    vec2 uvPerPx = 1.0 / (geo_size * texScale);

    vec4 color = base;
    if (lg_strength > 0.0001 && rim > 0.0001) {
        // The slab is deeper than the bezel is wide: that depth is what turns a
        // gentle surface tilt into a visible band of compressed outside content.
        float thickness = bezel * 3.0;
        float shift = lg_snell_shift(theta, lg_ior, thickness) * lg_strength;
        if (lg_fringing > 0.0001) {
            float dn = 0.04 * lg_fringing;
            float shiftR = lg_snell_shift(theta, lg_ior - dn, thickness) * lg_strength;
            float shiftB = lg_snell_shift(theta, lg_ior + dn, thickness) * lg_strength;
            vec4 g = lg_sample(uv_tex, uvPerPx, nrm * shift);
            color = vec4(
                lg_sample(uv_tex, uvPerPx, nrm * shiftR).r,
                g.g,
                lg_sample(uv_tex, uvPerPx, nrm * shiftB).b,
                g.a
            );
        } else {
            color = lg_sample(uv_tex, uvPerPx, nrm * shift);
        }
    }

    if (lg_specular > 0.0001 || lg_edge_shadow > 0.0001) {
        // Slope of the bezel drives how much light the rim catches.
        float slope = sin(theta);
        float facing = dot(nrm, lg_light);

        // Highlight toward the light, a softer one on the far wall.
        float toward = smoothstep(0.0, 1.0, facing);
        float away = smoothstep(0.0, 1.0, -facing);
        float band = pow(rim, 2.5);
        float highlight = band * slope * (0.55 * toward * toward + 0.18 * away * away);
        // Hairline: the polished edge itself, one pixel wide, all the way round.
        float hair = (1.0 - smoothstep(0.0, 1.5 * niri_scale, -d)) * 0.28;
        highlight = (highlight + hair) * lg_specular;
        // Screen blend keeps bright backdrops from clipping.
        color.rgb += highlight * (color.a - color.rgb);

        // Inner shadow on the side turned from the light.
        float shade = lg_edge_shadow * band * slope * away * 0.45;
        color.rgb *= 1.0 - shade;
    }

    return color;
}

void main() {
    vec3 coords_geo = input_to_geo * vec3(v_coords, 1.0);

    vec4 color = texture2D(tex, v_coords);
#if defined(NO_ALPHA)
    color = vec4(color.rgb, 1.0);
#endif

    // Convert expanded UV to window UV when padding is active
    // geo_size is the EXPANDED size (original window + 2*padding).
    // coords_geo is [0,1] within the expanded area.
    // We need windowUV = [0,1] within the ORIGINAL window.
    vec2 windowUV = coords_geo.xy;
    if (lg_padding_pixels > 0.5) {
        windowUV = (coords_geo.xy * geo_size - vec2(lg_padding_pixels)) / (geo_size - vec2(lg_padding_pixels * 2.0));
    }

    // Clip to the shape (not the padded area).
    float insideGeo = step(0.0, windowUV.x) * step(windowUV.x, 1.0)
                     * step(0.0, windowUV.y) * step(windowUV.y, 1.0);
    float glassOn = step(0.0001, lg_strength + lg_specular + lg_edge_shadow);

    if (insideGeo * glassOn > 0.0) {
        vec2 shapeSize = geo_size - vec2(lg_padding_pixels * 2.0);
        color = lg_glass(v_coords, windowUV, color, shapeSize, corner_radius);
    }

    color = postprocess(color);

    color = color * niri_rounding_alpha(windowUV * geo_size, geo_size, corner_radius)
                  * insideGeo;

    color = color * alpha;

#if defined(DEBUG_FLAGS)
    if (tint == 1.0)
        color = vec4(0.0, 0.2, 0.0, 0.2) + color * 0.8;
#endif

    gl_FragColor = color;
}
