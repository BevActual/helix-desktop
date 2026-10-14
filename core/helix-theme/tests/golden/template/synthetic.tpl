mode={{ mode }} image={{ image }} closest={{ closest_color }} dir={{config_dir}} file={{ config_file }}
icon={{ icon_theme }}
--- formats
hex={{ colors.primary.default.hex }} dark={{ colors.primary.dark.hex }} light={{ colors.primary.light.hex }}
stripped={{ colors.surface.default.hex_stripped }}
rgb={{ colors.primary.default.rgb }} csv={{ colors.primary.default.rgb_csv }} rgba={{ colors.primary.default.rgba }}
hsl={{ colors.error.default.hsl }} hsla={{ colors.error.default.hsla }}
r={{ colors.tertiary.default.red }} g={{ colors.tertiary.default.green }} b={{ colors.tertiary.default.blue }} a={{ colors.tertiary.default.alpha }}
h={{ colors.secondary.default.hue }} s={{ colors.secondary.default.saturation }} l={{ colors.secondary.default.lightness }}
unknownformat={{ colors.primary.default.nothing }}
alias={{ colors.hover.default.hex }} {{ colors.on_hover.default.hex }}
--- filters
set_alpha={{ colors.primary.default.rgba | set_alpha: 0.5 }}
alpha_small={{ colors.primary.default.rgba | set_alpha: 0.00001 }}
lighten={{ colors.primary.default.hex | lighten: 10 }}
darken={{ colors.primary.default.hex | darken: 10 }}
saturate={{ colors.primary.default.hex | saturate: 15 }}
desaturate={{ colors.primary.default.hex | desaturate: 15 }}
set_lightness={{ colors.primary.default.hex | set_lightness: 40 }}
set_hue={{ colors.primary.default.hex | set_hue: 200 }}
rotate_hue={{ colors.primary.default.hex | rotate_hue: -30 }}
set_saturation={{ colors.primary.default.hex | set_saturation: 55 }}
set_red={{ colors.primary.default.hex | set_red: 300 }}
set_green={{ colors.primary.default.hex | set_green: 12.5 }}
set_blue={{ colors.primary.default.hex | set_blue: -4 }}
auto_lightness={{ colors.primary.default.hex | auto_lightness: 20 }}
grayscale={{ colors.primary.default.hex | grayscale }}
invert={{ colors.primary.default.hex | invert }}
chain={{ colors.primary.default.hex | darken: 5 | saturate: 10 | set_alpha: 0.25 }}
blend={{ colors.primary.default.hex | blend: "#ff0000", 0.5 }}
blend_default={{ colors.primary.default.hex | blend: #00ff00 }}
harmonize={{ colors.primary.default.hex | harmonize: '#0000ff' }}
badnum={{ colors.primary.default.hex | lighten: abc }}
plusnum={{ colors.primary.default.hex | lighten: +5 }}
noarg={{ colors.primary.default.hex | lighten }}
to_color={{ colors.primary.default.hex | to_color | lighten: 5 }}
replace={{ colors.primary.default.hex | replace: "#", "0x" }}
replace_sq={{ colors.primary.default.hex | replace: '#', 'rgb:' }}
upper_then_lower={{ colors.primary.default.hex | lower_case }}
camel={{ colors.primary.default.rgb | camel_case }}
pascal={{ colors.primary.default.rgb | pascal_case }}
snake={{ colors.primary.default.rgb | snake_case }}
kebab={{ colors.primary.default.rgb | kebab_case }}
--- errors
badpath={{ colors.primary.default }}
unknowncolor={{ colors.nope.default.hex }}
unknownmode={{ colors.primary.sepia.hex }}
unknownbase={{ whatever.thing }}
unknownfilter={{ colors.primary.default.hex | frobnicate }}
--- control
<* for i in 0..4 *>
i={{ i }} idx={{ loop.index }} first={{ loop.first }} last={{ loop.last }}
<* endfor *>
<* if mode *>
has-mode
<* else *>
no-mode
<* endif *>
<* if not closest_color *>
no-closest
<* else *>
closest-set
<* endif *>
<* if {{ colors.primary.default.hex }} *>
primary-truthy
<* endif *>
<* if config.missing *>
missing-truthy
<* else *>
missing-falsy
<* endif *>
<* for name, value in colors *>
{{ name }}={{ value.default.hex }} {{ value.dark }}
<* endfor *>
<* for k, v in config *>
key={{ k }}
<* for item in v.list *>
  item={{ item }} i={{ loop.index }}
<* endfor *>
<* endfor *>
cfg={{ config.shell.name }} size={{ config.shell.size }} ratio={{ config.shell.ratio }} on={{ config.shell.enabled }} nested={{ config.shell.nested.k }}
first={{ config.shell.list.0 }} third={{ config.shell.list.2 }} out={{ config.shell.list.9 }} neg={{ config.shell.list.-1 }}
text {{ config.shell.name | pascal_case }} <* if config.shell.enabled *>inline-block<* endif *> tail
  <* if config.shell.enabled *>   
indented-block
  <* endif *>
unterminated {{ colors.primary.default.hex
last line without newline
