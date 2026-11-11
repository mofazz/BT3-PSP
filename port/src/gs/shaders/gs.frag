#version 450
// GS primitive, fragment stage: texture function (TEX0.TFX / TCC) and alpha test (TEST), as the GS does them.
// Colours are 0..1 for 0..255. Alpha is carried with 1.0 = 0x80 (the GS's "opaque").
layout(location = 0) in vec4 vColor;
layout(location = 1) in vec3 vStq;
layout(location = 0) out vec4 outColor;
layout(location = 1) out vec4 outAux; // .r: the alpha byte exactly as the GS stores it (object numbers live there)
layout(set = 2, binding = 0) uniform sampler2D tex;
layout(set = 2, binding = 1) uniform sampler2D dateTex; // a copy of the target's alpha bytes, for the destination alpha test
layout(set = 3, binding = 0) uniform Params {
    ivec4 mode;  // x: textured (1 from GS memory, 2 a frame buffer), y: TFX, z: TCC, w: alpha test (0 off, else ATST + 1)
    vec4 misc;   // x: AREF in GS units (0..255); y: destination alpha test (0 off, 1 pass where bit 7 of the
                 // stored alpha is 0, 2 where it is 1); z: 1 = FBA, the alpha written gets bit 7 set;
                 // w: for 2D sprites, output pixels per GS pixel (0 = sample per output pixel)
    vec4 rect;   // textured from a frame buffer (mode.x == 2): the uv range that may be sampled
} p;
void main() {
    float k = 255.0 / 128.0;
    if (p.misc.y != 0.0) {
        // TEST.DATE: the GS looks at bit 7 of the alpha already in the frame buffer. A GPU cannot read what it is
        // drawing to, so the back end hands over a copy made just before this run of draws.
        bool set = texelFetch(dateTex, ivec2(gl_FragCoord.xy), 0).r >= 127.5 / 255.0;
        if (set != (p.misc.y > 1.5)) discard;
    }
    vec3 rgb = vColor.rgb;
    float a = vColor.a * k;
    if (p.mode.x != 0) {
        vec2 uv = vStq.xy / vStq.z;
        if (p.misc.w != 0.0) {
            // A 2D sprite: take the texture coordinate where the GS takes it, at the whole GS pixel this fragment
            // belongs to (misc.w = output pixels per GS pixel). Every output pixel of that GS pixel then shows
            // the same texel, as on the console. Sampling at the output pixels' own centres reaches a quarter or
            // three quarters of a texel further, and at a sprite's edge that is the neighbouring picture of the
            // sheet (seen as slivers of another bar's colour and as thin lines along HUD panels).
            vec2 f = gl_FragCoord.xy, g = floor(f / p.misc.w) * p.misc.w;
            uv -= dFdx(uv) * (f.x - g.x) + dFdy(uv) * (f.y - g.y);
        }
        if (p.mode.x == 2) uv = clamp(uv, p.rect.xy, p.rect.zw);
        vec4 t = texture(tex, uv);
        if (p.mode.x == 1) t.a *= k; // a texture from GS memory keeps the GS alpha (0x80 opaque, up to 0xFF)
        if (p.mode.y == 0) {
            rgb = t.rgb * vColor.rgb * k;
            if (p.mode.z != 0) a = t.a * a;
        } else if (p.mode.y == 1) {
            rgb = t.rgb;
            if (p.mode.z != 0) a = t.a;
        } else {
            rgb = t.rgb * vColor.rgb * k + vec3(vColor.a);
            if (p.mode.z != 0) a = (p.mode.y == 2) ? t.a + a : t.a;
        }
    }
    if (p.mode.w != 0) {
        // the GS compares integers; round, or a value that is exactly AREF (tree leaves: 0x7F against 0x7F) fails by float error
        float ag = floor(a * 128.0 + 0.5);
        int f = p.mode.w - 1;
        bool pass = f == 0 ? false : f == 1 ? true : f == 2 ? ag < p.misc.x : f == 3 ? ag <= p.misc.x :
                    f == 4 ? abs(ag - p.misc.x) < 0.5 : f == 5 ? ag >= p.misc.x : f == 6 ? ag > p.misc.x : abs(ag - p.misc.x) >= 0.5;
        if (!pass) discard;
    }
    outColor = vec4(clamp(rgb, 0.0, 1.0), clamp(a, 0.0, 1.0));
    float ab = clamp(a * 128.0 / 255.0, 0.0, 1.0);
    if (p.misc.z != 0.0) ab = (float(int(ab * 255.0 + 0.5) | 128)) / 255.0; // FBA
    outAux = vec4(ab, 0.0, 0.0, 1.0);
}
