#include "raylib.h"
#include "raymath.h"
#include <math.h>
#include <stdlib.h>

#define FLUID_W    512
#define FLUID_H    512
#define SCREEN_W  1024
#define SCREEN_H  1024
#define VEL_SCALE  160.0f          // max velocity magnitude (pixels/frame)
#define VEL_SCALE_FULL (VEL_SCALE * 2.0f) // encoding scale used in shaders
#define JACOBI_ITERS 40
#define DISSIPATION  0.995f
#define FORCE_RAD    18.0f
#define MAX_FORCE    8.0f

static void DrawFullscreen(Texture2D src, int dstW, int dstH) {
    DrawTexturePro(src,
        (Rectangle){0, 0, src.width, src.height},
        (Rectangle){0, 0, dstW, dstH},
        (Vector2){0, 0}, 0.0f, WHITE);
}

int main(void) {
    InitWindow(SCREEN_W, SCREEN_H, "Navier-Stokes Fluid Simulation");
    SetTargetFPS(60);

    /* ---------- load shaders ---------- */
    Shader advect    = LoadShader(NULL, "shaders/advect.fs");
    Shader divSh     = LoadShader(NULL, "shaders/divergence.fs");
    Shader jacobi    = LoadShader(NULL, "shaders/jacobi.fs");
    Shader subGrad   = LoadShader(NULL, "shaders/subtract_gradient.fs");
    Shader fadeSh    = LoadShader(NULL, "shaders/fade.fs");
    Shader forceSh   = LoadShader(NULL, "shaders/force.fs");
    Shader renderSh  = LoadShader(NULL, "shaders/render.fs");

    /* ---------- uniform locations ---------- */
    int a_tex0    = GetShaderLocation(advect,   "tex0");
    int a_tex1    = GetShaderLocation(advect,   "tex1");
    int a_fsize   = GetShaderLocation(advect,   "fluidSize");
    int a_vscale  = GetShaderLocation(advect,   "velScale");
    int a_isvel   = GetShaderLocation(advect,   "isVel");

    int d_tex0    = GetShaderLocation(divSh,    "tex0");
    int d_fsize   = GetShaderLocation(divSh,    "fluidSize");

    int j_tex0    = GetShaderLocation(jacobi,   "tex0");
    int j_tex1    = GetShaderLocation(jacobi,   "tex1");
    int j_fsize   = GetShaderLocation(jacobi,   "fluidSize");

    int s_tex0    = GetShaderLocation(subGrad,  "tex0");
    int s_tex1    = GetShaderLocation(subGrad,  "tex1");
    int s_fsize   = GetShaderLocation(subGrad,  "fluidSize");

    int f_tex0    = GetShaderLocation(fadeSh,   "tex0");
    int f_diss    = GetShaderLocation(fadeSh,   "dissipation");

    int fo_maxf   = GetShaderLocation(forceSh,  "maxFluidForce");
    int fo_vscale = GetShaderLocation(forceSh,  "velScale");

    int r_tex0    = GetShaderLocation(renderSh, "tex0");
    int r_tex1    = GetShaderLocation(renderSh, "tex1");
    int r_vscale  = GetShaderLocation(renderSh, "velScale");

    /* ---------- render textures ---------- */
    RenderTexture2D vel[2] = {
        LoadRenderTexture(FLUID_W, FLUID_H),
        LoadRenderTexture(FLUID_W, FLUID_H)
    };
    RenderTexture2D dye[2] = {
        LoadRenderTexture(FLUID_W, FLUID_H),
        LoadRenderTexture(FLUID_W, FLUID_H)
    };
    RenderTexture2D divergence = LoadRenderTexture(FLUID_W, FLUID_H);
    RenderTexture2D pressure[2] = {
        LoadRenderTexture(FLUID_W, FLUID_H),
        LoadRenderTexture(FLUID_W, FLUID_H)
    };

    /* bilinear filtering for all sim textures */
    for (int i = 0; i < 2; i++) {
        SetTextureFilter(vel[i].texture,      TEXTURE_FILTER_BILINEAR);
        SetTextureFilter(dye[i].texture,      TEXTURE_FILTER_BILINEAR);
        SetTextureFilter(pressure[i].texture, TEXTURE_FILTER_BILINEAR);
    }
    SetTextureFilter(divergence.texture, TEXTURE_FILTER_BILINEAR);

    /* ---------- initialise fields ---------- */
    Color zeroVel = {128, 128, 0, 255}; // (0.5,0.5) = zero velocity
    for (int i = 0; i < 2; i++) {
        BeginTextureMode(vel[i]);
            ClearBackground(zeroVel);
        EndTextureMode();
        BeginTextureMode(pressure[i]);
            ClearBackground(BLANK);
        EndTextureMode();
    }

    BeginTextureMode(dye[0]);
        ClearBackground(BLACK);
        // splash some initial dye
        for (int i = 0; i < 7; i++) {
            Color c = {
                GetRandomValue(40, 255),
                GetRandomValue(40, 255),
                GetRandomValue(40, 255),
                255
            };
            DrawCircle(
                GetRandomValue(60, FLUID_W - 60),
                GetRandomValue(60, FLUID_H - 60),
                GetRandomValue(25, 70), c);
        }
    EndTextureMode();
    BeginTextureMode(dye[1]);
        ClearBackground(BLACK);
    EndTextureMode();

    BeginTextureMode(divergence);
        ClearBackground(BLANK);
    EndTextureMode();

    /* 1x1 white texture – used as dummy quad for custom shaders */
    Image whiteImg = GenImageColor(1, 1, WHITE);
    Texture2D whiteTex = LoadTextureFromImage(whiteImg);
    UnloadImage(whiteImg);

    int curV   = 0;
    int curDye = 0;
    int curP   = 0;

    Vector2 fluidSize = { (float)FLUID_W, (float)FLUID_H };
    float velScaleFull = VEL_SCALE_FULL;
    float dissipation  = DISSIPATION;
    float maxFluidForce = MAX_FORCE;

    while (!WindowShouldClose()) {
        Vector2 mouse = GetMousePosition();
        Vector2 delta = GetMouseDelta();
        float mx = mouse.x / (float)SCREEN_W * (float)FLUID_W;
        float my = mouse.y / (float)SCREEN_H * (float)FLUID_H;

        /* -------------------------------------------------
         * 1. Advect Velocity  (read curV -> write 1-curV)
         * ------------------------------------------------- */
        BeginTextureMode(vel[1 - curV]);
            ClearBackground(BLANK);
            BeginShaderMode(advect);
                SetShaderValueTexture(advect, a_tex0, vel[curV].texture);
                SetShaderValueTexture(advect, a_tex1, vel[curV].texture);
                SetShaderValue(advect, a_fsize, &fluidSize, SHADER_UNIFORM_VEC2);
                SetShaderValue(advect, a_vscale, &velScaleFull, SHADER_UNIFORM_FLOAT);
                float isVel1 = 1.0f;
                SetShaderValue(advect, a_isvel, &isVel1, SHADER_UNIFORM_FLOAT);
                DrawFullscreen(whiteTex, FLUID_W, FLUID_H);
            EndShaderMode();
        EndTextureMode();
        curV = 1 - curV;

        /* -------------------------------------------------
         * 2. Advect Dye  (read curDye -> write 1-curDye)
         * ------------------------------------------------- */
        BeginTextureMode(dye[1 - curDye]);
            ClearBackground(BLANK);
            BeginShaderMode(advect);
                SetShaderValueTexture(advect, a_tex0, dye[curDye].texture);
                SetShaderValueTexture(advect, a_tex1, vel[curV].texture);
                SetShaderValue(advect, a_fsize, &fluidSize, SHADER_UNIFORM_VEC2);
                SetShaderValue(advect, a_vscale, &velScaleFull, SHADER_UNIFORM_FLOAT);
                float isVel0 = 0.0f;
                SetShaderValue(advect, a_isvel, &isVel0, SHADER_UNIFORM_FLOAT);
                DrawFullscreen(whiteTex, FLUID_W, FLUID_H);
            EndShaderMode();
        EndTextureMode();
        curDye = 1 - curDye;

        /* -------------------------------------------------
         * 3. Fade Dye  (read curDye -> write 1-curDye)
         * ------------------------------------------------- */
        BeginTextureMode(dye[1 - curDye]);
            ClearBackground(BLANK);
            BeginShaderMode(fadeSh);
                SetShaderValueTexture(fadeSh, f_tex0, dye[curDye].texture);
                SetShaderValue(fadeSh, f_diss, &dissipation, SHADER_UNIFORM_FLOAT);
                DrawFullscreen(whiteTex, FLUID_W, FLUID_H);
            EndShaderMode();
        EndTextureMode();
        curDye = 1 - curDye;

        /* -------------------------------------------------
         * 4. Mouse interaction
         * ------------------------------------------------- */
        if (IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
            /* ---- inject dye (additive splat) ---- */
            Color dyeColor = {255, 55, 15, 200};
            BeginTextureMode(dye[curDye]);
                BeginBlendMode(BLEND_ADDITIVE);
                DrawTexturePro(whiteTex,
                    (Rectangle){0, 0, 1, 1},
                    (Rectangle){mx - FORCE_RAD, my - FORCE_RAD,
                                FORCE_RAD * 2.0f, FORCE_RAD * 2.0f},
                    (Vector2){0, 0}, 0.0f, dyeColor);
                EndBlendMode();
            EndTextureMode();

            /* ---- inject force into velocity ---- */
            if (Vector2Length(delta) > 0.1f) {
                float dx = Clamp(delta.x * 0.08f, -1.0f, 1.0f);
                float dy = Clamp(-delta.y * 0.08f, -1.0f, 1.0f);
                Color forceColor = {
                    (unsigned char)((dx * 0.5f + 0.5f) * 255.0f),
                    (unsigned char)((dy * 0.5f + 0.5f) * 255.0f),
                    127, 255
                };
                BeginTextureMode(vel[curV]);
                    BeginShaderMode(forceSh);
                        SetShaderValue(forceSh, fo_maxf, &maxFluidForce, SHADER_UNIFORM_FLOAT);
                        SetShaderValue(forceSh, fo_vscale, &velScaleFull, SHADER_UNIFORM_FLOAT);
                        DrawTexturePro(whiteTex,
                            (Rectangle){0, 0, 1, 1},
                            (Rectangle){mx - FORCE_RAD, my - FORCE_RAD,
                                        FORCE_RAD * 2.0f, FORCE_RAD * 2.0f},
                            (Vector2){0, 0}, 0.0f, forceColor);
                    EndShaderMode();
                EndTextureMode();
            }
        }

        /* -------------------------------------------------
         * 5. Divergence  (read curV)
         * ------------------------------------------------- */
        BeginTextureMode(divergence);
            ClearBackground(BLANK);
            BeginShaderMode(divSh);
                SetShaderValueTexture(divSh, d_tex0, vel[curV].texture);
                SetShaderValue(divSh, d_fsize, &fluidSize, SHADER_UNIFORM_VEC2);
                DrawFullscreen(whiteTex, FLUID_W, FLUID_H);
            EndShaderMode();
        EndTextureMode();

        /* -------------------------------------------------
         * 6. Jacobi pressure solve
         * ------------------------------------------------- */
        for (int i = 0; i < JACOBI_ITERS; i++) {
            BeginTextureMode(pressure[1 - curP]);
                ClearBackground(BLANK);
                BeginShaderMode(jacobi);
                    SetShaderValueTexture(jacobi, j_tex0, pressure[curP].texture);
                    SetShaderValueTexture(jacobi, j_tex1, divergence.texture);
                    SetShaderValue(jacobi, j_fsize, &fluidSize, SHADER_UNIFORM_VEC2);
                    DrawFullscreen(whiteTex, FLUID_W, FLUID_H);
                EndShaderMode();
            EndTextureMode();
            curP = 1 - curP;
        }

        /* -------------------------------------------------
         * 7. Subtract pressure gradient
         *    (read curV, write 1-curV)
         * ------------------------------------------------- */
        BeginTextureMode(vel[1 - curV]);
            ClearBackground(BLANK);
            BeginShaderMode(subGrad);
                SetShaderValueTexture(subGrad, s_tex0, vel[curV].texture);
                SetShaderValueTexture(subGrad, s_tex1, pressure[curP].texture);
                SetShaderValue(subGrad, s_fsize, &fluidSize, SHADER_UNIFORM_VEC2);
                DrawFullscreen(whiteTex, FLUID_W, FLUID_H);
            EndShaderMode();
        EndTextureMode();
        curV = 1 - curV;

        /* -------------------------------------------------
         * 8. Render to screen
         * ------------------------------------------------- */
        BeginDrawing();
            ClearBackground(BLACK);
            BeginShaderMode(renderSh);
                SetShaderValueTexture(renderSh, r_tex0, vel[curV].texture);
                SetShaderValueTexture(renderSh, r_tex1, dye[curDye].texture);
                SetShaderValue(renderSh, r_vscale, &velScaleFull, SHADER_UNIFORM_FLOAT);
                DrawFullscreen(whiteTex, SCREEN_W, SCREEN_H);
            EndShaderMode();

            DrawText("Drag left mouse to inject dye & force", 10, 10, 20, RAYWHITE);
            DrawText(TextFormat("Jacobi: %d  Dissipation: %.3f", JACOBI_ITERS, DISSIPATION),
                     10, 35, 20, LIGHTGRAY);
        EndDrawing();
    }

    /* ---------- cleanup ---------- */
    UnloadTexture(whiteTex);
    for (int i = 0; i < 2; i++) {
        UnloadRenderTexture(vel[i]);
        UnloadRenderTexture(dye[i]);
        UnloadRenderTexture(pressure[i]);
    }
    UnloadRenderTexture(divergence);
    UnloadShader(advect);
    UnloadShader(divSh);
    UnloadShader(jacobi);
    UnloadShader(subGrad);
    UnloadShader(fadeSh);
    UnloadShader(forceSh);
    UnloadShader(renderSh);
    CloseWindow();
    return 0;
}
