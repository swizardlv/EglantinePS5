// SPDX-License-Identifier: GPL-3.0-or-later
#version 460 core
layout(location = 0) in vec2 frag_tex_coord;
layout(location = 0) out vec4 color;
layout(binding = 0) uniform sampler2D color_texture;

vec4 KeyedTexel(ivec2 pixel, ivec2 extent) {
    vec3 rgb = texelFetch(color_texture, clamp(pixel, ivec2(0), extent - 1), 0).rgb;
    if (all(lessThanEqual(abs(rgb - vec3(1.0, 0.0, 0.0)), vec3(0.5 / 255.0)))) {
        return vec4(0.0);
    }
    return vec4(rgb, 1.0);
}

void main() {
    ivec2 extent = textureSize(color_texture, 0);
    vec2 position = frag_tex_coord * vec2(extent) - 0.5;
    ivec2 pixel = ivec2(floor(position));
    vec2 weight = fract(position);
    // Key before filtering, so a red hole does not bleed into neighbouring UI.
    vec4 top = mix(KeyedTexel(pixel, extent), KeyedTexel(pixel + ivec2(1, 0), extent), weight.x);
    vec4 bottom = mix(KeyedTexel(pixel + ivec2(0, 1), extent),
                      KeyedTexel(pixel + ivec2(1, 1), extent), weight.x);
    color = mix(top, bottom, weight.y);
}
