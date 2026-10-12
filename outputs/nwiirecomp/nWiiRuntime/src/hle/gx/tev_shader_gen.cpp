#include "runtime/gx/tev_shader_gen.h"
#include <algorithm>
#include <sstream>
#include <cstdlib>
#include <cstdio>
#include <string>

namespace nwii::runtime::gx {

// ---- Indirect texturing (LIGHT7, roadmap M3.2) -------------------------------
// Dolphin PixelShaderGen semantics. BP 0x10+n IND_CMD (TevStageIndirect): bt 0-1,
// fmt 2-3, bias 4-6, bs 7-8, matrix index 9-10, matrix id 11-12, sw 13-15,
// tw 16-18, lb_utclod 19, fb_addprev 20. BP 0x27 IREF (map 3 / coord 3 bits per
// indirect stage), BP 0x25/0x26 indirect coordinate scale (4+4 bits per stage),
// BP 0x06-0x0E matrices (uniform uIndMtx, see renderer_gl.cpp), BP 0x00 bits
// 16-18 numindstages. SS draws its EGG light textures (all character and map
// lighting) with indirect stages: normal-sphere map -> per-light matrix -> 1D
// gradient lookup (decomp eggLightTexture.cpp setMaterialNormalSphere/addLight).
static uint32_t IndCmd(const GXState& s, int n) { return s.bp[0x10 + n] & 0x1FFFFF; }
static uint32_t NumIndStages(const GXState& s) { return (s.bp[0x00] >> 16) & 7; }
static uint32_t TevCount(const GXState& s) {
    uint32_t n = s.numTevStages;
    return n == 0 ? 1 : (n > 16 ? 16 : n);
}
// Indirect stage referenced by TEV stage n with an active matrix or bump alpha.
static bool IndStageUsed(const GXState& s, int n) {
    const uint32_t c = IndCmd(s, n);
    return (c & 3) < NumIndStages(s) && (((c >> 7) & 3) != 0 || ((c >> 9) & 3) != 0);
}
// TEV swap table t (LIGHT7; Dolphin BPMemory TevKSel swap1/swap2): BP 0xF6+2t
// bits 0-1 / 2-3 select the source of R / G, BP 0xF7+2t those of B / A.
// Returns the GLSL swizzle ("rgba" = identity).
static std::string TevSwapSwizzle(const GXState& s, uint32_t t) {
    const uint32_t a = s.bp[0xF6 + 2 * (t & 3)], b = s.bp[0xF7 + 2 * (t & 3)];
    static const char kC[4] = {'r', 'g', 'b', 'a'};
    return {kC[a & 3], kC[(a >> 2) & 3], kC[b & 3], kC[(b >> 2) & 3]};
}
bool TEVShaderUsesIndirect(const GXState& s) {
    for (uint32_t n = 0; n < TevCount(s); ++n) {
        const uint32_t c = IndCmd(s, n);
        // Wrapping / add-previous act even without an indirect stage.
        if (IndStageUsed(s, n) || ((c >> 13) & 0xFF) != 0) return true;
    }
    return false;
}
uint8_t TEVIndirectMapsUsed(const GXState& s) {
    uint8_t m = 0;
    for (uint32_t n = 0; n < TevCount(s); ++n)
        if (IndStageUsed(s, n))
            m |= (uint8_t)(1u << ((s.bp[0x27] >> (6 * (IndCmd(s, n) & 3))) & 7));
    return m;
}

// ---- Texcoord scale (TEX7, BP 0x30-0x3F) --------------------------------------
// The SU multiplies normalized texcoord k by its scale (BP 0x30+2k / 0x31+2k,
// bits 0-15 = size-1) and the texture unit divides by the texmap size (Dolphin
// texdims). GX keeps both equal unless the game sets the scale itself
// (GXSetTexCoordScaleManually: SS dBgS::SetupMapGX/DrawMap/DrawSkyKeepMap, EGG
// PostEffectMaskDOF) or shares one coord between maps of different sizes. Only
// such stages get the scaled lookup, so ordinary shaders stay unchanged.
// Indirect shaders already use the fixed-point path with the scale (LIGHT7).
uint16_t TEVTexCoordScaleMismatch(const GXState& s) {
    if (TEVShaderUsesIndirect(s)) return 0;
    uint16_t m = 0;
    for (uint32_t n = 0; n < TevCount(s); ++n) {
        const TEVStage& st = s.tevStages[n];
        if (st.texMap > 7) continue;
        // texcoord >= numtexgens reads coord 0 (vertex data and, as in Dolphin, its scale)
        const uint32_t tc = (st.texCoord & 7) < (s.numTexGens & 0xF) ? (st.texCoord & 7) : 0u;
        const TexStage& tx = s.texStages[st.texMap];
        if ((s.bp[0x30 + 2 * tc] & 0xFFFF) + 1 != std::max<uint32_t>(tx.width, 1) ||
            (s.bp[0x31 + 2 * tc] & 0xFFFF) + 1 != std::max<uint32_t>(tx.height, 1))
            m |= (uint16_t)(1u << n);
    }
    return m;
}

// ---- LIGHT17: integer TEV + EFB storage precision ---------------------------
// (TEVIntegerMath / TEVColorNoPerspective / TEVOutputQuantKey are inline in
// tev_shader_gen.h, so the renderer links with older generator copies too.)

// LIGHT17: one TEV stage in Dolphin's integer maths (PixelShaderGen WriteStage /
// WriteTevRegular). Expects texColor / rasColor (swizzled, float) of the stage;
// writes itev[dest]. a/b/c are the low 8 bits of their source, d is s11;
// regular: ((d + bias) << s) OP ((((a << 8) + (b - a) * (c + (c >> 7))) << s)
// + rnd) >> 8, then >> 1 for divide-by-2 (no rounding bias there), rnd = 128
// for add / 127 for subtract; clamp to 0..255 or s11 -1024..1023.
static void EmitIntCombiner(std::ostream& fs, const TEVStage& st, int kcsel, int kasel) {
    static const char* const kK8[8] = {"255", "223", "191", "159", "128", "96", "64", "32"};
    static const char* const kComp[4] = {"r", "g", "b", "a"};
    auto kc = [&]() -> std::string {
        if (kcsel <= 7) return std::string("ivec3(") + kK8[kcsel] + ")";
        if (kcsel >= 0x0C && kcsel <= 0x0F) return "ikon[" + std::to_string(kcsel - 0x0C) + "].rgb";
        if (kcsel >= 0x10)
            return "ivec3(ikon[" + std::to_string((kcsel - 0x10) % 4) + "]." + kComp[(kcsel - 0x10) / 4] + ")";
        return "ivec3(0)";
    };
    auto ka = [&]() -> std::string {
        if (kasel <= 7) return kK8[kasel];
        if (kasel >= 0x10)
            return "ikon[" + std::to_string((kasel - 0x10) % 4) + "]." + kComp[(kasel - 0x10) / 4];
        return "0";
    };
    auto cin = [&](int arg) -> std::string {
        switch (arg) {
            case 0: return "itev[0].rgb";  case 1: return "ivec3(itev[0].a)";
            case 2: return "itev[1].rgb";  case 3: return "ivec3(itev[1].a)";
            case 4: return "itev[2].rgb";  case 5: return "ivec3(itev[2].a)";
            case 6: return "itev[3].rgb";  case 7: return "ivec3(itev[3].a)";
            case 8: return "itex.rgb";     case 9: return "ivec3(itex.a)";
            case 10: return "iras.rgb";    case 11: return "ivec3(iras.a)";
            case 12: return "ivec3(255)";  case 13: return "ivec3(128)";
            case 14: return kc();
            default: return "ivec3(0)";
        }
    };
    auto ain = [&](int arg) -> std::string {
        switch (arg) {
            case 0: return "itev[0].a"; case 1: return "itev[1].a";
            case 2: return "itev[2].a"; case 3: return "itev[3].a";
            case 4: return "itex.a";    case 5: return "iras.a";
            case 6: return ka();
            default: return "0";
        }
    };
    fs << "    itex = ivec4(round(clamp(texColor, 0.0, 1.0) * 255.0));\n";
    // Rasterised colour: Dolphin SW Rasterizer truncates the interpolated value
    // ((u16) cast); the small epsilon keeps exact vertex values from flooring
    // one step down through float interpolation error.
    fs << "    iras = ivec4(clamp(rasColor, 0.0, 1.0) * 255.0 + 0.0078125);\n";
    fs << "    ivec4 ta = ivec4(" << cin(st.colorInA) << ", " << ain(st.alphaInA) << ") & 255;\n";
    fs << "    ivec4 tb = ivec4(" << cin(st.colorInB) << ", " << ain(st.alphaInB) << ") & 255;\n";
    fs << "    ivec4 tc = ivec4(" << cin(st.colorInC) << ", " << ain(st.alphaInC) << ") & 255;\n";
    fs << "    ivec4 td = ivec4(" << cin(st.colorInD) << ", " << ain(st.alphaInD) << ");\n";
    auto regular = [&](const char* comp, uint32_t bias, uint32_t op, uint32_t scale) {
        const char* sl = scale == 1 ? " << 1" : scale == 2 ? " << 2" : "";
        const char* sb = bias == 1 ? " + 128" : bias == 2 ? " - 128" : "";
        const char* rnd = scale == 3 ? "" : (op ? " + 127" : " + 128");
        std::ostringstream e;
        e << "((((td." << comp << sb << ")" << sl << ") " << (op ? "-" : "+") << " (((((ta." << comp
          << " << 8) + (tb." << comp << " - ta." << comp << ") * (tc." << comp << " + (tc." << comp
          << " >> 7)))" << sl << ")" << rnd << ") >> 8))" << (scale == 3 ? " >> 1" : "") << ")";
        return e.str();
    };
    // colour
    if (st.colorBias == 3) {
        const char* rel = st.colorOp ? "==" : ">";
        if (st.colorScale == 3)
            fs << "    ivec3 cOut = td.rgb + ivec3(" << (st.colorOp ? "equal" : "greaterThan")
               << "(ta.rgb, tb.rgb)) * tc.rgb;\n";
        else {
            auto pack = [&](const char* v) { // R8 / GR16 / BGR24 (Dolphin comp16 / comp24)
                const std::string x(v);
                if (st.colorScale == 0) return x + ".r";
                if (st.colorScale == 1) return "(" + x + ".r + " + x + ".g * 256)";
                return "(" + x + ".r + " + x + ".g * 256 + " + x + ".b * 65536)";
            };
            fs << "    ivec3 cOut = td.rgb + ((" << pack("ta") << " " << rel << " " << pack("tb")
               << ") ? tc.rgb : ivec3(0));\n";
        }
    } else {
        fs << "    ivec3 cOut = " << regular("rgb", st.colorBias, st.colorOp == 1, st.colorScale) << ";\n";
    }
    fs << "    cOut = " << (st.colorClamp ? "clamp(cOut, 0, 255)" : "clamp(cOut, -1024, 1023)") << ";\n";
    // alpha (inputs read before this stage's colour write, as in Dolphin)
    if (st.alphaBias == 3) {
        const char* rel = st.alphaOp ? "==" : ">";
        if (st.alphaScale == 3)
            fs << "    int aOut = td.a + ((ta.a " << rel << " tb.a) ? tc.a : 0);\n";
        else {
            std::string a = "ta.r", b = "tb.r";
            if (st.alphaScale == 1) { a = "(ta.r + ta.g * 256)"; b = "(tb.r + tb.g * 256)"; }
            if (st.alphaScale == 2) { a = "(ta.r + ta.g * 256 + ta.b * 65536)"; b = "(tb.r + tb.g * 256 + tb.b * 65536)"; }
            fs << "    int aOut = td.a + ((" << a << " " << rel << " " << b << ") ? tc.a : 0);\n";
        }
    } else {
        fs << "    int aOut = " << regular("a", st.alphaBias, st.alphaOp == 1, st.alphaScale) << ";\n";
    }
    fs << "    aOut = " << (st.alphaClamp ? "clamp(aOut, 0, 255)" : "clamp(aOut, -1024, 1023)") << ";\n";
    fs << "    itev[" << (int)st.colorRegId << "].rgb = cOut;\n";
    fs << "    itev[" << (int)st.alphaRegId << "].a = aOut;\n";
}

// ---- Fog (TEX7, roadmap M3.3) ------------------------------------------------
// Dolphin PixelShaderGen WriteFog. BP 0xF1 (FogParam3) bits 21-23 fsel (0 off,
// 2 linear (1/3 behave as linear), 4 exp, 5 exp2, 6 backwards exp, 7 backwards
// exp2), bit 20 proj (1 = orthographic); BP 0xE8 bit 10 range adjust. Applied
// to the final TEV colour (RGB only) after the alpha test, as in Dolphin.
// Inputs read here must match the fog part of ShaderUid (renderer_gl.cpp).
static void EmitFog(std::ostream& fs, const GXState& s) {
    const uint32_t p3 = s.bp[0xF1];
    const uint32_t fsel = (p3 >> 21) & 7;
    if (fsel == 0) return;
    const bool ortho = (p3 >> 20) & 1;
    const bool range = (s.bp[0xE8] >> 10) & 1;
    fs << "    {\n";
    fs << "    int zCoord = clamp(int(gl_FragCoord.z * 16777216.0), 0, 0xFFFFFF);\n";
    if (!ortho)
        fs << "    float ze = (uFogF.x * 16777216.0) / float(uFogI.y - (zCoord >> uFogI.w));\n";
    else
        fs << "    float ze = uFogF.x * float(zCoord) / 16777216.0;\n";
    if (range) {
        fs << "    float offset = (2.0 * (gl_FragCoord.x / uFogF.w)) - 1.0 - uFogF.z;\n";
        fs << "    float floatindex = clamp(9.0 - abs(offset) * 9.0, 0.0, 9.0);\n";
        fs << "    uint il = uint(floatindex), iu = min(il + 1u, 9u);\n";
        fs << "    float k = mix(uFogRange[il >> 2u][il & 3u], uFogRange[iu >> 2u][iu & 3u], fract(floatindex));\n";
        fs << "    ze *= sqrt(offset * offset + k * k) / k;\n";
    }
    fs << "    float fog = clamp(ze - uFogF.y, 0.0, 1.0);\n";
    switch (fsel) {
        case 4: fs << "    fog = 1.0 - exp2(-8.0 * fog);\n"; break;
        case 5: fs << "    fog = 1.0 - exp2(-8.0 * fog * fog);\n"; break;
        case 6: fs << "    fog = exp2(-8.0 * (1.0 - fog));\n"; break;
        case 7: fs << "    fog = 1.0 - fog;\n    fog = exp2(-8.0 * fog * fog);\n"; break;
        default: break;
    }
    fs << "    int ifog = int(round(fog * 256.0));\n";
    fs << "    ivec3 fc = ivec3((uFogI.x >> 16) & 255, (uFogI.x >> 8) & 255, uFogI.x & 255);\n";
    fs << "    ivec3 pc = ivec3(floor(clamp(FragColor.rgb, 0.0, 1.0) * 255.0 + 0.5));\n";
    fs << "    FragColor.rgb = vec3((pc * (256 - ifog) + fc * ifog) >> 8) / 255.0;\n";
    fs << "    }\n";
}

GeneratedShader GenerateTEVShader(const GXState& state, uint8_t prim_type) {
    GeneratedShader shader;

    

    

    
    std::stringstream vs;
    vs << "#version 330 core\n";
    const bool itev = TEVIntegerMath(); // LIGHT17
    if (itev) vs << "#define GX_INT_LIGHT 1\n";

    
    
    vs << "layout(location = 0) in vec3 vertexPosition;\n";
    vs << "layout(location = 1) in vec4 vertexColor;\n";
    // XF texgen output (s, t, q) per texcoord, generated per vertex on the CPU
    // (renderer_gl.cpp TexGen, M1.8): location 2 = coord 0, 4..10 = coords 1..7.
    vs << "layout(location = 2) in vec3 aTex0;\n";
    vs << "layout(location = 3) in vec3 vertexNormal;\n";
    for (int i = 1; i < 8; ++i)
        vs << "layout(location = " << (3 + i) << ") in vec3 aTex" << i << ";\n";

    // LIGHT17 (opt-in NWII_COLOR_SCREENLINEAR=1): colours linear in screen space (Dolphin SW Rasterizer:
    // ColorSlopes without w; texcoords stay perspective-correct). Default: perspective-correct.
    const char* cq = TEVColorNoPerspective() ? "noperspective " : "";
    vs << cq << "out vec4 vColor0;\n";
    vs << cq << "out vec4 vColor1;\n";
    for (int i = 0; i < 8; ++i)
        vs << "out vec3 vTex" << i << ";\n";

    vs << "uniform mat4 mvp;\n";
    // XF colour-channel state (0x100a-0x1011) and the 8 lights (0x600).
    vs << "uniform vec4 uMatColor[2];\n";
    vs << "uniform vec4 uAmbColor[2];\n";
    vs << "uniform int uChanCtrl[4];\n"; // 0,1 = colour chans; 2,3 = alpha chans
    vs << "uniform vec4 uLightCol[8];\n";
    vs << "uniform vec4 uLightPos[8];\n";
    vs << "uniform vec4 uLightDir[8];\n";
    vs << "uniform vec4 uLightCosAtt[8];\n";
    vs << "uniform vec4 uLightDistAtt[8];\n";

    // GX colour channel: the rasterised colour is NOT the vertex colour —
    // it is material (register or vertex) modulated by ambient + lights,
    // per XF channel control. Games animate the material register (fades,
    // the WARNING "press" blink); ignoring it makes those effects vanish.
    // LIGHT7: Dolphin LightingShaderGen per channel -- colour and alpha are
    // separate channels, each with its own material/ambient source, enable,
    // light mask, diffuse function and attenuation function (XF 0x100E/0x100F
    // colour, 0x1010/0x1011 alpha). Light colour is RGBA (alpha lights use .a).
    vs << R"(
vec4 gx_lights(int ctrl, vec3 pos, vec3 _normal) {
    int  diffusefunc = (ctrl >> 7) & 3;
    int  attnfunc    = (ctrl >> 9) & 3;
    int  lightmask   = ((ctrl >> 2) & 0xF) | (((ctrl >> 11) & 0xF) << 4);
    vec4 acc = vec4(0.0);
    for (int i = 0; i < 8; ++i) {
        if ((lightmask & (1 << i)) == 0) continue;
        vec3 ldir; float attn;
        if (attnfunc == 3) { // spot
            ldir = uLightPos[i].xyz - pos;
            float dist2 = dot(ldir, ldir);
            float dist = sqrt(dist2);
            ldir = ldir / dist;
            attn = max(0.0, dot(ldir, uLightDir[i].xyz));
            attn = max(0.0, uLightCosAtt[i].x + uLightCosAtt[i].y * attn +
                            uLightCosAtt[i].z * attn * attn) /
                   dot(uLightDistAtt[i].xyz, vec3(1.0, dist, dist2));
        } else if (attnfunc == 1) { // specular
            ldir = normalize(uLightPos[i].xyz - pos);
            attn = (dot(_normal, ldir) >= 0.0)
                       ? max(0.0, dot(_normal, uLightDir[i].xyz)) : 0.0;
            vec3 cosAttn = uLightCosAtt[i].xyz;
            vec3 distAttn = (diffusefunc == 0) ? uLightDistAtt[i].xyz
                                               : normalize(uLightDistAtt[i].xyz);
            attn = max(0.0, dot(cosAttn, vec3(1.0, attn, attn * attn))) /
                   dot(distAttn, vec3(1.0, attn, attn * attn));
        } else { // none / directional
            ldir = uLightPos[i].xyz - pos;
            ldir = (length(ldir) == 0.0) ? _normal : normalize(ldir);
            attn = 1.0;
        }
        float ndl = (diffusefunc == 0) ? 1.0
                  : ((diffusefunc == 1) ? dot(ldir, _normal)
                                        : max(0.0, dot(ldir, _normal)));
        // Dolphin: lacc += round(attn * ndl * col) in 0..255 units.
        acc += round(attn * ndl * uLightCol[i] * 255.0) / 255.0;
    }
    return acc;
}
vec4 dolphin_chan(int chan, vec3 pos, vec3 nrm, vec4 vcol) {
    int ctrl = uChanCtrl[chan];
    int actrl = uChanCtrl[chan + 2];
    vec3 _normal = dot(nrm, nrm) > 0.0 ? normalize(nrm) : vec3(0.0);
    vec4 r;
    // colour: mat * clamp(amb + lights) when lighting is enabled, else mat.
    vec3 mat = ((ctrl & 1) != 0) ? vcol.rgb : uMatColor[chan].rgb;
    if ((ctrl & 2) != 0) {
        vec3 lacc = ((ctrl & 0x40) != 0) ? vcol.rgb : uAmbColor[chan].rgb;
        lacc += gx_lights(ctrl, pos, _normal).rgb;
#ifdef GX_INT_LIGHT
        // LIGHT17: Dolphin LightingShaderGen: (mat * (lacc + (lacc >> 7))) >> 8 on 0..255 ints.
        ivec3 im = ivec3(round(clamp(mat, 0.0, 1.0) * 255.0));
        ivec3 il = clamp(ivec3(round(lacc * 255.0)), 0, 255);
        r.rgb = vec3((im * (il + (il >> 7))) >> 8) / 255.0;
#else
        r.rgb = mat * clamp(lacc, 0.0, 1.0);
#endif
    } else {
        r.rgb = mat;
    }
    float amat = ((actrl & 1) != 0) ? vcol.a : uMatColor[chan].a;
    if ((actrl & 2) != 0) {
        float aacc = ((actrl & 0x40) != 0) ? vcol.a : uAmbColor[chan].a;
        aacc += gx_lights(actrl, pos, _normal).a;
#ifdef GX_INT_LIGHT
        int iam = int(round(clamp(amat, 0.0, 1.0) * 255.0));
        int ial = clamp(int(round(aacc * 255.0)), 0, 255);
        r.a = float((iam * (ial + (ial >> 7))) >> 8) / 255.0;
#else
        r.a = amat * clamp(aacc, 0.0, 1.0);
#endif
    } else {
        r.a = amat;
    }
    return clamp(r, 0.0, 1.0);
}
)";

    // Dolphin VertexShaderGen: channels >= numColorChans (XF 0x1009) are not
    // lit -- colour 0 is the vertex colour (white when absent), colour 1 the
    // vertex colour 1 if present, else colour 0. (The parser decodes only
    // colour 0, so colour 1 always takes the colour 0 output.)
    const uint32_t numColorChans = state.xf[0x09] & 3;
    vs << "void main() {\n";
    vs << "    gl_Position = mvp * vec4(vertexPosition, 1.0);\n";
    if (numColorChans == 0)
        vs << "    vColor0 = vertexColor;\n";
    else
        vs << "    vColor0 = dolphin_chan(0, vertexPosition, vertexNormal, vertexColor);\n";
    if (numColorChans >= 2)
        vs << "    vColor1 = dolphin_chan(1, vertexPosition, vertexNormal, vertexColor);\n";
    else
        vs << "    vColor1 = vColor0;\n";
    // Colour texgens (XF 0x1040+i type 2/3) take the LIT channel colour
    // (Dolphin: o.tex = float3(o.colors_n.x, o.colors_n.y, 1)); the CPU texgen
    // only sees the vertex colour. Coordinates >= numtexgens repeat coord 0.
    const uint32_t ntg = std::min<uint32_t>(state.xf[0x3F] & 0xF, 8);
    for (uint32_t i = 0; i < 8; ++i) {
        const uint32_t src = i < ntg ? i : 0;
        const uint32_t type = ntg ? (state.xf[0x40 + src] >> 4) & 7 : 0;
        if (type == 2 || type == 3)
            vs << "    vTex" << i << " = vec3(vColor" << (type - 2) << ".xy, 1.0);\n";
        else
            vs << "    vTex" << i << " = aTex" << i << ";\n";
    }
    vs << "}\n";
    shader.vertex_source = vs.str();

    std::stringstream fs;
    fs << "#version 330 core\n";
    fs << cq << "in vec4 vColor0;\n";
    fs << cq << "in vec4 vColor1;\n";
    for (int i = 0; i < 8; ++i)
        fs << "in vec3 vTex" << i << ";\n";
    // Projective divide per fragment (Dolphin PixelShaderGen: xy / z unless
    // z == 0); ST texgens arrive with z = 1.
    fs << "vec2 gxTexCoord(vec3 t) { return t.z == 0.0 ? t.xy : t.xy / t.z; }\n";
    // TEV compare mode helpers: the hardware compares 8-bit input values.
    fs << "vec3 tevQ(vec3 v) { return floor(clamp(v, 0.0, 1.0) * 255.0 + 0.5); }\n";
    fs << "float tevQ1(float v) { return floor(clamp(v, 0.0, 1.0) * 255.0 + 0.5); }\n";
    fs << "float tevPackR(vec3 v) { return tevQ(v).r; }\n";
    fs << "float tevPackGR(vec3 v) { vec3 q = tevQ(v); return q.r + q.g * 256.0; }\n";
    fs << "float tevPackBGR(vec3 v) { vec3 q = tevQ(v); return q.r + q.g * 256.0 + q.b * 65536.0; }\n";
    // Dual-source output (RENDER1, roadmap M1.5): index 0 is what the EFB
    // stores (alpha replaced by the PE dst-alpha constant, BP 0x42, when it is
    // enabled); index 1 is the TEV result the blender reads as source colour/
    // alpha (GL_SRC1_*), as in Dolphin's dual-source path.
    fs << "layout(location = 0, index = 0) out vec4 FragColor;\n";
    fs << "layout(location = 0, index = 1) out vec4 FragColor1;\n";
    fs << "uniform vec4 uDstAlpha;\n"; // x: enable, y: alpha
    // Fog (TEX7, M3.3): Dolphin I_FOGF (A, C, range centre, 2*vp.wd in EFB
    // px), I_FOGI (x: colour 0xRRGGBB, y: B magnitude, w: B shift), I_FOGRANGE.
    fs << "uniform vec4 uFogF;\nuniform ivec4 uFogI;\nuniform vec4 uFogRange[3];\n";
    
    for (int i = 0; i < 8; ++i) {
        fs << "uniform sampler2D uTex" << i << ";\n";
    }
    fs << "uniform vec4 uTevColor[4];\n";
    fs << "uniform vec4 uTevKColor[4];\n";
    // Indirect (LIGHT7): BP 0x06-0x0E matrices as Dolphin I_INDTEXMTX rows
    // (ma,mc,me,17-scale)/(mb,md,mf,..); uTexDim[i].xy = texmap i size in
    // texels, .zw = texcoord i scale (BP 0x30+2i / 0x31+2i, +1).
    fs << "uniform ivec4 uIndMtx[6];\n";
    fs << "uniform vec4 uTexDim[8];\n";

    fs << "vec4 tevReg[4];\n"; 
    fs << "vec4 texColor;\n";
    fs << "vec4 rasColor;\n";
    if (itev) // LIGHT17: integer TEV state (Dolphin PixelShaderGen: s11 registers, konst)
        fs << "ivec4 itev[4];\nivec4 ikon[4];\nivec4 itex;\nivec4 iras;\n";
    
    fs << "void main() {\n";
    fs << "    tevReg[0] = uTevColor[0];\n";
    fs << "    tevReg[1] = uTevColor[1];\n";
    fs << "    tevReg[2] = uTevColor[2];\n";
    fs << "    tevReg[3] = uTevColor[3];\n";
    if (itev)
        fs << "    for (int r = 0; r < 4; ++r) { itev[r] = ivec4(round(uTevColor[r] * 255.0)); "
              "ikon[r] = ivec4(round(uTevKColor[r] * 255.0)); }\n";

    uint8_t numTevs = state.numTevStages;
    if (numTevs == 0) numTevs = 1;

    // Indirect path (LIGHT7): Dolphin's fixed-point texel coordinates
    // (1/128 texel; texcoord scale BP 0x30+2k / texmap size) for every stage of
    // a shader that has any indirect operation. Shaders without one keep the
    // normalized-coordinate lookup below unchanged.
    const bool useInd = TEVShaderUsesIndirect(state);
    const uint16_t tcScaleMis = TEVTexCoordScaleMismatch(state); // TEX7, 0 if useInd
    const uint32_t numTexGens = state.numTexGens & 0xF;
    auto tcOf = [&](uint32_t tc) { return tc < numTexGens ? tc : 0u; };
    if (useInd) {
        fs << "    ivec2 fixpoint_uv[8];\n";
        for (uint32_t k = 0; k < 8; ++k) {
            if (numTexGens == 0 || k >= numTexGens)
                fs << "    fixpoint_uv[" << k << "] = ivec2(0);\n";
            else
                fs << "    fixpoint_uv[" << k << "] = ivec2(gxTexCoord(vTex" << k << ") * uTexDim[" << k
                   << "].zw * 128.0);\n";
        }
        fs << "    ivec2 tevcoord = ivec2(0);\n    ivec2 wrappedcoord;\n    int alphabump = 0;\n";
        uint32_t usedInd = 0;
        for (uint32_t n = 0; n < numTevs; ++n)
            if (IndStageUsed(state, (int)n)) usedInd |= 1u << (IndCmd(state, (int)n) & 3);
        for (uint32_t j = 0; j < 4; ++j) {
            fs << "    ivec3 iindtex" << j << " = ivec3(0);\n";
            if (!((usedInd >> j) & 1)) continue;
            const uint32_t iref = state.bp[0x27] >> (6 * j);
            const uint32_t map = iref & 7, tc = tcOf((iref >> 3) & 7);
            const uint32_t sc = state.bp[0x25 + j / 2] >> (8 * (j & 1));
            fs << "    iindtex" << j << " = ivec3(round(texture(uTex" << map << ", vec2(fixpoint_uv[" << tc
               << "] >> ivec2(" << (sc & 0xF) << ", " << ((sc >> 4) & 0xF) << ")) / (128.0 * uTexDim[" << map
               << "].xy)).abg * 255.0));\n";
        }
    }
    
    for (int i = 0; i < numTevs; ++i) {
        const auto& stage = state.tevStages[i];
        fs << "    // TEV Stage " << i << "\n";
        // Stage-local temporaries must not be redeclared in the main scope.
        // The TEV registers remain outside so subsequent stages can read them.
        fs << "    {\n";

        // Rasterized colour (Dolphin tev_ras_table): 5 = alpha bump, 6 = alpha
        // bump normalized, 7 (GX_COLOR_ZERO / NULL) = all zero incl. alpha.
        if (stage.colorChan == 0) fs << "    rasColor = vColor0;\n";
        else if (stage.colorChan == 1) fs << "    rasColor = vColor1;\n";
        else if (useInd && stage.colorChan == 5) fs << "    rasColor = vec4(float(alphabump) / 255.0);\n";
        else if (useInd && stage.colorChan == 6)
            fs << "    rasColor = vec4(float(alphabump | (alphabump >> 5)) / 255.0);\n";
        else fs << "    rasColor = vec4(0.0);\n";
        // TEV swap tables (LIGHT7, Dolphin WriteStage): the alpha env (BP 0xC1+2n)
        // selects the ras swap table (bits 0-1) and the tex swap table (bits 2-3).
        const uint32_t swapSel = state.bp[0xC1 + 2 * i];
        const std::string rasSwz = TevSwapSwizzle(state, swapSel & 3);
        const std::string texSwz = TevSwapSwizzle(state, (swapSel >> 2) & 3);
        if (stage.colorChan <= 1 && rasSwz != "rgba") fs << "    rasColor = rasColor." << rasSwz << ";\n";

        if (useInd) {
            // Dolphin WriteStage indirect op + wrapping (runs for every stage).
            const uint32_t c = IndCmd(state, i);
            const uint32_t bt = c & 3, fmt = (c >> 2) & 3, bias = (c >> 4) & 7, bs = (c >> 7) & 3;
            const uint32_t mtxIndex = (c >> 9) & 3, mtxId = (c >> 11) & 3;
            const uint32_t sw = (c >> 13) & 7, tw = (c >> 16) & 7, addprev = (c >> 20) & 1;
            const bool hasInd = bt < NumIndStages(state);
            const uint32_t tc = tcOf(stage.texCoord);
            static const char* const kFmtMask[4] = {"255", "31", "15", "7"};
            static const char* const kBumpMask[4] = {"248", "224", "240", "248"};
            static const char* const kComp[4] = {"", "x", "y", "z"};
            fs << "    ivec2 indtevtrans = ivec2(0);\n";
            if (hasInd && bs != 0)
                fs << "    alphabump = iindtex" << bt << "." << kComp[bs] << " & " << kBumpMask[fmt] << ";\n";
            if (hasInd && mtxIndex != 0 && mtxId <= 2) {
                const int m = 2 * ((int)mtxIndex - 1);
                fs << "    ivec3 crd = iindtex" << bt << " & ivec3(" << kFmtMask[fmt] << ");\n";
                const char* badd = fmt == 0 ? "-128" : "1";
                if (bias & 1) fs << "    crd.x += " << badd << ";\n";
                if (bias & 2) fs << "    crd.y += " << badd << ";\n";
                if (bias & 4) fs << "    crd.z += " << badd << ";\n";
                if (mtxId == 0)
                    fs << "    indtevtrans = ivec2(uIndMtx[" << m << "].x * crd.x + uIndMtx[" << m
                       << "].y * crd.y + uIndMtx[" << m << "].z * crd.z, uIndMtx[" << m + 1
                       << "].x * crd.x + uIndMtx[" << m + 1 << "].y * crd.y + uIndMtx[" << m + 1
                       << "].z * crd.z) >> 3;\n";
                else
                    fs << "    indtevtrans = (fixpoint_uv[" << tc << "] * crd." << (mtxId == 1 ? "xx" : "yy")
                       << ") >> 8;\n";
                fs << "    if (uIndMtx[" << m << "].w >= 0) indtevtrans >>= uIndMtx[" << m
                   << "].w; else indtevtrans <<= -uIndMtx[" << m << "].w;\n";
            }
            auto wrap = [&](uint32_t w, const char* comp) {
                fs << "    wrappedcoord." << comp << " = ";
                if (w == 0) fs << "fixpoint_uv[" << tc << "]." << comp << ";\n";
                else if (w >= 6) fs << "0;\n";
                else fs << "fixpoint_uv[" << tc << "]." << comp << " & " << ((256 >> (w - 1)) * 128 - 1) << ";\n";
            };
            wrap(sw, "x");
            wrap(tw, "y");
            fs << "    tevcoord = " << (addprev ? "tevcoord + " : "") << "wrappedcoord + indtevtrans;\n";
            fs << "    tevcoord = (tevcoord << 8) >> 8;\n";
        }

        if (stage.texMap != 0xFF && useInd) {
            fs << "    texColor = texture(uTex" << (int)stage.texMap << ", vec2(tevcoord) / (128.0 * uTexDim["
               << (int)stage.texMap << "].xy));\n";
        } else if (stage.texMap != 0xFF && ((tcScaleMis >> i) & 1)) {
            // TEX7: texcoord scale != texmap size (BP 0x30+2k): uv * scale / size.
            fs << "    texColor = texture(uTex" << (int)stage.texMap << ", gxTexCoord(vTex" << (int)stage.texCoord
               << ") * (uTexDim[" << (int)tcOf(stage.texCoord & 7) << "].zw / uTexDim[" << (int)(stage.texMap & 7)
               << "].xy));\n";
        } else if (stage.texMap != 0xFF) {
            fs << "    texColor = texture(uTex" << (int)stage.texMap << ", gxTexCoord(vTex" << (int)stage.texCoord << "));\n";
        } else {
            fs << "    texColor = vec4(1.0);\n";
        }
        if (stage.texMap != 0xFF && texSwz != "rgba") fs << "    texColor = texColor." << texSwz << ";\n";

        uint32_t ksel_reg = state.bp[0xF6 + (i / 2)];
        int kcsel = 0, kasel = 0;
        if ((i % 2) == 0) {
            kcsel = (ksel_reg >> 4) & 0x1F;
            kasel = (ksel_reg >> 9) & 0x1F;
        } else {
            kcsel = (ksel_reg >> 14) & 0x1F;
            kasel = (ksel_reg >> 19) & 0x1F;
        }
        
        auto get_konst_color = [&](int kc) -> std::string {
            if (kc <= 0x07) {
                
                char buf[32];
                std::snprintf(buf, sizeof(buf), "vec3(%.6f)", (8 - kc) / 8.0f);
                return std::string(buf);
            } else if (kc >= 0x0C && kc <= 0x0F) {
                return "uTevKColor[" + std::to_string(kc - 0x0C) + "].rgb";
            } else if (kc >= 0x10 && kc <= 0x1F) {
                int idx = (kc - 0x10) % 4;
                int comp = (kc - 0x10) / 4;
                const char* comps[] = {"r", "g", "b", "a"};
                return "vec3(uTevKColor[" + std::to_string(idx) + "]." + comps[comp] + ")";
            }
            return "vec3(0.0)";
        };

        auto get_konst_alpha = [&](int ka) -> std::string {
            if (ka <= 0x07) {
                
                char buf[32];
                std::snprintf(buf, sizeof(buf), "%.6f", (8 - ka) / 8.0f);
                return std::string(buf);
            } else if (ka >= 0x10 && ka <= 0x1F) {
                int idx = (ka - 0x10) % 4;
                int comp = (ka - 0x10) / 4;
                const char* comps[] = {"r", "g", "b", "a"};
                return "uTevKColor[" + std::to_string(idx) + "]." + comps[comp];
            }
            return "0.0";
        };
        std::string kc_str = get_konst_color(kcsel);
        std::string ka_str = get_konst_alpha(kasel);
        if (itev) { // LIGHT17
            EmitIntCombiner(fs, stage, kcsel, kasel);
            fs << "    }\n";
            continue;
        }
        
        fs << "    vec3 cIn[4];\n";
        auto map_color_input = [&](int in_idx, int arg) {
            switch(arg) {
                case 0: fs << "    cIn[" << in_idx << "] = tevReg[0].rgb;\n"; break;
                case 1: fs << "    cIn[" << in_idx << "] = vec3(tevReg[0].a);\n"; break;
                case 2: fs << "    cIn[" << in_idx << "] = tevReg[1].rgb;\n"; break;
                case 3: fs << "    cIn[" << in_idx << "] = vec3(tevReg[1].a);\n"; break;
                case 4: fs << "    cIn[" << in_idx << "] = tevReg[2].rgb;\n"; break;
                case 5: fs << "    cIn[" << in_idx << "] = vec3(tevReg[2].a);\n"; break;
                case 6: fs << "    cIn[" << in_idx << "] = tevReg[3].rgb;\n"; break;
                case 7: fs << "    cIn[" << in_idx << "] = vec3(tevReg[3].a);\n"; break;
                case 8: fs << "    cIn[" << in_idx << "] = texColor.rgb;\n"; break;
                case 9: fs << "    cIn[" << in_idx << "] = vec3(texColor.a);\n"; break;
                case 10: fs << "    cIn[" << in_idx << "] = rasColor.rgb;\n"; break;
                case 11: fs << "    cIn[" << in_idx << "] = vec3(rasColor.a);\n"; break;
                case 12: fs << "    cIn[" << in_idx << "] = vec3(1.0);\n"; break;
                case 13: fs << "    cIn[" << in_idx << "] = vec3(0.5);\n"; break;
                case 14: fs << "    cIn[" << in_idx << "] = " << kc_str << ";\n"; break; 
                case 15: fs << "    cIn[" << in_idx << "] = vec3(0.0);\n"; break;
                default: fs << "    cIn[" << in_idx << "] = vec3(0.0);\n"; break;
            }
        };
        map_color_input(0, stage.colorInA);
        map_color_input(1, stage.colorInB);
        map_color_input(2, stage.colorInC);
        map_color_input(3, stage.colorInD);
        
        fs << "    vec3 cOut;\n";
        if (stage.colorBias == 3) {
            // Compare mode (bias == 3; Dolphin PixelShaderGen tev_rgb_comparison_*):
            // op selects GT/EQ, scale the compared quantity -- R8, GR16, BGR24
            // (8-bit channels packed little-endian) or RGB8 per channel;
            // out = d + (a OP b ? c : 0). Compared on 8-bit values.
            const char* rel = stage.colorOp ? "==" : ">";
            if (stage.colorScale == 3) {
                fs << "    cOut = cIn[3] + (" << (stage.colorOp ? "vec3(equal(" : "vec3(greaterThan(")
                   << "tevQ(cIn[0]), tevQ(cIn[1]))) * cIn[2]);\n";
            } else {
                const char* pack = stage.colorScale == 0 ? "tevPackR" : stage.colorScale == 1 ? "tevPackGR" : "tevPackBGR";
                fs << "    cOut = cIn[3] + ((" << pack << "(cIn[0]) " << rel << " " << pack
                   << "(cIn[1])) ? cIn[2] : vec3(0.0));\n";
            }
        } else {
            fs << "    vec3 cBias;\n";
            if (stage.colorBias == 1) fs << "    cBias = vec3(0.5);\n";
            else if (stage.colorBias == 2) fs << "    cBias = vec3(-0.5);\n";
            else fs << "    cBias = vec3(0.0);\n";

            if (stage.colorOp == 0) {
                fs << "    cOut = cIn[3] + mix(cIn[0], cIn[1], cIn[2]) + cBias;\n";
            } else if (stage.colorOp == 1) {
                fs << "    cOut = cIn[3] - mix(cIn[0], cIn[1], cIn[2]) + cBias;\n";
            } else {
                fs << "    cOut = cIn[3] + mix(cIn[0], cIn[1], cIn[2]) + cBias;\n";
            }

            if (stage.colorScale == 1) fs << "    cOut *= 2.0;\n";
            else if (stage.colorScale == 2) fs << "    cOut *= 4.0;\n";
            else if (stage.colorScale == 3) fs << "    cOut *= 0.5;\n";
        }

        if (stage.colorClamp == 1) fs << "    cOut = clamp(cOut, 0.0, 1.0);\n";

        fs << "    tevReg[" << (int)stage.colorRegId << "].rgb = cOut;\n";
        
        fs << "    float aIn[4];\n";
        auto map_alpha_input = [&](int in_idx, int arg) {
            switch(arg) {
                case 0: fs << "    aIn[" << in_idx << "] = tevReg[0].a;\n"; break;
                case 1: fs << "    aIn[" << in_idx << "] = tevReg[1].a;\n"; break;
                case 2: fs << "    aIn[" << in_idx << "] = tevReg[2].a;\n"; break;
                case 3: fs << "    aIn[" << in_idx << "] = tevReg[3].a;\n"; break;
                case 4: fs << "    aIn[" << in_idx << "] = texColor.a;\n"; break;
                case 5: fs << "    aIn[" << in_idx << "] = rasColor.a;\n"; break;
                case 6: fs << "    aIn[" << in_idx << "] = " << ka_str << ";\n"; break; 
                case 7: fs << "    aIn[" << in_idx << "] = 0.0;\n"; break;
                default: fs << "    aIn[" << in_idx << "] = 0.0;\n"; break;
            }
        };
        map_alpha_input(0, stage.alphaInA);
        map_alpha_input(1, stage.alphaInB);
        map_alpha_input(2, stage.alphaInC);
        map_alpha_input(3, stage.alphaInD);
        
        fs << "    float aOut;\n";
        if (stage.alphaBias == 3) {
            // Compare mode (Dolphin tev_a_comparison_*): modes 0-2 compare the
            // COLOUR a/b inputs (R8 / GR16 / BGR24), mode 3 (A8) the alpha
            // inputs; out = d + (a OP b ? c : 0).
            const char* rel = stage.alphaOp ? "==" : ">";
            if (stage.alphaScale == 3) {
                fs << "    aOut = aIn[3] + ((tevQ1(aIn[0]) " << rel << " tevQ1(aIn[1])) ? aIn[2] : 0.0);\n";
            } else {
                const char* pack = stage.alphaScale == 0 ? "tevPackR" : stage.alphaScale == 1 ? "tevPackGR" : "tevPackBGR";
                fs << "    aOut = aIn[3] + ((" << pack << "(cIn[0]) " << rel << " " << pack
                   << "(cIn[1])) ? aIn[2] : 0.0);\n";
            }
        } else {
            fs << "    float aBias;\n";
            if (stage.alphaBias == 1) fs << "    aBias = 0.5;\n";
            else if (stage.alphaBias == 2) fs << "    aBias = -0.5;\n";
            else fs << "    aBias = 0.0;\n";

            if (stage.alphaOp == 0) {
                fs << "    aOut = aIn[3] + mix(aIn[0], aIn[1], aIn[2]) + aBias;\n";
            } else if (stage.alphaOp == 1) {
                fs << "    aOut = aIn[3] - mix(aIn[0], aIn[1], aIn[2]) + aBias;\n";
            } else {
                fs << "    aOut = aIn[3] + mix(aIn[0], aIn[1], aIn[2]) + aBias;\n";
            }

            if (stage.alphaScale == 1) fs << "    aOut *= 2.0;\n";
            else if (stage.alphaScale == 2) fs << "    aOut *= 4.0;\n";
            else if (stage.alphaScale == 3) fs << "    aOut *= 0.5;\n";
        }

        if (stage.alphaClamp == 1) fs << "    aOut = clamp(aOut, 0.0, 1.0);\n";

        fs << "    tevReg[" << (int)stage.alphaRegId << "].a = aOut;\n";
        fs << "    }\n";
    }
    
    // Dolphin PixelShaderGen: "the results of the last texenv stage are put
    // onto the screen, regardless of the used destination register" (CAP7 D1:
    // SS character/Loftwing materials end in C2).
    {
        const auto& last = state.tevStages[numTevs - 1];
        if (itev) { // LIGHT17: Dolphin "prev = prev & 255" (SW: u8 of the last stage's registers)
            if (last.colorRegId != 0) fs << "    itev[0].rgb = itev[" << (int)last.colorRegId << "].rgb;\n";
            if (last.alphaRegId != 0) fs << "    itev[0].a = itev[" << (int)last.alphaRegId << "].a;\n";
            fs << "    ivec4 prev = itev[0] & 255;\n";
        } else {
        if (last.colorRegId != 0) fs << "    tevReg[0].rgb = tevReg[" << (int)last.colorRegId << "].rgb;\n";
        if (last.alphaRegId != 0) fs << "    tevReg[0].a = tevReg[" << (int)last.alphaRegId << "].a;\n";
        }
    }
    if (itev)
        fs << "    FragColor = vec4(prev) / 255.0;\n";
    else
        fs << "    FragColor = tevReg[0];\n";

    
    static const bool flat_color = std::getenv("NWII_FLATCOLOR") != nullptr;
    if (flat_color)
        fs << "    FragColor = vec4(1.0, 0.0, 0.0, 1.0);\n";

    

    {
        const auto& at = state.alphaTest;
        auto cmp = [&](uint8_t comp, uint8_t ref) -> std::string {
            char rb[32];
            std::snprintf(rb, sizeof(rb), "%.6f", ref / 255.0f);
            std::string r(rb);
            if (itev) { // LIGHT17: integer compare on the 8-bit TEV alpha (Dolphin tev_alpha_funcs_table)
                const std::string ir = std::to_string((int)ref);
                switch (comp) {
                    case 0: return std::string("false");
                    case 1: return "prev.a < " + ir;
                    case 2: return "prev.a == " + ir;
                    case 3: return "prev.a <= " + ir;
                    case 4: return "prev.a > " + ir;
                    case 5: return "prev.a != " + ir;
                    case 6: return "prev.a >= " + ir;
                    default: return std::string("true");
                }
            }
            switch (comp) {
                case 0: return "false";
                case 1: return "FragColor.a < " + r;
                case 2: return "FragColor.a == " + r;
                case 3: return "FragColor.a <= " + r;
                case 4: return "FragColor.a > " + r;
                case 5: return "FragColor.a != " + r;
                case 6: return "FragColor.a >= " + r;
                default: return "true";
            }
        };
        static const bool no_alpha_test = std::getenv("NWII_NOALPHATEST") != nullptr;
        bool disabled = (at.comp0 == 7 && at.comp1 == 7 && at.logic == 0) ||
                        no_alpha_test;
        if (!disabled) {
            std::string a = cmp(at.comp0, at.ref0);
            std::string b = cmp(at.comp1, at.ref1);
            std::string test;
            switch (at.logic) {
                case 0: test = "(" + a + ") && (" + b + ")"; break;
                case 1: test = "(" + a + ") || (" + b + ")"; break;
                case 2: test = "((" + a + ") != (" + b + "))"; break;
                default: test = "((" + a + ") == (" + b + "))"; break;
            }
            fs << "    if (!(" << test << ")) discard;\n";
        }
    }
    EmitFog(fs, state);
    fs << "    FragColor1 = FragColor;\n";
    fs << "    if (uDstAlpha.x > 0.5) FragColor.a = uDstAlpha.y;\n";
    // LIGHT17: EFB storage precision for plain stores (Dolphin SW EfbInterface:
    // Dither() then 6-bit store, reads Convert6To8; RGB565 5/6/5). The stored
    // value is written already expanded, so later reads/blends see exactly the
    // hardware's value. Blend / logic-op draws keep the 8-bit source (the
    // hardware quantises the blend RESULT); the renderer re-quantises those.
    // Dither row parity: physical EFB row (top origin) of an even-height EFB.
    {
        const uint32_t qk = TEVOutputQuantKey(state);
        if (qk && !(qk & 8)) {
            fs << "    {\n    ivec4 qv = ivec4(round(clamp(FragColor, 0.0, 1.0) * 255.0));\n";
            if ((qk & 3) == 1) {
                if (qk & 4)
                    fs << "    ivec2 qp = ivec2(gl_FragCoord.xy);\n"
                          "    qv.rgb = (qv.rgb - (qv.rgb >> 6)) + abs(((qp.y + 1) & 1) * 3 - (qp.x & 1) * 2);\n";
                fs << "    qv >>= 2;\n    FragColor = vec4((qv << 2) | (qv >> 4)) / 255.0;\n";
            } else {
                fs << "    ivec3 q = qv.rgb >> ivec3(3, 2, 3);\n"
                      "    FragColor.rgb = vec3(ivec3((q.r << 3) | (q.r >> 2), (q.g << 2) | (q.g >> 4),"
                      " (q.b << 3) | (q.b >> 2))) / 255.0;\n";
            }
            fs << "    }\n";
        }
    }
    
    fs << "}\n";
    shader.fragment_source = fs.str();
    
    return shader;
}

} 
