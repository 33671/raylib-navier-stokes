/***************************************************************************
 *  Navier-Stokes Fluid Simulation — DIAGNOSTIC VERSION
 *
 *  V    = toggle raw display: dye → velocity-R → velocity-G → divergence
 *  L/R-click = draw force + dye  (works even when paused)
 *  Space = pause/resume simulation
 *  R    = reset
 *  1    = seed constant velocity  (tests advection)
 *  2    = run ONLY step 1 (advect vel) once
 *  3    = run FULL pipeline once
 ****************************************************************************/

#include "raylib/src/raylib.h"
#include "raylib/src/rlgl.h"
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

#define FLUID_SIZE         256
#define SCREEN_WIDTH       1024
#define SCREEN_HEIGHT      768
#define JACOBI_ITERATIONS  40
#define VEL_SCALE          80.0f
#define DYE_DISSIPATION    0.995f

// Raw display modes  (no debug shader — shows texture as-is)
enum { SHOW_DYE = 0, SHOW_VEL, SHOW_DIVERGENCE, SHOW_COUNT };

static inline int Swap(int c) { return 1 - c; }

// ---- Texture binding helper -----------------------------------------------
static void BindTex(Shader s, const char *name, Texture2D t, int unit)
{
    int loc = GetShaderLocation(s, name);
    rlActiveTextureSlot(unit);
    rlEnableTexture(t.id);
    rlSetUniform(loc, &unit, RL_SHADER_UNIFORM_INT, 1);
}

// ---- Full-screen shader pass ---------------------------------------------
static void Pass(RenderTexture2D dst, Shader s, Texture2D t0, Texture2D t1)
{
    BeginTextureMode(dst);
        ClearBackground(BLANK);
        BeginShaderMode(s);
            if (t0.id) BindTex(s, "texture0", t0, 0);
            if (t1.id) BindTex(s, "texture1", t1, 1);
            DrawRectangle(0, 0, dst.texture.width, dst.texture.height, WHITE);
        EndShaderMode();
    EndTextureMode();
    rlActiveTextureSlot(0); rlDisableTexture();
    rlActiveTextureSlot(1); rlDisableTexture();
}

// =========================================================================
int main(void)
{
    SetConfigFlags(FLAG_WINDOW_RESIZABLE);
    InitWindow(SCREEN_WIDTH, SCREEN_HEIGHT,
               "FLUID DIAGNOSTIC  [V=view 1=seed 2/3=step  Space=pause]");
    SetTargetFPS(0);

    // ---- Render targets --------------------------------------------------
    RenderTexture2D vel[2], dye[2], pressure[2], divergence;
    for (int i = 0; i < 2; i++) {
        vel[i] = LoadRenderTexture(FLUID_SIZE, FLUID_SIZE);
        dye[i] = LoadRenderTexture(FLUID_SIZE, FLUID_SIZE);
        pressure[i] = LoadRenderTexture(FLUID_SIZE, FLUID_SIZE);
        SetTextureFilter(vel[i].texture, TEXTURE_FILTER_BILINEAR);
        SetTextureFilter(dye[i].texture, TEXTURE_FILTER_BILINEAR);
        SetTextureFilter(pressure[i].texture, TEXTURE_FILTER_BILINEAR);
    }
    divergence = LoadRenderTexture(FLUID_SIZE, FLUID_SIZE);
    SetTextureFilter(divergence.texture, TEXTURE_FILTER_BILINEAR);

    // ---- Shaders ---------------------------------------------------------
    Shader shAdvect     = LoadShader("shaders/passthrough.vs",
                                     "shaders/advect.fs");
    Shader shFade       = LoadShader("shaders/passthrough.vs",
                                     "shaders/fade.fs");
    Shader shDivergence = LoadShader("shaders/passthrough.vs",
                                     "shaders/divergence.fs");
    Shader shJacobi     = LoadShader("shaders/passthrough.vs",
                                     "shaders/jacobi.fs");
    Shader shSubGrad    = LoadShader("shaders/passthrough.vs",
                                     "shaders/subtract_gradient.fs");

    // Uniforms
    float fs[2] = { FLUID_SIZE, FLUID_SIZE };
    float vs    = 2.0f * VEL_SCALE;
    float diss  = DYE_DISSIPATION;

    SetShaderValue(shAdvect, GetShaderLocation(shAdvect, "fluidSize"),
                   fs, SHADER_UNIFORM_VEC2);
    SetShaderValue(shAdvect, GetShaderLocation(shAdvect, "velScale"),
                   &vs, SHADER_UNIFORM_FLOAT);
    SetShaderValue(shDivergence, GetShaderLocation(shDivergence, "fluidSize"),
                   fs, SHADER_UNIFORM_VEC2);
    SetShaderValue(shJacobi, GetShaderLocation(shJacobi, "fluidSize"),
                   fs, SHADER_UNIFORM_VEC2);
    SetShaderValue(shSubGrad, GetShaderLocation(shSubGrad, "fluidSize"),
                   fs, SHADER_UNIFORM_VEC2);
    SetShaderValue(shFade, GetShaderLocation(shFade, "dissipation"),
                   &diss, SHADER_UNIFORM_FLOAT);

    int locIsVel = GetShaderLocation(shAdvect, "isVel");

    // ---- Clear -----------------------------------------------------------
    for (int i = 0; i < 2; i++) {
        BeginTextureMode(vel[i]); ClearBackground(BLANK); EndTextureMode();
        BeginTextureMode(dye[i]); ClearBackground(BLANK); EndTextureMode();
        BeginTextureMode(pressure[i]); ClearBackground(BLANK); EndTextureMode();
    }
    BeginTextureMode(divergence); ClearBackground(BLANK); EndTextureMode();

    // ---- Seed dye so we have something to look at ------------------------
    BeginTextureMode(dye[0]);
        DrawCircle(FLUID_SIZE/2, FLUID_SIZE/2, 25, ColorFromHSV(200,0.9f,0.9f));
        DrawCircle(FLUID_SIZE/2+30,FLUID_SIZE/2-10,18, ColorFromHSV(50,0.9f,0.9f));
        DrawCircle(FLUID_SIZE/2-30,FLUID_SIZE/2+10,18, ColorFromHSV(340,0.9f,0.9f));
    EndTextureMode();

    // ---- State -----------------------------------------------------------
    int curVel=0, curDye=0, curPres=0;
    int showWhat = SHOW_DYE;
    bool paused  = false;
    Vector2 prevMouse = { -1, -1 };

    while (!WindowShouldClose())
    {
        float dt = GetFrameTime();
        if (dt > 0.033f) dt = 0.016f;

        Vector2 m = GetMousePosition();
        bool mdown = IsMouseButtonDown(MOUSE_BUTTON_LEFT) ||
                     IsMouseButtonDown(MOUSE_BUTTON_RIGHT);

        // Fit square
        float dw = GetScreenWidth(), dh = GetScreenHeight();
        float drawW, drawH, offX, offY;
        if (dw/dh > 1.0f) {
            drawH=dh; drawW=drawH; offX=(dw-drawW)*0.5f; offY=0;
        } else {
            drawW=dw; drawH=drawW; offX=0; offY=(dh-drawH)*0.5f;
        }

        Vector2 fUV = { (m.x-offX)/drawW, (m.y-offY)/drawH };
        bool inside = (fUV.x>=0 && fUV.x<=1 && fUV.y>=0 && fUV.y<=1);

        // ---- Controls ----------------------------------------------------
        if (IsKeyPressed(KEY_R)) {
            for (int i=0; i<2; i++) {
                BeginTextureMode(vel[i]); ClearBackground(BLANK); EndTextureMode();
                BeginTextureMode(dye[i]); ClearBackground(BLANK); EndTextureMode();
                BeginTextureMode(pressure[i]); ClearBackground(BLANK); EndTextureMode();
            }
            BeginTextureMode(divergence); ClearBackground(BLANK); EndTextureMode();
            // Re-seed dye
            BeginTextureMode(dye[0]);
                DrawCircle(FLUID_SIZE/2,FLUID_SIZE/2,25,ColorFromHSV(200,0.9f,0.9f));
            EndTextureMode();
            curVel=0; curDye=0; curPres=0;
        }
        if (IsKeyPressed(KEY_SPACE)) paused = !paused;
        if (IsKeyPressed(KEY_V)) showWhat = (showWhat+1) % SHOW_COUNT;

        // ---- 1. Seed constant velocity  (test advection) -----------------
        if (IsKeyPressed(KEY_ONE)) {
            // Fill vel[0] and vel[1] with constant rightward vx=+30
            Color fc = { 175, 128, 0, 255 };  // (30/80*0.5+0.5)*255 = 175
            for (int i = 0; i < 2; i++) {
                BeginTextureMode(vel[i]);
                    DrawRectangle(0, 0, FLUID_SIZE, FLUID_SIZE, fc);
                EndTextureMode();
            }
            // Seed dye blobs
            BeginTextureMode(dye[curDye]);
                DrawCircle(60, FLUID_SIZE/2, 20, ColorFromHSV(120,1,1));
                DrawCircle(FLUID_SIZE/2, 50, 15, ColorFromHSV(280,1,1));
            EndTextureMode();
            curVel = 0;
        }

        // ---- 2. Single advection step ------------------------------------
        if (IsKeyPressed(KEY_TWO)) {
            float one = 1.0f;
            SetShaderValue(shAdvect, locIsVel, &one, SHADER_UNIFORM_FLOAT);
            Pass(vel[Swap(curVel)], shAdvect,
                 vel[curVel].texture, vel[curVel].texture);
            curVel = Swap(curVel);
        }

        // ---- 3. Full pipeline once ---------------------------------------
        if (IsKeyPressed(KEY_THREE)) {
            float isVelVal;

            // Advect velocity
            isVelVal = 1.0f;
            SetShaderValue(shAdvect, locIsVel, &isVelVal, SHADER_UNIFORM_FLOAT);
            Pass(vel[Swap(curVel)], shAdvect,
                 vel[curVel].texture, vel[curVel].texture);
            curVel = Swap(curVel);

            // Advect dye
            isVelVal = 0.0f;
            SetShaderValue(shAdvect, locIsVel, &isVelVal, SHADER_UNIFORM_FLOAT);
            Pass(dye[Swap(curDye)], shAdvect,
                 dye[curDye].texture, vel[curVel].texture);
            curDye = Swap(curDye);

            // Fade
            Pass(dye[Swap(curDye)], shFade,
                 dye[curDye].texture, (Texture2D){0});
            curDye = Swap(curDye);

            // Divergence
            BeginTextureMode(divergence); ClearBackground(BLANK);
            EndTextureMode();
            Pass(divergence, shDivergence,
                 vel[curVel].texture, (Texture2D){0});

            // Jacobi
            BeginTextureMode(pressure[curPres]); ClearBackground(BLANK);
            EndTextureMode();
            BeginTextureMode(pressure[Swap(curPres)]); ClearBackground(BLANK);
            EndTextureMode();
            for (int i=0; i<JACOBI_ITERATIONS; i++) {
                Pass(pressure[Swap(curPres)], shJacobi,
                     pressure[curPres].texture, divergence.texture);
                curPres = Swap(curPres);
            }

            // Subtract gradient
            Pass(vel[Swap(curVel)], shSubGrad,
                 vel[curVel].texture, pressure[curPres].texture);
            curVel = Swap(curVel);

            // Clear pressure
            BeginTextureMode(pressure[Swap(curPres)]); ClearBackground(BLANK);
            EndTextureMode();
            curPres = Swap(curPres);
        }

        // ---- Continuous simulation (when not paused) ---------------------
        if (!paused)
        {
            float iv;
            // 1
            iv=1.0f; SetShaderValue(shAdvect,locIsVel,&iv,SHADER_UNIFORM_FLOAT);
            Pass(vel[Swap(curVel)], shAdvect, vel[curVel].texture,
                 vel[curVel].texture);
            curVel = Swap(curVel);
            // 2
            iv=0.0f; SetShaderValue(shAdvect,locIsVel,&iv,SHADER_UNIFORM_FLOAT);
            Pass(dye[Swap(curDye)], shAdvect, dye[curDye].texture,
                 vel[curVel].texture);
            curDye = Swap(curDye);
            // 3
            Pass(dye[Swap(curDye)], shFade, dye[curDye].texture,
                 (Texture2D){0});
            curDye = Swap(curDye);
            // 4 — forces  (drawn even when paused)
        }

        // ---- 4. Forces  (ALWAYS active, paused or not) -------------------
        if (mdown && inside)
        {
            float fx=0, fy=0;
            if (prevMouse.x >= 0) {
                fx = (m.x - prevMouse.x) * 6.0f;
                fy = (m.y - prevMouse.y) * 6.0f;
            }
            int px = (int)(fUV.x * FLUID_SIZE);
            int py = (int)(fUV.y * FLUID_SIZE);
            int r  = FLUID_SIZE / 14;
            if (r<5) r=5;

            if (fx> VEL_SCALE) fx= VEL_SCALE;
            if (fx<-VEL_SCALE) fx=-VEL_SCALE;
            if (fy> VEL_SCALE) fy= VEL_SCALE;
            if (fy<-VEL_SCALE) fy=-VEL_SCALE;

            Color fc = {
                (unsigned char)((fx/VEL_SCALE*0.5f+0.5f)*255),
                (unsigned char)((fy/VEL_SCALE*0.5f+0.5f)*255),
                0, 255
            };
            BeginTextureMode(vel[curVel]);
                DrawCircle(px, py, (float)r, fc);
            EndTextureMode();

            float hue = fmodf(GetTime()*0.4f, 1.0f);
            BeginTextureMode(dye[curDye]);
                DrawCircle(px, py, (float)(r+8),
                           ColorFromHSV(hue*360, 1.0f, 1.0f));
            EndTextureMode();
        }
        prevMouse = m;

        // ---- Continuous sim continued (steps 5-8) ------------------------
        if (!paused)
        {
            // 5
            BeginTextureMode(divergence); ClearBackground(BLANK);
            EndTextureMode();
            Pass(divergence, shDivergence, vel[curVel].texture,
                 (Texture2D){0});
            // 6
            BeginTextureMode(pressure[curPres]); ClearBackground(BLANK);
            EndTextureMode();
            BeginTextureMode(pressure[Swap(curPres)]); ClearBackground(BLANK);
            EndTextureMode();
            for (int i=0; i<JACOBI_ITERATIONS; i++) {
                Pass(pressure[Swap(curPres)], shJacobi,
                     pressure[curPres].texture, divergence.texture);
                curPres = Swap(curPres);
            }
            // 7
            Pass(vel[Swap(curVel)], shSubGrad, vel[curVel].texture,
                 pressure[curPres].texture);
            curVel = Swap(curVel);
            // 8
            BeginTextureMode(pressure[Swap(curPres)]); ClearBackground(BLANK);
            EndTextureMode();
            curPres = Swap(curPres);
        }

        // ---- Render ------------------------------------------------------
        BeginDrawing();
            ClearBackground(BLACK);

            Rectangle dstRect = { offX, offY, drawW, drawH };
            Rectangle srcRect = { 0, 0, FLUID_SIZE, (float)-FLUID_SIZE };

            Texture2D texToShow;
            const char *label = "";

            switch (showWhat) {
            case SHOW_DYE:
                texToShow = dye[curDye].texture;
                label = "DYE  (raw)";
                break;
            case SHOW_VEL:
                texToShow = vel[curVel].texture;
                label = "VELOCITY  (raw RGBA)  — R=vx G=vy  0.5=zero";
                break;
            case SHOW_DIVERGENCE:
                texToShow = divergence.texture;
                label = "DIVERGENCE  (raw)  — R channel only";
                break;
            }

            DrawTexturePro(texToShow, srcRect, dstRect,
                           (Vector2){0,0}, 0.0f, WHITE);

            // UI
            DrawFPS(10, 10);
            DrawText(paused ? "PAUSED" : "RUNNING", 10, 40, 20,
                     paused ? RED : GREEN);
            DrawText(label, 10, 68, 20, YELLOW);
            char buf[128];
            snprintf(buf, sizeof(buf),
                     "curVel=%d curDye=%d  V=view  1=seed  2=step1  3=full-step",
                     curVel, curDye);
            DrawText(buf, 10, 94, 20, LIGHTGRAY);
            DrawText("L/R-click=drag force (always on)  Space=pause  R=reset",
                     10, 120, 20, LIGHTGRAY);
        EndDrawing();
    }

    UnloadShader(shAdvect); UnloadShader(shFade);
    UnloadShader(shDivergence); UnloadShader(shJacobi);
    UnloadShader(shSubGrad);
    UnloadRenderTexture(divergence);
    for (int i=0; i<2; i++) {
        UnloadRenderTexture(vel[i]);
        UnloadRenderTexture(dye[i]);
        UnloadRenderTexture(pressure[i]);
    }
    CloseWindow();
    return 0;
}
