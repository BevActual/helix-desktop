//! HCT colour space (CAM16 hue/chroma + L* tone) and tonal palettes, ported from
//! `third_party/material_color_utilities` (`cpp/cam`, `cpp/palettes/tones`, `cpp/utils`).

use std::collections::HashMap;

pub type Argb = u32;

const PI: f64 = 3.141592653589793;

#[derive(Clone, Copy)]
struct Vec3 {
    a: f64,
    b: f64,
    c: f64,
}

// ---- utils ------------------------------------------------------------------------------

fn linearized(component: i32) -> f64 {
    let normalized = f64::from(component) / 255.0;
    if normalized <= 0.040449936 {
        normalized / 12.92 * 100.0
    } else {
        ((normalized + 0.055) / 1.055).powf(2.4) * 100.0
    }
}

fn delinearized(component: f64) -> i32 {
    let normalized = component / 100.0;
    let d = if normalized <= 0.0031308 {
        normalized * 12.92
    } else {
        1.055 * normalized.powf(1.0 / 2.4) - 0.055
    };
    ((d * 255.0).round() as i32).clamp(0, 255)
}

fn argb_from_rgb(r: i32, g: i32, b: i32) -> Argb {
    0xFF00_0000 | (((r & 0xff) as u32) << 16) | (((g & 0xff) as u32) << 8) | ((b & 0xff) as u32)
}

fn argb_from_linrgb(v: Vec3) -> Argb {
    argb_from_rgb(delinearized(v.a), delinearized(v.b), delinearized(v.c))
}

fn sanitize_degrees(degrees: f64) -> f64 {
    if degrees < 0.0 {
        (degrees % 360.0) + 360.0
    } else if degrees >= 360.0 {
        degrees % 360.0
    } else {
        degrees
    }
}

fn signum(n: f64) -> f64 {
    if n < 0.0 {
        -1.0
    } else if n == 0.0 {
        0.0
    } else {
        1.0
    }
}

fn y_from_lstar(lstar: f64) -> f64 {
    const KE: f64 = 8.0;
    if lstar > KE {
        let cube_root = (lstar + 16.0) / 116.0;
        cube_root * cube_root * cube_root * 100.0
    } else {
        lstar / (24389.0 / 27.0) * 100.0
    }
}

fn lstar_from_y(y: f64) -> f64 {
    const E: f64 = 216.0 / 24389.0;
    let yn = y / 100.0;
    if yn <= E {
        (24389.0 / 27.0) * yn
    } else {
        116.0 * yn.powf(1.0 / 3.0) - 16.0
    }
}

/// L* of an ARGB colour (`LstarFromArgb`).
pub fn lstar_from_argb(argb: Argb) -> f64 {
    let r = linearized(((argb & 0x00ff_0000) >> 16) as i32);
    let g = linearized(((argb & 0x0000_ff00) >> 8) as i32);
    let b = linearized((argb & 0x0000_00ff) as i32);
    lstar_from_y(0.2126 * r + 0.7152 * g + 0.0722 * b)
}

fn int_from_lstar(lstar: f64) -> Argb {
    let c = delinearized(y_from_lstar(lstar));
    argb_from_rgb(c, c, c)
}

fn matrix_multiply(input: Vec3, m: &[[f64; 3]; 3]) -> Vec3 {
    Vec3 {
        a: input.a * m[0][0] + input.b * m[0][1] + input.c * m[0][2],
        b: input.a * m[1][0] + input.b * m[1][1] + input.c * m[1][2],
        c: input.a * m[2][0] + input.b * m[2][1] + input.c * m[2][2],
    }
}

// ---- CAM16 --------------------------------------------------------------------------------

struct ViewingConditions {
    background_y_to_white_point_y: f64,
    aw: f64,
    nbb: f64,
    ncb: f64,
    c: f64,
    n_c: f64,
    fl: f64,
    z: f64,
    rgb_d: [f64; 3],
}

/// `kDefaultViewingConditions`.
const DEFAULT_VIEWING: ViewingConditions = ViewingConditions {
    background_y_to_white_point_y: 0.184186503,
    aw: 29.981000900,
    nbb: 1.016919255,
    ncb: 1.016919255,
    c: 0.689999998,
    n_c: 1.000000000,
    fl: 0.388481468,
    z: 1.909169555,
    rgb_d: [1.021177769, 0.986307740, 0.933960497],
};

/// The CAM16 correlates HCT needs.
#[derive(Clone, Copy, Debug)]
pub struct Cam {
    pub hue: f64,
    pub chroma: f64,
}

pub fn cam_from_int(argb: Argb) -> Cam {
    let vc = &DEFAULT_VIEWING;
    let red_l = linearized(((argb & 0x00ff_0000) >> 16) as i32);
    let green_l = linearized(((argb & 0x0000_ff00) >> 8) as i32);
    let blue_l = linearized((argb & 0x0000_00ff) as i32);
    let x = 0.41233895 * red_l + 0.35762064 * green_l + 0.18051042 * blue_l;
    let y = 0.2126 * red_l + 0.7152 * green_l + 0.0722 * blue_l;
    let z = 0.01932141 * red_l + 0.11916382 * green_l + 0.95034478 * blue_l;

    let r_c = 0.401288 * x + 0.650173 * y - 0.051461 * z;
    let g_c = -0.250268 * x + 1.204414 * y + 0.045854 * z;
    let b_c = -0.002079 * x + 0.048952 * y + 0.953127 * z;

    let r_d = vc.rgb_d[0] * r_c;
    let g_d = vc.rgb_d[1] * g_c;
    let b_d = vc.rgb_d[2] * b_c;

    let r_af = (vc.fl * r_d.abs() / 100.0).powf(0.42);
    let g_af = (vc.fl * g_d.abs() / 100.0).powf(0.42);
    let b_af = (vc.fl * b_d.abs() / 100.0).powf(0.42);
    let r_a = signum(r_d) * 400.0 * r_af / (r_af + 27.13);
    let g_a = signum(g_d) * 400.0 * g_af / (g_af + 27.13);
    let b_a = signum(b_d) * 400.0 * b_af / (b_af + 27.13);

    let a = (11.0 * r_a + -12.0 * g_a + b_a) / 11.0;
    let b = (r_a + g_a - 2.0 * b_a) / 9.0;
    let u = (20.0 * r_a + 20.0 * g_a + 21.0 * b_a) / 20.0;
    let p2 = (40.0 * r_a + 20.0 * g_a + b_a) / 20.0;

    let degrees = b.atan2(a) * 180.0 / PI;
    let hue = sanitize_degrees(degrees);
    let ac = p2 * vc.nbb;

    let j = 100.0 * (ac / vc.aw).powf(vc.c * vc.z);
    let hue_prime = if hue < 20.14 { hue + 360.0 } else { hue };
    let e_hue = 0.25 * ((hue_prime * PI / 180.0 + 2.0).cos() + 3.8);
    let p1 = 50000.0 / 13.0 * e_hue * vc.n_c * vc.ncb;
    let t = p1 * (a * a + b * b).sqrt() / (u + 0.305);
    let alpha = t.powf(0.9) * (1.64 - 0.29_f64.powf(vc.background_y_to_white_point_y)).powf(0.73);
    let c = alpha * (j / 100.0).sqrt();
    Cam { hue, chroma: c }
}

// ---- solver ---------------------------------------------------------------------------------

const SCALED_DISCOUNT_FROM_LINRGB: [[f64; 3]; 3] = [
    [0.001200833568784504, 0.002389694492170889, 0.0002795742885861124],
    [0.0005891086651375999, 0.0029785502573438758, 0.0003270666104008398],
    [0.00010146692491640572, 0.0005364214359186694, 0.0032979401770712076],
];

const LINRGB_FROM_SCALED_DISCOUNT: [[f64; 3]; 3] = [
    [1373.2198709594231, -1100.4251190754821, -7.278681089101213],
    [-271.815969077903, 559.6580465940733, -32.46047482791194],
    [1.9622899599665666, -57.173814538844006, 308.7233197812385],
];

const Y_FROM_LINRGB: [f64; 3] = [0.2126, 0.7152, 0.0722];

const CRITICAL_PLANES: [f64; 255] = [
    0.015176349177441876, 0.045529047532325624, 0.07588174588720938, 0.10623444424209313, 
    0.13658714259697685, 0.16693984095186062, 0.19729253930674434, 0.2276452376616281, 
    0.2579979360165119, 0.28835063437139563, 0.3188300904430532, 0.350925934958123, 0.3848314933096426, 
    0.42057480301049466, 0.458183274052838, 0.4976837250274023, 0.5391024159806381, 0.5824650784040898, 
    0.6277969426914107, 0.6751227633498623, 0.7244668422128921, 0.775853049866786, 0.829304845476233, 
    0.8848452951698498, 0.942497089126609, 1.0022825574869039, 1.0642236851973577, 1.1283421258858297, 
    1.1946592148522128, 1.2631959812511864, 1.3339731595349034, 1.407011200216447, 1.4823302800086415, 
    1.5599503113873272, 1.6398909516233677, 1.7221716113234105, 1.8068114625156377, 1.8938294463134073, 
    1.9832442801866852, 2.075074464868551, 2.1693382909216234, 2.2660538449872063, 2.36523901573795, 
    2.4669114995532007, 2.5710888059345764, 2.6777882626779785, 2.7870270208169257, 2.898822059350997, 
    3.0131901897720907, 3.1301480604002863, 3.2497121605402226, 3.3718988244681087, 3.4967242352587946, 
    3.624204428461639, 3.754355295633311, 3.887192587735158, 4.022731918402185, 4.160988767090289, 
    4.301978482107941, 4.445716283538092, 4.592217266055746, 4.741496401646282, 4.893568542229298, 
    5.048448422192488, 5.20615066083972, 5.3666897647573375, 5.5300801301023865, 5.696336044816294, 
    5.865471690767354, 6.037501145825082, 6.212438385869475, 6.390297286737924, 6.571091626112461, 
    6.7548350853498045, 6.941541251256611, 7.131223617812143, 7.323895587840543, 7.5195704746346665, 
    7.7182615035334345, 7.919981813454504, 8.124744458384042, 8.332562408825165, 8.543448553206703, 
    8.757415699253682, 8.974476575321063, 9.194643831691977, 9.417930041841839, 9.644347703669503, 
    9.873909240696694, 10.106627003236781, 10.342513269534024, 10.58158024687427, 10.8238400726681, 
    11.069304815507364, 11.317986476196008, 11.569896988756009, 11.825048221409341, 12.083451977536606, 
    12.345119996613247, 12.610063955123938, 12.878295467455942, 13.149826086772048, 13.42466730586372, 
    13.702830557985108, 13.984327217668513, 14.269168601521828, 14.55736596900856, 14.848930523210871, 
    15.143873411576273, 15.44220572664832, 15.743938506781891, 16.04908273684337, 16.35764934889634, 
    16.66964922287304, 16.985093187232053, 17.30399201960269, 17.62635644741625, 17.95219714852476, 
    18.281524751807332, 18.614349837764564, 18.95068293910138, 19.290534541298456, 19.633915083172692, 
    19.98083495742689, 20.331304511189067, 20.685334046541502, 21.042933821039977, 21.404114048223256, 
    21.76888489811322, 22.137256497705877, 22.50923893145328, 22.884842241736916, 23.264076429332462, 
    23.6469514538663, 24.033477234264016, 24.42366364919083, 24.817520537484558, 25.21505769858089, 
    25.61628489293138, 26.021211842414342, 26.429848230738664, 26.842203703840827, 27.258287870275353, 
    27.678110301598522, 28.10168053274597, 28.529008062403893, 28.96010235337422, 29.39497283293396, 
    29.83362889318845, 30.276079891419332, 30.722335150426627, 31.172403958865512, 31.62629557157785, 
    32.08401920991837, 32.54558406207592, 33.010999283389665, 33.4802739966603, 33.953417292456834, 
    34.430438229418264, 34.911345834551085, 35.39614910352207, 35.88485700094671, 36.37747846067349, 
    36.87402238606382, 37.37449765026789, 37.87891309649659, 38.38727753828926, 38.89959975977785, 
    39.41588851594697, 39.93615253289054, 40.460400508064545, 40.98864111053629, 41.520882981230194, 
    42.05713473317016, 42.597404951718396, 43.141702194811224, 43.6900349931913, 44.24241185063697, 
    44.798841244188324, 45.35933162437017, 45.92389141541209, 46.49252901546552, 47.065252796817916, 
    47.64207110610409, 48.22299226451468, 48.808024568002054, 49.3971762874833, 49.9904556690408, 
    50.587870934119984, 51.189430279724725, 51.79514187861014, 52.40501387947288, 53.0190544071392, 
    53.637271562750364, 54.259673423945976, 54.88626804504493, 55.517063457223934, 56.15206766869424, 
    56.79128866487574, 57.43473440856916, 58.08241284012621, 58.734331877617365, 59.39049941699807, 
    60.05092333227251, 60.715611475655585, 61.38457167773311, 62.057811747619894, 62.7353394731159, 
    63.417162620860914, 64.10328893648692, 64.79372614476921, 65.48848194977529, 66.18756403501224, 
    66.89098006357258, 67.59873767827808, 68.31084450182222, 69.02730813691093, 69.74813616640164, 
    70.47333615344107, 71.20291564160104, 71.93688215501312, 72.67524319850172, 73.41800625771542, 
    74.16517879925733, 74.9167682708136, 75.67278210128072, 76.43322770089146, 77.1981124613393, 
    77.96744375590167, 78.74122893956174, 79.51947534912904, 80.30219030335869, 81.08938110306934, 
    81.88105503125999, 82.67721935322541, 83.4778813166706, 84.28304815182372, 85.09272707154808, 
    85.90692527145302, 86.72564993000343, 87.54890820862819, 88.3767072518277, 89.2090541872801, 
    90.04595612594655, 90.88742016217518, 91.73345337380438, 92.58406282226491, 93.43925555268066, 
    94.29903859396902, 95.16341895893969, 96.03240364439274, 96.9059996312159, 97.78421388448044, 
    98.6670533535366, 99.55452497210776
];

fn sanitize_radians(angle: f64) -> f64 {
    (angle + PI * 8.0) % (PI * 2.0)
}

fn true_delinearized(component: f64) -> f64 {
    let n = component / 100.0;
    let d = if n <= 0.0031308 { n * 12.92 } else { 1.055 * n.powf(1.0 / 2.4) - 0.055 };
    d * 255.0
}

fn chromatic_adaptation(component: f64) -> f64 {
    let af = component.abs().powf(0.42);
    signum(component) * 400.0 * af / (af + 27.13)
}

fn hue_of(linrgb: Vec3) -> f64 {
    let sd = matrix_multiply(linrgb, &SCALED_DISCOUNT_FROM_LINRGB);
    let r_a = chromatic_adaptation(sd.a);
    let g_a = chromatic_adaptation(sd.b);
    let b_a = chromatic_adaptation(sd.c);
    let a = (11.0 * r_a + -12.0 * g_a + b_a) / 11.0;
    let b = (r_a + g_a - 2.0 * b_a) / 9.0;
    b.atan2(a)
}

fn are_in_cyclic_order(a: f64, b: f64, c: f64) -> bool {
    sanitize_radians(b - a) < sanitize_radians(c - a)
}

fn intercept(source: f64, mid: f64, target: f64) -> f64 {
    (mid - source) / (target - source)
}

fn lerp_point(s: Vec3, t: f64, e: Vec3) -> Vec3 {
    Vec3 { a: s.a + (e.a - s.a) * t, b: s.b + (e.b - s.b) * t, c: s.c + (e.c - s.c) * t }
}

fn get_axis(v: Vec3, axis: usize) -> f64 {
    match axis {
        0 => v.a,
        1 => v.b,
        2 => v.c,
        _ => -1.0,
    }
}

fn set_coordinate(source: Vec3, coordinate: f64, target: Vec3, axis: usize) -> Vec3 {
    let t = intercept(get_axis(source, axis), coordinate, get_axis(target, axis));
    lerp_point(source, t, target)
}

fn is_bounded(x: f64) -> bool {
    (0.0..=100.0).contains(&x)
}

fn nth_vertex(y: f64, n: i32) -> Vec3 {
    let (k_r, k_g, k_b) = (Y_FROM_LINRGB[0], Y_FROM_LINRGB[1], Y_FROM_LINRGB[2]);
    let coord_a = if n % 4 <= 1 { 0.0 } else { 100.0 };
    let coord_b = if n % 2 == 0 { 0.0 } else { 100.0 };
    let none = Vec3 { a: -1.0, b: -1.0, c: -1.0 };
    if n < 4 {
        let (g, b) = (coord_a, coord_b);
        let r = (y - g * k_g - b * k_b) / k_r;
        if is_bounded(r) { Vec3 { a: r, b: g, c: b } } else { none }
    } else if n < 8 {
        let (b, r) = (coord_a, coord_b);
        let g = (y - r * k_r - b * k_b) / k_g;
        if is_bounded(g) { Vec3 { a: r, b: g, c: b } } else { none }
    } else {
        let (r, g) = (coord_a, coord_b);
        let b = (y - r * k_r - g * k_g) / k_b;
        if is_bounded(b) { Vec3 { a: r, b: g, c: b } } else { none }
    }
}

fn bisect_to_segment(y: f64, target_hue: f64) -> [Vec3; 2] {
    let mut left = Vec3 { a: -1.0, b: -1.0, c: -1.0 };
    let mut right = left;
    let (mut left_hue, mut right_hue) = (0.0, 0.0);
    let mut initialized = false;
    let mut uncut = true;
    for n in 0..12 {
        let mid = nth_vertex(y, n);
        if mid.a < 0.0 {
            continue;
        }
        let mid_hue = hue_of(mid);
        if !initialized {
            left = mid;
            right = mid;
            left_hue = mid_hue;
            right_hue = mid_hue;
            initialized = true;
            continue;
        }
        if uncut || are_in_cyclic_order(left_hue, mid_hue, right_hue) {
            uncut = false;
            if are_in_cyclic_order(left_hue, target_hue, mid_hue) {
                right = mid;
                right_hue = mid_hue;
            } else {
                left = mid;
                left_hue = mid_hue;
            }
        }
    }
    [left, right]
}

fn midpoint(a: Vec3, b: Vec3) -> Vec3 {
    Vec3 { a: (a.a + b.a) / 2.0, b: (a.b + b.b) / 2.0, c: (a.c + b.c) / 2.0 }
}

fn bisect_to_limit(y: f64, target_hue: f64) -> Vec3 {
    let [mut left, mut right] = bisect_to_segment(y, target_hue);
    let mut left_hue = hue_of(left);
    for axis in 0..3 {
        if get_axis(left, axis) != get_axis(right, axis) {
            let (mut l_plane, mut r_plane);
            if get_axis(left, axis) < get_axis(right, axis) {
                l_plane = (true_delinearized(get_axis(left, axis)) - 0.5).floor() as i32;
                r_plane = (true_delinearized(get_axis(right, axis)) - 0.5).ceil() as i32;
            } else {
                l_plane = (true_delinearized(get_axis(left, axis)) - 0.5).ceil() as i32;
                r_plane = (true_delinearized(get_axis(right, axis)) - 0.5).floor() as i32;
            }
            for _ in 0..8 {
                if (r_plane - l_plane).abs() <= 1 {
                    break;
                }
                let m_plane = ((f64::from(l_plane) + f64::from(r_plane)) / 2.0).floor() as i32;
                let mid_plane_coordinate = CRITICAL_PLANES[m_plane as usize];
                let mid = set_coordinate(left, mid_plane_coordinate, right, axis);
                let mid_hue = hue_of(mid);
                if are_in_cyclic_order(left_hue, target_hue, mid_hue) {
                    right = mid;
                    r_plane = m_plane;
                } else {
                    left = mid;
                    left_hue = mid_hue;
                    l_plane = m_plane;
                }
            }
        }
    }
    midpoint(left, right)
}

fn inverse_chromatic_adaptation(adapted: f64) -> f64 {
    let abs = adapted.abs();
    let base = f64::max(0.0, 27.13 * abs / (400.0 - abs));
    signum(adapted) * base.powf(1.0 / 0.42)
}

fn find_result_by_j(hue_radians: f64, chroma: f64, y: f64) -> Argb {
    let vc = &DEFAULT_VIEWING;
    let mut j = y.sqrt() * 11.0;
    let t_inner_coeff = 1.0 / (1.64 - 0.29_f64.powf(vc.background_y_to_white_point_y)).powf(0.73);
    let e_hue = 0.25 * ((hue_radians + 2.0).cos() + 3.8);
    let p1 = e_hue * (50000.0 / 13.0) * vc.n_c * vc.ncb;
    let h_sin = hue_radians.sin();
    let h_cos = hue_radians.cos();
    for round in 0..5 {
        let j_normalized = j / 100.0;
        let alpha = if chroma == 0.0 || j == 0.0 { 0.0 } else { chroma / j_normalized.sqrt() };
        let t = (alpha * t_inner_coeff).powf(1.0 / 0.9);
        let ac = vc.aw * j_normalized.powf(1.0 / vc.c / vc.z);
        let p2 = ac / vc.nbb;
        let gamma = 23.0 * (p2 + 0.305) * t / (23.0 * p1 + 11.0 * t * h_cos + 108.0 * t * h_sin);
        let a = gamma * h_cos;
        let b = gamma * h_sin;
        let r_a = (460.0 * p2 + 451.0 * a + 288.0 * b) / 1403.0;
        let g_a = (460.0 * p2 - 891.0 * a - 261.0 * b) / 1403.0;
        let b_a = (460.0 * p2 - 220.0 * a - 6300.0 * b) / 1403.0;
        let scaled = Vec3 {
            a: inverse_chromatic_adaptation(r_a),
            b: inverse_chromatic_adaptation(g_a),
            c: inverse_chromatic_adaptation(b_a),
        };
        let linrgb = matrix_multiply(scaled, &LINRGB_FROM_SCALED_DISCOUNT);
        if linrgb.a < 0.0 || linrgb.b < 0.0 || linrgb.c < 0.0 {
            return 0;
        }
        let fnj = Y_FROM_LINRGB[0] * linrgb.a + Y_FROM_LINRGB[1] * linrgb.b + Y_FROM_LINRGB[2] * linrgb.c;
        if fnj <= 0.0 {
            return 0;
        }
        if round == 4 || (fnj - y).abs() < 0.002 {
            if linrgb.a > 100.01 || linrgb.b > 100.01 || linrgb.c > 100.01 {
                return 0;
            }
            return argb_from_linrgb(linrgb);
        }
        j -= (fnj - y) * j / (2.0 * fnj);
    }
    0
}

/// `SolveToInt`: sRGB colour with the given hue, chroma and L*, chroma maximised if out of gamut.
pub fn solve_to_int(hue_degrees: f64, chroma: f64, lstar: f64) -> Argb {
    if chroma < 0.0001 || lstar < 0.0001 || lstar > 99.9999 {
        return int_from_lstar(lstar);
    }
    let hue_degrees = sanitize_degrees(hue_degrees);
    let hue_radians = hue_degrees / 180.0 * PI;
    let y = y_from_lstar(lstar);
    let exact = find_result_by_j(hue_radians, chroma, y);
    if exact != 0 {
        return exact;
    }
    argb_from_linrgb(bisect_to_limit(y, hue_radians))
}

// ---- Hct ------------------------------------------------------------------------------------

#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Hct {
    argb: Argb,
    hue: f64,
    chroma: f64,
    tone: f64,
}

impl Hct {
    pub fn from_argb(argb: Argb) -> Self {
        let cam = cam_from_int(argb);
        Self { argb, hue: cam.hue, chroma: cam.chroma, tone: lstar_from_argb(argb) }
    }

    pub fn new(hue: f64, chroma: f64, tone: f64) -> Self {
        Self::from_argb(solve_to_int(hue, chroma, tone))
    }

    pub fn hue(&self) -> f64 {
        self.hue
    }

    pub fn chroma(&self) -> f64 {
        self.chroma
    }

    pub fn tone(&self) -> f64 {
        self.tone
    }

    pub fn to_argb(&self) -> Argb {
        self.argb
    }

    pub fn set_hue(&mut self, hue: f64) {
        *self = Self::new(hue, self.chroma, self.tone);
    }

    pub fn set_chroma(&mut self, chroma: f64) {
        *self = Self::new(self.hue, chroma, self.tone);
    }

    pub fn set_tone(&mut self, tone: f64) {
        *self = Self::new(self.hue, self.chroma, tone);
    }
}

// ---- tonal palettes ---------------------------------------------------------------------------

struct KeyColor {
    hue: f64,
    requested_chroma: f64,
    cache: HashMap<u64, f64>,
}

impl KeyColor {
    const MAX_CHROMA_VALUE: f64 = 200.0;

    fn max_chroma(&mut self, tone: i32) -> f64 {
        let key = f64::from(tone).to_bits();
        if let Some(c) = self.cache.get(&key) {
            return *c;
        }
        let c = Hct::new(self.hue, Self::MAX_CHROMA_VALUE, f64::from(tone)).chroma();
        self.cache.insert(key, c);
        c
    }

    fn create(&mut self) -> Hct {
        const PIVOT_TONE: i32 = 50;
        const STEP: i32 = 1;
        const EPSILON: f64 = 0.01;
        let (mut lower, mut upper) = (0, 100);
        while lower < upper {
            let mid = (lower + upper) / 2;
            let is_ascending = self.max_chroma(mid) < self.max_chroma(mid + STEP);
            let sufficient = self.max_chroma(mid) >= self.requested_chroma - EPSILON;
            if sufficient {
                if (lower - PIVOT_TONE).abs() < (upper - PIVOT_TONE).abs() {
                    upper = mid;
                } else {
                    if lower == mid {
                        return Hct::new(self.hue, self.requested_chroma, f64::from(lower));
                    }
                    lower = mid;
                }
            } else if is_ascending {
                lower = mid + STEP;
            } else {
                upper = mid;
            }
        }
        Hct::new(self.hue, self.requested_chroma, f64::from(lower))
    }
}

#[derive(Clone, Copy, Debug)]
pub struct TonalPalette {
    pub hue: f64,
    pub chroma: f64,
    pub key_color: Hct,
}

impl TonalPalette {
    pub fn from_argb(argb: Argb) -> Self {
        let cam = cam_from_int(argb);
        Self::from_hue_chroma(cam.hue, cam.chroma)
    }

    pub fn from_hue_chroma(hue: f64, chroma: f64) -> Self {
        let key_color = KeyColor { hue, requested_chroma: chroma, cache: HashMap::new() }.create();
        Self { hue, chroma, key_color }
    }

    pub fn from_hct(hct: Hct) -> Self {
        Self { hue: hct.hue(), chroma: hct.chroma(), key_color: hct }
    }

    pub fn get(&self, tone: f64) -> Argb {
        solve_to_int(self.hue, self.chroma, tone)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn round_trips_through_hct() {
        for argb in [0xff00_0000, 0xffff_ffff, 0xff12_3456, 0xff80_8080, 0xffff_8800] {
            let hct = Hct::from_argb(argb);
            let back = Hct::new(hct.hue(), hct.chroma(), hct.tone());
            assert_eq!(back.to_argb(), argb, "{argb:08x}");
        }
    }
}
