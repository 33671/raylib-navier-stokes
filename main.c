/*
 *  Navier-Stokes Fluid Simulation – Raylib Port
 *  Based on Pavel Dobryakov's WebGL Fluid Simulation
 *  (https://github.com/PavelDoGreat/WebGL-Fluid-Simulation)
 *
 *  Simulation runs on CPU at 128² (velocity / pressure) and 256² (dye).
 *  Display uses raylib shaders for shading, bloom, and sunrays.
 */

#include "raylib.h"
#include "raymath.h"
#include "rlgl.h"

#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdio.h>

/* ------------------------------------------------------------------ */
/*  Configuration                                                      */
/* ------------------------------------------------------------------ */
#define SIM_W          128
#define SIM_H          128
#define DYE_W          256
#define DYE_H          256

#define PRESSURE_ITERS 20

static float g_densityDissipation  = 1.0f;
static float g_velocityDissipation = 0.2f;
static float g_pressure            = 0.8f;
static float g_curlAmount          = 30.0f;
static float g_splatRadius         = 0.25f;    /* divided by 100 in splat, net ~0.0025 */
static float g_splatForce          = 6000.0f;

static bool  g_shading             = true;
static bool  g_colorful            = true;
static bool  g_paused              = false;
static bool  g_bloomEnabled         = true;
static bool  g_sunraysEnabled       = true;

static float g_bloomThreshold      = 0.6f;
static float g_bloomSoftKnee       = 0.7f;
static float g_sunraysWeight       = 1.0f;

/* ------------------------------------------------------------------ */
/*  Simulation grids  (CPU)                                            */
/* ------------------------------------------------------------------ */
typedef struct {
    int   w, h;
    float *vx,  *vy;         /* velocity   */
    float *vx0, *vy0;        /* velocity buffer */
    float *dr,  *dg,  *db;   /* dye        */
    float *dr0, *dg0, *db0;  /* dye buffer */
    float *p;                /* pressure   */
    float *div;              /* divergence */
    float *curl;             /* vorticity  */
} Fluid;

static Fluid g_fluid;

/* ------------------------------------------------------------------ */
/*  Helper: HSV → RGB                                                  */
/* ------------------------------------------------------------------ */
static Color hsv_to_rgb(float h, float s, float v) {
    Color c;
    int   i = (int)(h * 6.0f);
    float f = h * 6.0f - i;
    float p = v * (1.0f - s);
    float q = v * (1.0f - f * s);
    float t = v * (1.0f - (1.0f - f) * s);
    float r, g, b;
    switch (i % 6) {
        case 0: r = v; g = t; b = p; break;
        case 1: r = q; g = v; b = p; break;
        case 2: r = p; g = v; b = t; break;
        case 3: r = p; g = q; b = v; break;
        case 4: r = t; g = p; b = v; break;
        default:r = v; g = p; b = q; break;
    }
    c.r = (unsigned char)(r * 255);
    c.g = (unsigned char)(g * 255);
    c.b = (unsigned char)(b * 255);
    c.a = 255;
    return c;
}

/* ------------------------------------------------------------------ */
/*  Grid helpers                                                       */
/* ------------------------------------------------------------------ */
static inline int idx2(int x, int y, int w)   { return y*w + x; }
/* bilinear sample from a float grid; returns 0 outside */
static float sample_bilinear(const float *field, int w, int h, float x, float y) {
    x = x - 0.5f;   /* cell-centre → corner index */
    y = y - 0.5f;
    int   x0 = (int)floorf(x), y0 = (int)floorf(y);
    int   x1 = x0 + 1,          y1 = y0 + 1;
    float fx = x - x0,          fy = y - y0;

    float v00 = (x0>=0 && x0<w && y0>=0 && y0<h) ? field[y0*w + x0] : 0.0f;
    float v10 = (x1>=0 && x1<w && y0>=0 && y0<h) ? field[y0*w + x1] : 0.0f;
    float v01 = (x0>=0 && x0<w && y1>=0 && y1<h) ? field[y1*w + x0] : 0.0f;
    float v11 = (x1>=0 && x1<w && y1>=0 && y1<h) ? field[y1*w + x1] : 0.0f;

    float v0 = v00 + (v10 - v00) * fx;
    float v1 = v01 + (v11 - v01) * fx;
    return v0 + (v1 - v0) * fy;
}

/* clamp */
static inline float clampf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

/* ------------------------------------------------------------------ */
/*  Fluid lifecycle                                                    */
/* ------------------------------------------------------------------ */
static void fluid_init(Fluid *f, int sw, int sh, int dw, int dh) {
    memset(f, 0, sizeof(*f));
    f->w = sw; f->h = sh;
    int sn = sw * sh;
    f->vx   = (float*)calloc(sn, sizeof(float));
    f->vy   = (float*)calloc(sn, sizeof(float));
    f->vx0  = (float*)calloc(sn, sizeof(float));
    f->vy0  = (float*)calloc(sn, sizeof(float));
    f->p    = (float*)calloc(sn, sizeof(float));
    f->div  = (float*)calloc(sn, sizeof(float));
    f->curl = (float*)calloc(sn, sizeof(float));

    int dn = dw * dh;
    f->dr   = (float*)calloc(dn, sizeof(float));
    f->dg   = (float*)calloc(dn, sizeof(float));
    f->db   = (float*)calloc(dn, sizeof(float));
    f->dr0  = (float*)calloc(dn, sizeof(float));
    f->dg0  = (float*)calloc(dn, sizeof(float));
    f->db0  = (float*)calloc(dn, sizeof(float));
    f->w = sw; f->h = sh;
}

static void fluid_free(Fluid *f) {
    free(f->vx); free(f->vy); free(f->vx0); free(f->vy0);
    free(f->p); free(f->div); free(f->curl);
    free(f->dr); free(f->dg); free(f->db);
    free(f->dr0); free(f->dg0); free(f->db0);
    memset(f, 0, sizeof(*f));
}

/* ------------------------------------------------------------------ */
/*  Splat – inject velocity + colour at a point                        */
/* ------------------------------------------------------------------ */
static void fluid_splat(Fluid *f, float x, float y,
                        float dx, float dy,
                        float cr, float cg, float cb) {
    int sw = SIM_W, sh = SIM_H;
    int dw = DYE_W, dh = DYE_H;

    /* Gauss kernel: exp(-d² / radius)  — radius is in UV units           */
    float radSq = (g_splatRadius * 0.04f);    /* scale down (@ /25)      */
    radSq = radSq * radSq;
    if (radSq < 1e-8f) radSq = 1e-8f;

    /* ---- velocity splat (sim grid) ---- */
    int r0 = (int)((g_splatRadius * 0.04f * sw) + 2);
    int cx = (int)(x * sw + 0.5f);
    int cy = (int)(y * sh + 0.5f);

    for (int j = cy - r0; j <= cy + r0; j++) {
        if (j < 0 || j >= sh) continue;
        for (int i = cx - r0; i <= cx + r0; i++) {
            if (i < 0 || i >= sw) continue;
            float px = (float)i / sw - x;
            float py = (float)j / sh - y;
            float w  = expf(-(px*px + py*py) / radSq);
            f->vx[idx2(i,j,sw)] += dx * w;
            f->vy[idx2(i,j,sw)] += dy * w;
        }
    }

    /* ---- dye splat (dye grid) ---- */
    r0 = (int)((g_splatRadius * 0.04f * dw) + 2);
    cx = (int)(x * dw + 0.5f);
    cy = (int)(y * dh + 0.5f);
    for (int j = cy - r0; j <= cy + r0; j++) {
        if (j < 0 || j >= dh) continue;
        for (int i = cx - r0; i <= cx + r0; i++) {
            if (i < 0 || i >= dw) continue;
            float px = (float)i / dw - x;
            float py = (float)j / dh - y;
            float w  = expf(-(px*px + py*py) / radSq);
            int   idx = idx2(i,j,dw);
            f->dr[idx] += cr * w;
            f->dg[idx] += cg * w;
            f->db[idx] += cb * w;
        }
    }
}

/* ------------------------------------------------------------------ */
/*  Simulation step  (one full frame)                                  */
/* ------------------------------------------------------------------ */
static void fluid_step(Fluid *f, float dt) {
    int   sw = SIM_W, sh = SIM_H, sn = sw * sh;
    int   dw = DYE_W, dh = DYE_H, dn = dw * dh;
    float *vx = f->vx, *vy = f->vy;
    float *vx0 = f->vx0, *vy0 = f->vy0;
    float *p   = f->p, *div = f->div, *curl = f->curl;

    /* -- 1. Curl ---------------------------------------------------- */
    for (int j = 0; j < sh; j++) {
        for (int i = 0; i < sw; i++) {
            float L = (i > 0)      ? vy[idx2(i-1,j,sw)] : -vy[idx2(0,j,sw)];
            float R = (i < sw-1)   ? vy[idx2(i+1,j,sw)] : -vy[idx2(sw-1,j,sw)];
            float T = (j < sh-1)   ? vx[idx2(i,j+1,sw)] : -vx[idx2(i,sh-1,sw)];
            float B = (j > 0)      ? vx[idx2(i,j-1,sw)] : -vx[idx2(i,0,sw)];
            curl[idx2(i,j,sw)] = 0.5f * (R - L - T + B);
        }
    }

    /* -- 2. Vorticity confinement ----------------------------------- */
    for (int j = 0; j < sh; j++) {
        for (int i = 0; i < sw; i++) {
            float L = (i > 0)      ? curl[idx2(i-1,j,sw)] : 0;
            float R = (i < sw-1)   ? curl[idx2(i+1,j,sw)] : 0;
            float T = (j < sh-1)   ? curl[idx2(i,j+1,sw)] : 0;
            float B = (j > 0)      ? curl[idx2(i,j-1,sw)] : 0;
            float C = curl[idx2(i,j,sw)];

            float fx = fabsf(T) - fabsf(B);
            float fy = fabsf(R) - fabsf(L);
            float len = sqrtf(fx*fx + fy*fy) + 0.0001f;
            fx /= len; fy /= len;
            fx *= g_curlAmount * C;
            fy *= g_curlAmount * C;
            fy *= -1.0f;

            int id = idx2(i,j,sw);
            vx[id] += fx * dt;
            vy[id] += fy * dt;
            vx[id] = clampf(vx[id], -1000.0f, 1000.0f);
            vy[id] = clampf(vy[id], -1000.0f, 1000.0f);
        }
    }

    /* -- 3. Divergence ---------------------------------------------- */
    for (int j = 0; j < sh; j++) {
        for (int i = 0; i < sw; i++) {
            float L = (i > 0)      ? vx[idx2(i-1,j,sw)] : -vx[idx2(0,j,sw)];
            float R = (i < sw-1)   ? vx[idx2(i+1,j,sw)] : -vx[idx2(sw-1,j,sw)];
            float T = (j < sh-1)   ? vy[idx2(i,j+1,sw)] : -vy[idx2(i,sh-1,sw)];
            float B = (j > 0)      ? vy[idx2(i,j-1,sw)] : -vy[idx2(i,0,sw)];
            div[idx2(i,j,sw)] = 0.5f * (R - L + T - B);
        }
    }

    /* -- 4. Pressure solve (Jacobi) --------------------------------- */
    /* pre-scale existing pressure                                    */
    for (int i = 0; i < sn; i++) p[i] *= g_pressure;

    for (int iter = 0; iter < PRESSURE_ITERS; iter++) {
        for (int j = 0; j < sh; j++) {
            for (int i = 0; i < sw; i++) {
                float L = (i > 0)    ? p[idx2(i-1,j,sw)] : p[idx2(0,j,sw)];
                float R = (i < sw-1) ? p[idx2(i+1,j,sw)] : p[idx2(sw-1,j,sw)];
                float T = (j < sh-1) ? p[idx2(i,j+1,sw)] : p[idx2(i,sh-1,sw)];
                float B = (j > 0)    ? p[idx2(i,j-1,sw)] : p[idx2(i,0,sw)];
                float d = div[idx2(i,j,sw)];
                /* write to vx0 as scratch buffer (reused) */
                vx0[idx2(i,j,sw)] = (L + R + B + T - d) * 0.25f;
            }
        }
        /* swap p <-> vx0 scratch */
        { float *tmp = p; p = vx0; vx0 = tmp; }
    }
    /* restore pointers: p is correct, vx0 back */
    { float *tmp = p; p = vx0; vx0 = tmp; }

    /* -- 5. Gradient subtraction ------------------------------------ */
    for (int j = 0; j < sh; j++) {
        for (int i = 0; i < sw; i++) {
            float L = (i > 0)    ? p[idx2(i-1,j,sw)] : p[idx2(0,j,sw)];
            float R = (i < sw-1) ? p[idx2(i+1,j,sw)] : p[idx2(sw-1,j,sw)];
            float T = (j < sh-1) ? p[idx2(i,j+1,sw)] : p[idx2(i,sh-1,sw)];
            float B = (j > 0)    ? p[idx2(i,j-1,sw)] : p[idx2(i,0,sw)];
            int id = idx2(i,j,sw);
            vx[id] -= (R - L);
            vy[id] -= (T - B);
        }
    }

    /* -- 6. Advect velocity ----------------------------------------- */
    for (int j = 0; j < sh; j++) {
        for (int i = 0; i < sw; i++) {
            int   id  = idx2(i,j,sw);
            float bx  = i + 0.5f - dt * vx[id];
            float by  = j + 0.5f - dt * vy[id];
            vx0[id] = sample_bilinear(vx, sw, sh, bx, by);
            vy0[id] = sample_bilinear(vy, sw, sh, bx, by);
        }
    }
    /* dissipation */
    float vdecay = 1.0f / (1.0f + g_velocityDissipation * dt);
    for (int i = 0; i < sn; i++) {
        f->vx[i] = vx0[i] * vdecay;
        f->vy[i] = vy0[i] * vdecay;
    }

    /* -- 7. Advect dye ---------------------------------------------- */
    for (int j = 0; j < dh; j++) {
        for (int i = 0; i < dw; i++) {
            /* sample velocity at dye's UV coord                  */
            float uvx = ((float)i + 0.5f) / dw;
            float uvy = ((float)j + 0.5f) / dh;
            float sx  = uvx * sw;   /* in sim-cell coords */
            float sy  = uvy * sh;
            float velx = sample_bilinear(f->vx, sw, sh, sx, sy);
            float vely = sample_bilinear(f->vy, sw, sh, sx, sy);
            /* backtrack in dye-grid coords                      */
            float bx  = i + 0.5f - dt * velx * ((float)dw / sw);
            float by  = j + 0.5f - dt * vely * ((float)dh / sh);
            int   id  = idx2(i,j,dw);
            f->dr0[id] = sample_bilinear(f->dr, dw, dh, bx, by);
            f->dg0[id] = sample_bilinear(f->dg, dw, dh, bx, by);
            f->db0[id] = sample_bilinear(f->db, dw, dh, bx, by);
        }
    }
    float ddecay = 1.0f / (1.0f + g_densityDissipation * dt);
    for (int i = 0; i < dn; i++) {
        f->dr[i] = f->dr0[i] * ddecay;
        f->dg[i] = f->dg0[i] * ddecay;
        f->db[i] = f->db0[i] * ddecay;
    }
}

/* ------------------------------------------------------------------ */
/*  Shader sources                                                     */
/* ------------------------------------------------------------------ */

/* ---- Display vertex shader (outputs neighbours for shading) ---- */
static const char *displayVS =
#if defined(PLATFORM_WEB)
    "#version 100\n"
    "attribute vec3 vertexPosition;"
    "attribute vec2 vertexTexCoord;"
    "varying vec2 fragTexCoord;"
    "varying vec2 vL, vR, vT, vB;"
    "uniform vec2 texelSize;"
    "uniform mat4 mvp;"
    "void main() {"
    "  fragTexCoord = vertexTexCoord;"
    "  vL = fragTexCoord + vec2(-texelSize.x, 0.0);"
    "  vR = fragTexCoord + vec2( texelSize.x, 0.0);"
    "  vT = fragTexCoord + vec2(0.0,  texelSize.y);"
    "  vB = fragTexCoord + vec2(0.0, -texelSize.y);"
    "  gl_Position = mvp * vec4(vertexPosition, 1.0);"
    "}";
#else
    "#version 330\n"
    "in vec3 vertexPosition;"
    "in vec2 vertexTexCoord;"
    "out vec2 fragTexCoord;"
    "out vec2 vL; out vec2 vR; out vec2 vT; out vec2 vB;"
    "uniform vec2 texelSize;"
    "uniform mat4 mvp;"
    "void main() {"
    "  fragTexCoord = vertexTexCoord;"
    "  vL = fragTexCoord + vec2(-texelSize.x, 0.0);"
    "  vR = fragTexCoord + vec2( texelSize.x, 0.0);"
    "  vT = fragTexCoord + vec2(0.0,  texelSize.y);"
    "  vB = fragTexCoord + vec2(0.0, -texelSize.y);"
    "  gl_Position = mvp * vec4(vertexPosition, 1.0);"
    "}";
#endif

/* ---- Display fragment shader (shading + bloom + sunrays) ---- */
static const char *displayFS =
#if defined(PLATFORM_WEB)
    "#version 100\n"
    "precision highp float;"
    "varying vec2 fragTexCoord;"
    "varying vec2 vL, vR, vT, vB;"
    "uniform sampler2D texture0;"   /* dye       */
    "uniform sampler2D texture1;"   /* bloom     */
    "uniform sampler2D texture2;"   /* sunrays   */
    "uniform vec4 shadeOn;"         /* .x = shading */
    "void main() {"
    "  vec3 c = texture2D(texture0, fragTexCoord).rgb;"
    "  if (shadeOn.x > 0.5) {"
    "    vec3 lc = texture2D(texture0, vL).rgb;"
    "    vec3 rc = texture2D(texture0, vR).rgb;"
    "    vec3 tc = texture2D(texture0, vT).rgb;"
    "    vec3 bc = texture2D(texture0, vB).rgb;"
    "    float dx = length(rc) - length(lc);"
    "    float dy = length(tc) - length(bc);"
    "    vec3 n = normalize(vec3(dx, dy, 1.0/256.0));"
    "    float diff = dot(n, vec3(0.0,0.0,1.0)) + 0.7;"
    "    diff = clamp(diff, 0.7, 1.0);"
    "    c *= diff;"
    "  }"
    "  c += texture2D(texture1, fragTexCoord).rgb;"  /* bloom */
    "  c *= texture2D(texture2, fragTexCoord).r;"     /* sunrays */
    "  gl_FragColor = vec4(c, 1.0);"
    "}";
#else
    "#version 330\n"
    "in vec2 fragTexCoord;"
    "in vec2 vL; in vec2 vR; in vec2 vT; in vec2 vB;"
    "out vec4 finalColor;"
    "uniform sampler2D texture0;"   /* dye       */
    "uniform sampler2D texture1;"   /* bloom     */
    "uniform sampler2D texture2;"   /* sunrays   */
    "uniform vec4 shadeOn;"         /* .x = shading */
    "void main() {"
    "  vec3 c = texture(texture0, fragTexCoord).rgb;"
    "  if (shadeOn.x > 0.5) {"
    "    vec3 lc = texture(texture0, vL).rgb;"
    "    vec3 rc = texture(texture0, vR).rgb;"
    "    vec3 tc = texture(texture0, vT).rgb;"
    "    vec3 bc = texture(texture0, vB).rgb;"
    "    float dx = length(rc) - length(lc);"
    "    float dy = length(tc) - length(bc);"
    "    vec3 n = normalize(vec3(dx, dy, 1.0/256.0));"
    "    float diff = dot(n, vec3(0.0,0.0,1.0)) + 0.7;"
    "    diff = clamp(diff, 0.7, 1.0);"
    "    c *= diff;"
    "  }"
    "  c += texture(texture1, fragTexCoord).rgb;"
    "  c *= texture(texture2, fragTexCoord).r;"
    "  finalColor = vec4(c, 1.0);"
    "}";
#endif

/* ---- Downsample + threshold (bloom prefilter) ---- */
static const char *bloomPrefilterFS =
#if defined(PLATFORM_WEB)
    "#version 100\nprecision mediump float;"
    "varying vec2 fragTexCoord;"
    "uniform sampler2D texture0;"
    "uniform vec3 curve;"
    "uniform float threshold;"
    "void main() {"
    "  vec3 c = texture2D(texture0, fragTexCoord).rgb;"
    "  float br = max(c.r, max(c.g, c.b));"
    "  float rq = clamp(br - curve.x, 0.0, curve.y);"
    "  rq = curve.z * rq * rq;"
    "  c *= max(rq, br - threshold) / max(br, 0.0001);"
    "  gl_FragColor = vec4(c, 0.0);"
    "}";
#else
    "#version 330\n"
    "in vec2 fragTexCoord;"
    "out vec4 finalColor;"
    "uniform sampler2D texture0;"
    "uniform vec3 curve;"
    "uniform float threshold;"
    "void main() {"
    "  vec3 c = texture(texture0, fragTexCoord).rgb;"
    "  float br = max(c.r, max(c.g, c.b));"
    "  float rq = clamp(br - curve.x, 0.0, curve.y);"
    "  rq = curve.z * rq * rq;"
    "  c *= max(rq, br - threshold) / max(br, 0.0001);"
    "  finalColor = vec4(c, 0.0);"
    "}";
#endif

/* ---- Simple 4-tap blur ---- */
static const char *blurFS =
#if defined(PLATFORM_WEB)
    "#version 100\nprecision mediump float;"
    "varying vec2 fragTexCoord;"
    "varying vec2 vL; varying vec2 vR; varying vec2 vT; varying vec2 vB;"
    "uniform sampler2D texture0;"
    "void main() {"
    "  vec4 c  = texture2D(texture0, fragTexCoord) * 0.25;"
    "  c += texture2D(texture0, vL) * 0.25;"
    "  c += texture2D(texture0, vR) * 0.25;"
    "  c += texture2D(texture0, vT) * 0.25;"
    "  c += texture2D(texture0, vB) * 0.25;"
    "  gl_FragColor = c;"
    "}";
#else
    "#version 330\n"
    "in vec2 fragTexCoord;"
    "in vec2 vL; in vec2 vR; in vec2 vT; in vec2 vB;"
    "out vec4 finalColor;"
    "uniform sampler2D texture0;"
    "void main() {"
    "  vec4 c  = texture(texture0, fragTexCoord) * 0.25;"
    "  c += texture(texture0, vL) * 0.25;"
    "  c += texture(texture0, vR) * 0.25;"
    "  c += texture(texture0, vT) * 0.25;"
    "  c += texture(texture0, vB) * 0.25;"
    "  finalColor = c;"
    "}";
#endif

/* ---- Sunrays mask ---- */
static const char *sunraysMaskFS =
#if defined(PLATFORM_WEB)
    "#version 100\nprecision highp float;"
    "varying vec2 fragTexCoord;"
    "uniform sampler2D texture0;"
    "void main() {"
    "  vec4 c = texture2D(texture0, fragTexCoord);"
    "  float br = max(c.r, max(c.g, c.b));"
    "  c.a = 1.0 - min(max(br * 20.0, 0.0), 0.8);"
    "  gl_FragColor = c;"
    "}";
#else
    "#version 330\n"
    "in vec2 fragTexCoord;"
    "out vec4 finalColor;"
    "uniform sampler2D texture0;"
    "void main() {"
    "  vec4 c = texture(texture0, fragTexCoord);"
    "  float br = max(c.r, max(c.g, c.b));"
    "  c.a = 1.0 - min(max(br * 20.0, 0.0), 0.8);"
    "  finalColor = c;"
    "}";
#endif

/* ---- Sunrays radial blur ---- */
static const char *sunraysFS =
#if defined(PLATFORM_WEB)
    "#version 100\nprecision highp float;"
    "varying vec2 fragTexCoord;"
    "uniform sampler2D texture0;"
    "uniform float weight;"
    "void main() {"
    "  float density = 0.3, decay = 0.95, exposure = 0.7;"
    "  vec2 coord = fragTexCoord;"
    "  vec2 dir = fragTexCoord - 0.5;"
    "  dir *= 1.0/16.0 * density;"
    "  float illDec = 1.0;"
    "  float color = texture2D(texture0, fragTexCoord).a;"
    "  for (int i=0; i<16; i++) {"
    "    coord -= dir;"
    "    color += texture2D(texture0, coord).a * illDec * weight;"
    "    illDec *= decay;"
    "  }"
    "  gl_FragColor = vec4(color * exposure, 0.0, 0.0, 1.0);"
    "}";
#else
    "#version 330\n"
    "in vec2 fragTexCoord;"
    "out vec4 finalColor;"
    "uniform sampler2D texture0;"
    "uniform float weight;"
    "void main() {"
    "  float density = 0.3, decay = 0.95, exposure = 0.7;"
    "  vec2 coord = fragTexCoord;"
    "  vec2 dir = fragTexCoord - 0.5;"
    "  dir *= 1.0/16.0 * density;"
    "  float illDec = 1.0;"
    "  float color = texture(texture0, fragTexCoord).a;"
    "  for (int i=0; i<16; i++) {"
    "    coord -= dir;"
    "    color += texture(texture0, coord).a * illDec * weight;"
    "    illDec *= decay;"
    "  }"
    "  finalColor = vec4(color * exposure, 0.0, 0.0, 1.0);"
    "}";
#endif

/* ---- Full-screen-vert that also outputs L/R/T/B neighbours ---- */
static const char *blurVS =
#if defined(PLATFORM_WEB)
    "#version 100\n"
    "attribute vec3 vertexPosition;"
    "attribute vec2 vertexTexCoord;"
    "varying vec2 fragTexCoord;"
    "varying vec2 vL, vR, vT, vB;"
    "uniform vec2 texelSize;"
    "uniform mat4 mvp;"
    "void main() {"
    "  fragTexCoord = vertexTexCoord;"
    "  vL = fragTexCoord + vec2(-texelSize.x, 0.0);"
    "  vR = fragTexCoord + vec2( texelSize.x, 0.0);"
    "  vT = fragTexCoord + vec2(0.0,  texelSize.y);"
    "  vB = fragTexCoord + vec2(0.0, -texelSize.y);"
    "  gl_Position = mvp * vec4(vertexPosition, 1.0);"
    "}";
#else
    "#version 330\n"
    "in vec3 vertexPosition;"
    "in vec2 vertexTexCoord;"
    "out vec2 fragTexCoord;"
    "out vec2 vL; out vec2 vR; out vec2 vT; out vec2 vB;"
    "uniform vec2 texelSize;"
    "uniform mat4 mvp;"
    "void main() {"
    "  fragTexCoord = vertexTexCoord;"
    "  vL = fragTexCoord + vec2(-texelSize.x, 0.0);"
    "  vR = fragTexCoord + vec2( texelSize.x, 0.0);"
    "  vT = fragTexCoord + vec2(0.0,  texelSize.y);"
    "  vB = fragTexCoord + vec2(0.0, -texelSize.y);"
    "  gl_Position = mvp * vec4(vertexPosition, 1.0);"
    "}";
#endif

/* ---- Default vertex for prefilter / mask shaders ---- */
static const char *defaultVS =
#if defined(PLATFORM_WEB)
    "#version 100\n"
    "attribute vec3 vertexPosition;"
    "attribute vec2 vertexTexCoord;"
    "varying vec2 fragTexCoord;"
    "uniform mat4 mvp;"
    "void main() {"
    "  fragTexCoord = vertexTexCoord;"
    "  gl_Position = mvp * vec4(vertexPosition, 1.0);"
    "}";
#else
    "#version 330\n"
    "in vec3 vertexPosition;"
    "in vec2 vertexTexCoord;"
    "out vec2 fragTexCoord;"
    "uniform mat4 mvp;"
    "void main() {"
    "  fragTexCoord = vertexTexCoord;"
    "  gl_Position = mvp * vec4(vertexPosition, 1.0);"
    "}";
#endif

/* ------------------------------------------------------------------ */
/*  Bloom pyramid data                                                 */
/* ------------------------------------------------------------------ */
#define BLOOM_MAX_LEVELS 8
static RenderTexture2D g_bloomLR  = {0};   /* downsample accumulator */
static RenderTexture2D g_bloomPyr[BLOOM_MAX_LEVELS];
static int             g_bloomLevels = 0;

/* Sunrays textures */
static RenderTexture2D g_sunraysRT    = {0};
static RenderTexture2D g_sunraysTmpRT = {0};

/* Dye texture (updated from CPU each frame) */
static Texture2D        g_dyeTex    = {0};

/* Shader handles */
static Shader g_shaderDisplay   = {0};
static Shader g_shaderPrefilter = {0};
static Shader g_shaderBlur      = {0};
static Shader g_shaderSunMask   = {0};
static Shader g_shaderSun       = {0};

static int g_locTexelSize       = -1;
static int g_locShadeOn         = -1;
static int g_locBloomCurve      = -1;
static int g_locBloomThresh     = -1;
static int g_locSunWeight       = -1;
static int g_locBlurTexelSize   = -1;

/* ------------------------------------------------------------------ */
/*  Post-processing helpers                                            */
/* ------------------------------------------------------------------ */

/* ---- Bloom --------------------------------------------------------- */
static void rebuild_bloom(int w, int h) {
    /* unload previous */
    if (g_bloomLR.id)    UnloadRenderTexture(g_bloomLR);
    for (int i = 0; i < g_bloomLevels; i++)
        UnloadRenderTexture(g_bloomPyr[i]);

    g_bloomLR = LoadRenderTexture(w, h);
    g_bloomLevels = 0;
    for (int i = 0; i < BLOOM_MAX_LEVELS; i++) {
        int lw = w >> (i + 1);
        int lh = h >> (i + 1);
        if (lw < 2 || lh < 2) break;
        g_bloomPyr[i] = LoadRenderTexture(lw, lh);
        g_bloomLevels++;
    }
}

static void apply_bloom(Texture2D src, RenderTexture2D dst) {
    if (g_bloomLevels < 2) return;

    /* --- clear dst to black (safe default) --- */
    BeginTextureMode(dst);
    ClearBackground(BLACK);
    EndTextureMode();

    /* --- prefilter --- */
    BeginTextureMode(g_bloomLR);
    ClearBackground(BLACK);
    BeginShaderMode(g_shaderPrefilter);
    float knee   = g_bloomThreshold * g_bloomSoftKnee + 0.0001f;
    float curve0 = g_bloomThreshold - knee;
    float curve1 = knee * 2.0f;
    float curve2 = 0.25f / knee;
    SetShaderValue(g_shaderPrefilter, g_locBloomCurve,
                   (float[3]){curve0, curve1, curve2}, SHADER_UNIFORM_VEC3);
    SetShaderValue(g_shaderPrefilter, g_locBloomThresh,
                   &g_bloomThreshold, SHADER_UNIFORM_FLOAT);
    SetShaderValueTexture(g_shaderPrefilter,
        GetShaderLocation(g_shaderPrefilter, "texture0"), src);
    DrawRectangle(0, 0, g_bloomLR.texture.width, g_bloomLR.texture.height, WHITE);
    EndShaderMode();
    EndTextureMode();

    /* --- downsample pyramid --- */
    RenderTexture2D last = g_bloomLR;
    BeginShaderMode(g_shaderBlur);
    for (int i = 0; i < g_bloomLevels; i++) {
        float ts[2] = { 1.0f/last.texture.width, 1.0f/last.texture.height };
        SetShaderValue(g_shaderBlur, g_locBlurTexelSize, ts, SHADER_UNIFORM_VEC2);
        BeginTextureMode(g_bloomPyr[i]);
        SetShaderValueTexture(g_shaderBlur,
            GetShaderLocation(g_shaderBlur, "texture0"), last.texture);
        DrawRectangle(0, 0, g_bloomPyr[i].texture.width,
                      g_bloomPyr[i].texture.height, WHITE);
        EndTextureMode();
        last = g_bloomPyr[i];
    }
    EndShaderMode();

    /* --- upsample + accumulate --- */
    rlSetBlendFactors(1, 1, 0x8006);  /* GL_ONE, GL_ONE, GL_FUNC_ADD */
    rlSetBlendMode(BLEND_CUSTOM);
    BeginShaderMode(g_shaderBlur);
    for (int i = g_bloomLevels - 2; i >= 0; i--) {
        float ts[2] = { 1.0f/last.texture.width, 1.0f/last.texture.height };
        SetShaderValue(g_shaderBlur, g_locBlurTexelSize, ts, SHADER_UNIFORM_VEC2);
        BeginTextureMode(g_bloomPyr[i]);
        SetShaderValueTexture(g_shaderBlur,
            GetShaderLocation(g_shaderBlur, "texture0"), last.texture);
        DrawRectangle(0, 0, g_bloomPyr[i].texture.width,
                      g_bloomPyr[i].texture.height, WHITE);
        EndTextureMode();
        last = g_bloomPyr[i];
    }
    EndShaderMode();
    rlSetBlendMode(BLEND_ALPHA);

    /* --- final upsample into dst --- */
    BeginShaderMode(g_shaderBlur);
    {
        float ts[2] = { 1.0f/last.texture.width, 1.0f/last.texture.height };
        SetShaderValue(g_shaderBlur, g_locBlurTexelSize, ts, SHADER_UNIFORM_VEC2);
        BeginTextureMode(dst);
        SetShaderValueTexture(g_shaderBlur,
            GetShaderLocation(g_shaderBlur, "texture0"), last.texture);
        /* apply intensity in a second pass using a cheap multiply */
        DrawRectangle(0, 0, dst.texture.width, dst.texture.height, WHITE);
        EndTextureMode();
    }
    EndShaderMode();
}

/* ---- Sunrays ------------------------------------------------------- */
static void rebuild_sunrays(void) {
    if (g_sunraysRT.id)    UnloadRenderTexture(g_sunraysRT);
    if (g_sunraysTmpRT.id) UnloadRenderTexture(g_sunraysTmpRT);
    int sw = 196, sh = 196;  /* fixed sunrays resolution */
    g_sunraysRT    = LoadRenderTexture(sw, sh);
    g_sunraysTmpRT = LoadRenderTexture(sw, sh);
}

static void apply_sunrays(Texture2D src, RenderTexture2D dst) {
    /* clear dst */
    BeginTextureMode(dst);
    ClearBackground(BLACK);
    EndTextureMode();

    /* mask: copy src + set alpha = 1 - min(max(br*20, 0), 0.8) */
    BeginTextureMode(g_sunraysRT);
    ClearBackground(BLACK);
    BeginShaderMode(g_shaderSunMask);
    SetShaderValueTexture(g_shaderSunMask,
        GetShaderLocation(g_shaderSunMask, "texture0"), src);
    DrawRectangle(0, 0, g_sunraysRT.texture.width, g_sunraysRT.texture.height, WHITE);
    EndShaderMode();
    EndTextureMode();

    /* radial blur */
    BeginTextureMode(dst);
    BeginShaderMode(g_shaderSun);
    SetShaderValue(g_shaderSun, g_locSunWeight,
                   &g_sunraysWeight, SHADER_UNIFORM_FLOAT);
    SetShaderValueTexture(g_shaderSun,
        GetShaderLocation(g_shaderSun, "texture0"), g_sunraysRT.texture);
    DrawRectangle(0, 0, dst.texture.width, dst.texture.height, WHITE);
    EndShaderMode();
    EndTextureMode();
}

/* ------------------------------------------------------------------ */
/*  Texture upload (dye → GPU)                                         */
/* ------------------------------------------------------------------ */
static void upload_dye(Fluid *f) {
    int dw = DYE_W, dh = DYE_H;
    unsigned char *pixels = (unsigned char*)malloc(dw * dh * 4);
    for (int j = 0; j < dh; j++) {
        int simRow = dh - 1 - j;   /* flip Y: sim bottom→image bottom */
        for (int i = 0; i < dw; i++) {
            int idx = simRow * dw + i;
            float r = f->dr[idx];
            float g = f->dg[idx];
            float b = f->db[idx];
            float m = 1.0f + fmaxf(r, fmaxf(g, b));
            int p = (j * dw + i) * 4;
            pixels[p + 0] = (unsigned char)(clampf(r/m, 0,1) * 255);
            pixels[p + 1] = (unsigned char)(clampf(g/m, 0,1) * 255);
            pixels[p + 2] = (unsigned char)(clampf(b/m, 0,1) * 255);
            pixels[p + 3] = 255;
        }
    }
    UpdateTexture(g_dyeTex, pixels);
    free(pixels);
}

/* ------------------------------------------------------------------ */
/*  Input / main loop                                                  */
/* ------------------------------------------------------------------ */
int main(void) {
    /* --- Window ---------------------------------------------------- */
    const int screenW = 1024, screenH = 768;
    SetConfigFlags(FLAG_MSAA_4X_HINT | FLAG_WINDOW_RESIZABLE);
    InitWindow(screenW, screenH, "Navier-Stokes Fluid Simulation – Raylib");
    SetTargetFPS(0);          /* uncapped */

    /* --- Init simulation ------------------------------------------- */
    fluid_init(&g_fluid, SIM_W, SIM_H, DYE_W, DYE_H);

    /* --- Dye texture ----------------------------------------------- */
    {
        Image img = GenImageColor(DYE_W, DYE_H, BLACK);
        g_dyeTex = LoadTextureFromImage(img);
        UnloadImage(img);
        SetTextureFilter(g_dyeTex, TEXTURE_FILTER_BILINEAR);
    }

    /* --- Load shaders ---------------------------------------------- */
    g_shaderDisplay   = LoadShaderFromMemory(displayVS, displayFS);
    g_shaderPrefilter = LoadShaderFromMemory(defaultVS, bloomPrefilterFS);
    g_shaderBlur      = LoadShaderFromMemory(blurVS, blurFS);
    g_shaderSunMask   = LoadShaderFromMemory(defaultVS, sunraysMaskFS);
    g_shaderSun       = LoadShaderFromMemory(defaultVS, sunraysFS);

    g_locTexelSize   = GetShaderLocation(g_shaderDisplay, "texelSize");
    g_locShadeOn     = GetShaderLocation(g_shaderDisplay, "shadeOn");
    g_locBloomCurve  = GetShaderLocation(g_shaderPrefilter, "curve");
    g_locBloomThresh = GetShaderLocation(g_shaderPrefilter, "threshold");
    g_locSunWeight     = GetShaderLocation(g_shaderSun, "weight");
    g_locBlurTexelSize = GetShaderLocation(g_shaderBlur, "texelSize");

    /* --- Post-process textures ------------------------------------- */
    rebuild_bloom(screenW, screenH);
    rebuild_sunrays();

    /* --- Initial random splats ------------------------------------- */
    {
        int count = (rand() % 20) + 5;
        for (int i = 0; i < count; i++) {
            Color c = hsv_to_rgb((float)rand()/RAND_MAX, 1.0f, 1.0f);
            float x  = (float)rand()/RAND_MAX;
            float y  = (float)rand()/RAND_MAX;
            float dx = 1000.0f * ((float)rand()/RAND_MAX - 0.5f);
            float dy = 1000.0f * ((float)rand()/RAND_MAX - 0.5f);
            fluid_splat(&g_fluid, x, y, dx, dy,
                        c.r/255.0f*10.0f, c.g/255.0f*10.0f, c.b/255.0f*10.0f);
        }
    }

    /* --- Per-frame state ------------------------------------------- */
    double lastTime = GetTime();
    float  colorTimer = 0.0f;

    Color  pointerColor = hsv_to_rgb(0.0f, 1.0f, 1.0f);
    bool   mouseDown = false;
    Vector2 lastMouse = {0};

    /* --- Main loop ------------------------------------------------- */
    while (!WindowShouldClose()) {
        /* ---- delta time ------------------------------------------- */
        double now = GetTime();
        float  dt  = (float)(now - lastTime);
        lastTime = now;
        if (dt > 0.05f) dt = 0.016f;  /* clamp for debug / resume     */

        /* ---- resize handling -------------------------------------- */
        if (IsWindowResized()) {
            int nw = GetScreenWidth();
            int nh = GetScreenHeight();
            rebuild_bloom(nw, nh);
        }

        /* ---- colour cycling --------------------------------------- */
        if (g_colorful) {
            colorTimer += dt * 10.0f;
            if (colorTimer >= 1.0f) {
                colorTimer -= 1.0f;
                pointerColor = hsv_to_rgb((float)rand()/RAND_MAX, 1.0f, 1.0f);
            }
        }

        /* ---- input ------------------------------------------------- */
        /* mouse */
        if (IsMouseButtonPressed(MOUSE_LEFT_BUTTON)) {
            mouseDown = true;
            lastMouse = GetMousePosition();
        }
        if (IsMouseButtonReleased(MOUSE_LEFT_BUTTON)) {
            mouseDown = false;
        }
        if (mouseDown) {
            Vector2 m = GetMousePosition();
            Vector2 d = { m.x - lastMouse.x, m.y - lastMouse.y };
            if (d.x != 0.0f || d.y != 0.0f) {
                int sw = GetScreenWidth(), sh = GetScreenHeight();
                float x  = lastMouse.x / sw;
                float y  = 1.0f - lastMouse.y / sh;
                float dx = (d.x / sw)  * g_splatForce;
                float dy = (d.y / sh)  * g_splatForce * -1.0f;
                float cr = pointerColor.r / 255.0f;
                float cg = pointerColor.g / 255.0f;
                float cb = pointerColor.b / 255.0f;
                fluid_splat(&g_fluid, x, y, dx, dy, cr, cg, cb);
            }
            lastMouse = m;
        }

        /* touch */
        int tc = GetTouchPointCount();
        for (int i = 0; i < tc; i++) {
            Vector2 t = GetTouchPosition(i);
            int sw = GetScreenWidth(), sh = GetScreenHeight();
            float x = t.x / sw;
            float y = 1.0f - t.y / sh;
            float cr = pointerColor.r / 255.0f;
            float cg = pointerColor.g / 255.0f;
            float cb = pointerColor.b / 255.0f;
            /* inject a small random velocity */
            float dx = 300.0f * ((float)rand()/RAND_MAX - 0.5f);
            float dy = 300.0f * ((float)rand()/RAND_MAX - 0.5f);
            fluid_splat(&g_fluid, x, y, dx, dy, cr, cg, cb);
        }

        /* keyboard */
        if (IsKeyPressed(KEY_P))     g_paused = !g_paused;
        if (IsKeyPressed(KEY_SPACE)) {
            int count = (rand() % 20) + 5;
            for (int i = 0; i < count; i++) {
                Color c = hsv_to_rgb((float)rand()/RAND_MAX, 1.0f, 1.0f);
                float x  = (float)rand()/RAND_MAX;
                float y  = (float)rand()/RAND_MAX;
                float dx = 1000.0f * ((float)rand()/RAND_MAX - 0.5f);
                float dy = 1000.0f * ((float)rand()/RAND_MAX - 0.5f);
                fluid_splat(&g_fluid, x, y, dx, dy,
                    c.r/255.0f*10.0f, c.g/255.0f*10.0f, c.b/255.0f*10.0f);
            }
        }
        if (IsKeyPressed(KEY_B))     g_bloomEnabled   = !g_bloomEnabled;
        if (IsKeyPressed(KEY_S))     g_sunraysEnabled = !g_sunraysEnabled;
        if (IsKeyPressed(KEY_H))     g_shading = !g_shading;

        /* ---- simulation step -------------------------------------- */
        if (!g_paused) {
            fluid_step(&g_fluid, dt);
        }

        /* ---- upload dye to GPU ------------------------------------ */
        upload_dye(&g_fluid);

        int scrW = GetScreenWidth(), scrH = GetScreenHeight();

        /* ---- final display ---------------------------------------- */
        BeginDrawing();
        ClearBackground(BLACK);

        if (g_shading) {
            BeginShaderMode(g_shaderDisplay);
            {
                float ts[2] = { 1.0f/(float)DYE_W, -1.0f/(float)DYE_H };
                SetShaderValue(g_shaderDisplay, g_locTexelSize, ts, SHADER_UNIFORM_VEC2);
                float shade = 1.0f;
                SetShaderValue(g_shaderDisplay, g_locShadeOn,
                               (float[4]){shade,0,0,0}, SHADER_UNIFORM_VEC4);
                SetShaderValueTexture(g_shaderDisplay,
                    GetShaderLocation(g_shaderDisplay, "texture0"), g_dyeTex);
                static Texture2D blackTex, whiteTex;
                if (!blackTex.id) {
                    Image img = GenImageColor(1,1,BLACK);
                    blackTex = LoadTextureFromImage(img); UnloadImage(img);
                    img = GenImageColor(1,1,WHITE);
                    whiteTex = LoadTextureFromImage(img); UnloadImage(img);
                }
                SetShaderValueTexture(g_shaderDisplay,
                    GetShaderLocation(g_shaderDisplay, "texture1"), blackTex);
                SetShaderValueTexture(g_shaderDisplay,
                    GetShaderLocation(g_shaderDisplay, "texture2"), whiteTex);
            }
            DrawTexturePro(g_dyeTex,
                (Rectangle){0, 0, (float)DYE_W, (float)DYE_H},
                (Rectangle){0, 0, (float)scrW, (float)scrH},
                (Vector2){0, 0}, 0.0f, WHITE);
            EndShaderMode();
        } else {
            DrawTexturePro(g_dyeTex,
                (Rectangle){0, 0, (float)DYE_W, (float)DYE_H},
                (Rectangle){0, 0, (float)scrW, (float)scrH},
                (Vector2){0, 0}, 0.0f, WHITE);
        }

        /* ---- HUD -------------------------------------------------- */
        DrawFPS(8, 8);
        const char *status = g_paused ? "PAUSED" : "RUNNING";
        DrawText(status, 8, 32, 20, g_paused ? RED : GREEN);
        DrawText("Mouse drag  |  SPACE random  |  P pause  |  B bloom  |  S sunrays  |  H shading",
                 8, scrH - 24, 16, GRAY);

        EndDrawing();
    }

    /* --- Cleanup --------------------------------------------------- */
    fluid_free(&g_fluid);
    UnloadTexture(g_dyeTex);
    UnloadShader(g_shaderDisplay);
    UnloadShader(g_shaderPrefilter);
    UnloadShader(g_shaderBlur);
    UnloadShader(g_shaderSunMask);
    UnloadShader(g_shaderSun);
    if (g_bloomLR.id)    UnloadRenderTexture(g_bloomLR);
    for (int i = 0; i < g_bloomLevels; i++)
        UnloadRenderTexture(g_bloomPyr[i]);
    if (g_sunraysRT.id)    UnloadRenderTexture(g_sunraysRT);
    if (g_sunraysTmpRT.id) UnloadRenderTexture(g_sunraysTmpRT);

    CloseWindow();
    return 0;
}
