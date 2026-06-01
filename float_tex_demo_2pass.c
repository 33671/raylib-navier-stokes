/******************************************************************************
 *  float_tex_demo_2pass.c
 *  双 Pass 渲染演示：
 *    Pass 1 — 用 R32G32B32A32F 纹理 + 动画 shader，渲染到 offscreen FBO
 *    Pass 2 — 对 Pass 1 的结果做后处理，再输出到屏幕
 *
 *  编译:
 *    gcc -Wall -Wextra -O2 -I./raylib/src float_tex_demo_2pass.c \
 *        ./raylib/src/libraylib.a -lGL -lm -lpthread -ldl -lrt -lX11 \
 *        -o float_tex_demo_2pass
 *
 *  运行:
 *    ./float_tex_demo_2pass
 ******************************************************************************/

#include "raylib.h"
#include "rlgl.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

int main(void)
{
    const int screenWidth  = 800;
    const int screenHeight = 600;

    InitWindow(screenWidth, screenHeight, "2-Pass: R32G32B32A32F + Post-Processing");

    // ---------------------------------------------------------------
    // 1. 生成彩色格子数据 (R32G32B32A32F)
    // ---------------------------------------------------------------
    const int texW = 512;
    const int texH = 512;
    const int cellSize = 64;

    float *pixels = (float *)RL_MALLOC(texW * texH * 4 * sizeof(float));

    for (int y = 0; y < texH; y++)
    {
        for (int x = 0; x < texW; x++)
        {
            int idx = (y * texW + x) * 4;
            int cx = x / cellSize;
            int cy = y / cellSize;

            if ((cx + cy) % 2 == 0)
            {
                float r = 0.2f + 0.8f * ((float)x / texW);
                float g = 0.1f + 0.3f * ((float)y / texH);
                float b = 0.1f;
                float a = 0.8f + 0.2f * ((float)(x + y) / (texW + texH));
                pixels[idx + 0] = r;
                pixels[idx + 1] = g;
                pixels[idx + 2] = b;
                pixels[idx + 3] = a;
            }
            else
            {
                float r = 0.1f;
                float g = 0.2f + 0.4f * ((float)y / texH);
                float b = 0.5f + 0.5f * ((float)x / texW);
                float a = 0.6f + 0.4f * (1.0f - (float)(x + y) / (texW + texH));
                pixels[idx + 0] = r;
                pixels[idx + 1] = g;
                pixels[idx + 2] = b;
                pixels[idx + 3] = a;
            }
        }
    }

    // 上传 R32G32B32A32F 纹理
    unsigned int texId = rlLoadTexture(pixels, texW, texH,
                                       PIXELFORMAT_UNCOMPRESSED_R32G32B32A32, 1);
    RL_FREE(pixels);

    Texture2D floatTex = {
        .id      = texId,
        .width   = texW,
        .height  = texH,
        .mipmaps = 1,
        .format  = PIXELFORMAT_UNCOMPRESSED_R32G32B32A32
    };

    // ---------------------------------------------------------------
    // 2. 创建 offscreen FBO (Pass 1 的输出目标)
    //    注意：LoadRenderTexture 固定 RGBA8，对后处理来说完全够用
    // ---------------------------------------------------------------
    RenderTexture2D fbo = LoadRenderTexture(texW, texH);

    // ---------------------------------------------------------------
    // 3. Pass 1 Shader — 动画 (和之前一样)
    // ---------------------------------------------------------------
    const char *fsPass1 =
        "#version 330\n"
        "in vec2 fragTexCoord;\n"
        "in vec4 fragColor;\n"
        "out vec4 finalColor;\n"
        "uniform sampler2D texture0;\n"
        "uniform vec4 colDiffuse;\n"
        "uniform float uTime;\n"
        "void main()\n"
        "{\n"
        "    float t = uTime;\n"
        "\n"
        "    // 波浪扭曲\n"
        "    vec2 uv = fragTexCoord;\n"
        "    uv.x += 0.03 * sin(uv.y * 20.0 + t * 3.0);\n"
        "    uv.y += 0.03 * cos(uv.x * 20.0 + t * 2.5);\n"
        "\n"
        "    // 缩放脉冲\n"
        "    float pulse = 1.0 + 0.08 * sin(t * 1.5);\n"
        "    uv = (uv - 0.5) * pulse + 0.5;\n"
        "\n"
        "    // 旋转\n"
        "    float angle = t * 0.2;\n"
        "    float sA = sin(angle);\n"
        "    float cA = cos(angle);\n"
        "    vec2 ruv = uv - 0.5;\n"
        "    uv = vec2(ruv.x * cA - ruv.y * sA, ruv.x * sA + ruv.y * cA) + 0.5;\n"
        "\n"
        "    vec4 texColor = texture(texture0, uv);\n"
        "\n"
        "    // RGB 分裂\n"
        "    float shift = 0.02 * sin(t * 2.0);\n"
        "    float r = texture(texture0, uv + vec2( shift, 0.0)).r;\n"
        "    float g = texture(texture0, uv + vec2( 0.0, 0.0)).g;\n"
        "    float b = texture(texture0, uv + vec2(-shift, 0.0)).b;\n"
        "\n"
        "    vec4 c = vec4(r, g, b, texColor.a);\n"
        "\n"
        "    // 光晕脉冲\n"
        "    float dist = length(fragTexCoord - 0.5);\n"
        "    float glow = 0.5 + 0.5 * sin(dist * 8.0 - t * 4.0);\n"
        "    c.rgb += glow * 0.1;\n"
        "\n"
        "    finalColor = c * colDiffuse * fragColor;\n"
        "}\n";

    Shader shaderPass1 = LoadShaderFromMemory(NULL, fsPass1);
    int locTime1  = GetShaderLocation(shaderPass1, "uTime");

    // ---------------------------------------------------------------
    // 4. Pass 2 Shader — 后处理
    //    效果：像素化 + 扫描线 + 晕影(Vignette) + 色差
    //    一眼就能看出是后处理！
    // ---------------------------------------------------------------
    const char *fsPass2 =
        "#version 330\n"
        "in vec2 fragTexCoord;\n"
        "in vec4 fragColor;\n"
        "out vec4 finalColor;\n"
        "uniform sampler2D texture0;\n"
        "uniform vec4 colDiffuse;\n"
        "uniform float uTime;\n"
        "uniform vec2 uResolution;\n"
        "uniform int effect;\n"   // 0~5 切换不同效果
        "void main()\n"
        "{\n"
        "    vec2 uv = fragTexCoord;\n"
        "    vec4 c;\n"
        "\n"
        "    // ----- 效果 0: 像素化 (Mosaic) -----\n"
        "    if (effect == 0)\n"
        "    {\n"
        "        float cell = 16.0 + 8.0 * sin(uTime * 0.5);\n"
        "        vec2 puv = floor(uv * cell) / cell;\n"
        "        c = texture(texture0, puv);\n"
        "    }\n"
        "    // ----- 效果 1: 扫描线 + 故障 (Scanlines + Glitch) -----\n"
        "    else if (effect == 1)\n"
        "    {\n"
        "        c = texture(texture0, uv);\n"
        "        // 扫描线\n"
        "        float scanline = sin(uv.y * uResolution.y * 3.14159 * 0.5);\n"
        "        scanline = abs(scanline) * 0.4 + 0.6;\n"
        "        // 横向随机噪条\n"
        "        float glitch = step(0.97, sin(uv.y * 200.0 + uTime * 10.0));\n"
        "        vec2 guv = uv + vec2(glitch * 0.05 * sin(uTime * 5.0), 0.0);\n"
        "        c = texture(texture0, guv);\n"
        "        c.rgb *= scanline;\n"
        "        // 偶尔闪白\n"
        "        c.rgb += glitch * 0.3;\n"
        "    }\n"
        "    // ----- 效果 2: 色差 + 晕影 (Chromatic Aberration + Vignette) -----\n"
        "    else if (effect == 2)\n"
        "    {\n"
        "        float strength = 0.01 + 0.005 * sin(uTime);\n"
        "        float r = texture(texture0, uv + vec2(strength, 0.0)).r;\n"
        "        float g = texture(texture0, uv + vec2(0.0, 0.0)).g;\n"
        "        float b = texture(texture0, uv - vec2(strength, 0.0)).b;\n"
        "        c = vec4(r, g, b, 1.0);\n"
        "        // 晕影\n"
        "        float vig = 1.0 - length(uv - 0.5) * 0.8;\n"
        "        c.rgb *= vig;\n"
        "    }\n"
        "    // ----- 效果 3: 边缘发光 (Edge Glow / Sobel) -----\n"
        "    else if (effect == 3)\n"
        "    {\n"
        "        vec2 off = 1.0 / uResolution;\n"
        "        float tl = length(texture(texture0, uv + vec2(-off.x,  off.y)).rgb);\n"
        "        float t  = length(texture(texture0, uv + vec2( 0.0,   off.y)).rgb);\n"
        "        float tr = length(texture(texture0, uv + vec2( off.x,  off.y)).rgb);\n"
        "        float l  = length(texture(texture0, uv + vec2(-off.x,  0.0)).rgb);\n"
        "        float r  = length(texture(texture0, uv + vec2( off.x,  0.0)).rgb);\n"
        "        float bl = length(texture(texture0, uv + vec2(-off.x, -off.y)).rgb);\n"
        "        float b  = length(texture(texture0, uv + vec2( 0.0,  -off.y)).rgb);\n"
        "        float br = length(texture(texture0, uv + vec2( off.x, -off.y)).rgb);\n"
        "        float gx = -tl - 2.0*t - tr + bl + 2.0*b + br;\n"
        "        float gy = -tl - 2.0*l - bl + tr + 2.0*r + br;\n"
        "        float edge = sqrt(gx*gx + gy*gy);\n"
        "        vec4 orig = texture(texture0, uv);\n"
        "        c = mix(orig, vec4(edge * 2.0), 0.6 + 0.3 * sin(uTime));\n"
        "    }\n"
        "    // ----- 效果 4: 极坐标扭曲 (Polar Twirl) -----\n"
        "    else if (effect == 4)\n"
        "    {\n"
        "        vec2 p = uv - 0.5;\n"
        "        float radius = length(p);\n"
        "        float angle = atan(p.y, p.x);\n"
        "        angle += uTime * 0.5 * (1.0 - radius);\n"
        "        vec2 twirl = vec2(cos(angle), sin(angle)) * radius + 0.5;\n"
        "        c = texture(texture0, twirl);\n"
        "    }\n"
        "    // ----- 效果 5: CRT 模拟 (所有效果大杂烩) -----\n"
        "    else\n"
        "    {\n"
        "        // 像素化\n"
        "        vec2 crtRes = vec2(320.0, 240.0);\n"
        "        vec2 crtUv = floor(uv * crtRes) / crtRes;\n"
        "        c = texture(texture0, crtUv);\n"
        "        // 扫描线\n"
        "        float scan = 0.5 + 0.5 * sin(uv.y * uResolution.y * 3.14159);\n"
        "        c.rgb *= 0.7 + 0.3 * scan;\n"
        "        // 色差\n"
        "        float caOff = 0.003;\n"
        "        c.r = texture(texture0, crtUv + vec2(caOff, 0.0)).r;\n"
        "        c.b = texture(texture0, crtUv - vec2(caOff, 0.0)).b;\n"
        "        // 晕影\n"
        "        float v = 1.0 - length(uv - 0.5) * 0.6;\n"
        "        c.rgb *= v;\n"
        "        // 屏幕 curvature\n"
        "        vec2 cur = (uv - 0.5) * 1.02;\n"
        "        cur *= 1.0 + dot(cur, cur) * 0.3;\n"
        "        if (abs(cur.x) > 0.5 || abs(cur.y) > 0.5)\n"
        "            c = vec4(0.0);\n"
        "    }\n"
        "\n"
        "    finalColor = c * colDiffuse * fragColor;\n"
        "}\n";

    Shader shaderPass2 = LoadShaderFromMemory(NULL, fsPass2);
    int locTime2  = GetShaderLocation(shaderPass2, "uTime");
    int locRes    = GetShaderLocation(shaderPass2, "uResolution");
    int locEffect = GetShaderLocation(shaderPass2, "effect");

    int currentEffect = 0;
    int pauseAnim = 0;
    float time = 0.0f;

    SetTargetFPS(60);

    // ---------------------------------------------------------------
    // 5. 主循环 — 双 Pass 渲染
    // ---------------------------------------------------------------
    while (!WindowShouldClose())
    {
        if (!pauseAnim) time += GetFrameTime();

        // 按键切换后处理效果
        if (IsKeyPressed(KEY_SPACE))
        {
            pauseAnim = !pauseAnim;
        }
        if (IsKeyPressed(KEY_RIGHT) || IsKeyPressed(KEY_DOWN))
        {
            currentEffect = (currentEffect + 1) % 6;
            TraceLog(LOG_INFO, "Post-effect: %d", currentEffect);
        }
        if (IsKeyPressed(KEY_LEFT) || IsKeyPressed(KEY_UP))
        {
            currentEffect = (currentEffect - 1 + 6) % 6;
            TraceLog(LOG_INFO, "Post-effect: %d", currentEffect);
        }
        if (IsKeyPressed(KEY_R))
        {
            time = 0.0f;
            pauseAnim = 0;
            currentEffect = 0;
        }

        // ============ Pass 1: 渲染 float 纹理到 FBO ============
        BeginTextureMode(fbo);
        ClearBackground(BLACK);

        BeginShaderMode(shaderPass1);
        SetShaderValue(shaderPass1, locTime1, &time, SHADER_UNIFORM_FLOAT);
        SetShaderValueTexture(shaderPass1, GetShaderLocation(shaderPass1, "texture0"), floatTex);
        // 用默认的 colDiffuse = (1,1,1,1) 和 fragColor = (1,1,1,1)
        DrawTexture(floatTex, 0, 0, WHITE);
        EndShaderMode();

        EndTextureMode();

        // ============ Pass 2: 后处理 → 输出到屏幕 ============
        BeginDrawing();
        ClearBackground((Color){ 30, 30, 30, 255 });

        // 用后处理 shader 绘制 FBO 的颜色纹理
        BeginShaderMode(shaderPass2);
        Vector2 res = { (float)fbo.texture.width, (float)fbo.texture.height };
        SetShaderValue(shaderPass2, locTime2,  &time,          SHADER_UNIFORM_FLOAT);
        SetShaderValue(shaderPass2, locRes,    &res,           SHADER_UNIFORM_VEC2);
        SetShaderValue(shaderPass2, locEffect, &currentEffect, SHADER_UNIFORM_INT);
        SetShaderValueTexture(shaderPass2, GetShaderLocation(shaderPass2, "texture0"),
                              fbo.texture);

        // 拉伸到全屏显示
        DrawTexturePro(fbo.texture,
                       (Rectangle){ 0, 0, (float)texW, (float)-texH },  // 源：flip Y
                       (Rectangle){ 0, 0, (float)screenWidth, (float)screenHeight },
                       (Vector2){ 0, 0 }, 0.0f, WHITE);
        EndShaderMode();

        // ---- HUD ----
        const char *effectNames[] = {
            "0: Pixelation (Mosaic)",
            "1: Scanlines + Glitch",
            "2: Chromatic Aberration + Vignette",
            "3: Edge Glow (Sobel)",
            "4: Polar Twirl",
            "5: CRT Simulator"
        };
        DrawText(TextFormat("Pass 1: R32G32B32A32F + Animation  -->  Pass 2: %s",
                 effectNames[currentEffect]), 10, 10, 18, GREEN);
        DrawText(TextFormat("Time: %.2f  [Space] pause  [R] reset", time),
                 10, 32, 18, GREEN);
        DrawText("[<-  ->] switch effect", 10, 54, 18, GREEN);
        DrawText(TextFormat("FPS: %d", GetFPS()), screenWidth - 100, 10, 18, YELLOW);

        EndDrawing();
    }

    // 清理
    UnloadShader(shaderPass1);
    UnloadShader(shaderPass2);
    UnloadRenderTexture(fbo);
    rlUnloadTexture(texId);

    CloseWindow();
    return 0;
}
