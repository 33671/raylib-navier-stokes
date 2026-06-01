/******************************************************************************
 *  float_tex_demo.c
 *  用 R32G32B32A32F 纹理格式上传并显示彩色格子图像
 *
 *  编译:
 *    gcc -Wall -Wextra -O2 -I./raylib/src float_tex_demo.c \
 *        ./raylib/src/libraylib.a -lGL -lm -lpthread -ldl -lrt -lX11 -o float_tex_demo
 *
 *  运行:
 *    ./float_tex_demo
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

    InitWindow(screenWidth, screenHeight, "R32G32B32A32F - Colorful Grid Demo");

    // ---------------------------------------------------------------
    // 1. 生成彩色格子数据 (R32G32B32A32F)
    // ---------------------------------------------------------------
    const int texW = 512;
    const int texH = 512;
    const int cellSize = 64;  // 每个格子 64x64 像素

    // 每个像素 4 个 float = 16 bytes
    float *pixels = (float *)RL_MALLOC(texW * texH * 4 * sizeof(float));

    for (int y = 0; y < texH; y++)
    {
        for (int x = 0; x < texW; x++)
        {
            int idx = (y * texW + x) * 4;
            int cx = x / cellSize;  // 格子列索引
            int cy = y / cellSize;  // 格子行索引

            // 棋盘格模式: (cx + cy) % 2 == 0 为一组颜色，否则为另一组
            if ((cx + cy) % 2 == 0)
            {
                // 偶数格: 红色系，根据位置渐变
                float r = 0.2f + 0.8f * ((float)x / texW);
                float g = 0.1f + 0.3f * ((float)y / texH);
                float b = 0.1f;
                float a = 0.8f + 0.2f * ((float)(x + y) / (texW + texH)); // 半透明~全透
                pixels[idx + 0] = r;
                pixels[idx + 1] = g;
                pixels[idx + 2] = b;
                pixels[idx + 3] = a;
            }
            else
            {
                // 奇数格: 蓝色系
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

    // ---------------------------------------------------------------
    // 2. 上传纹理 (R32G32B32A32F)
    // ---------------------------------------------------------------
    unsigned int texId = rlLoadTexture(pixels, texW, texH,
                                       PIXELFORMAT_UNCOMPRESSED_R32G32B32A32,
                                       1);
    RL_FREE(pixels);

    if (texId == 0)
    {
        TraceLog(LOG_ERROR, "Failed to load R32G32B32A32F texture!");
        CloseWindow();
        return 1;
    }

    TraceLog(LOG_INFO, "Float texture loaded OK, id=%u", texId);

    // 封装为 Texture2D 结构，方便 raylib 使用
    Texture2D floatTex = {
        .id      = texId,
        .width   = texW,
        .height  = texH,
        .mipmaps = 1,
        .format  = PIXELFORMAT_UNCOMPRESSED_R32G32B32A32
    };

    // ---------------------------------------------------------------
    // 3. 写一个让图像动起来的 fragment shader
    // ---------------------------------------------------------------
    // 效果组合：
    //   - 波浪扭曲 UV（像水面波动）
    //   - 颜色随时间偏移
    //   - 按 S 切换 彩色/Alpha 显示
    //   - 按 Space 切换动画暂停/继续
    // ---------------------------------------------------------------
    const char *fsCode =
        "#version 330\n"
        "in vec2 fragTexCoord;\n"
        "in vec4 fragColor;\n"
        "out vec4 finalColor;\n"
        "uniform sampler2D texture0;\n"
        "uniform vec4 colDiffuse;\n"
        "uniform float uTime;\n"
        "uniform int showAlpha;\n"
        "void main()\n"
        "{\n"
        "    float t = uTime;\n"
        "\n"
        "    // --- 效果1: 波浪扭曲 UV ---\n"
        "    vec2 uv = fragTexCoord;\n"
        "    uv.x += 0.03 * sin(uv.y * 20.0 + t * 3.0);\n"
        "    uv.y += 0.03 * cos(uv.x * 20.0 + t * 2.5);\n"
        "\n"
        "    // --- 效果2: 缩放脉冲 ---\n"
        "    float pulse = 1.0 + 0.08 * sin(t * 1.5);\n"
        "    uv = (uv - 0.5) * pulse + 0.5;\n"
        "\n"
        "    // --- 效果3: 旋转（随时间缓慢旋转） ---\n"
        "    float angle = t * 0.2;\n"
        "    float sA = sin(angle);\n"
        "    float cA = cos(angle);\n"
        "    vec2 ruv = uv - 0.5;\n"
        "    uv = vec2(ruv.x * cA - ruv.y * sA, ruv.x * sA + ruv.y * cA) + 0.5;\n"
        "\n"
        "    // 采样纹理\n"
        "    vec4 texColor = texture(texture0, uv);\n"
        "\n"
        "    // --- 效果4: 颜色偏移（RGB 分裂） ---\n"
        "    float shift = 0.02 * sin(t * 2.0);\n"
        "    float r = texture(texture0, uv + vec2( shift, 0.0)).r;\n"
        "    float g = texture(texture0, uv + vec2( 0.0, 0.0)).g;\n"
        "    float b = texture(texture0, uv + vec2(-shift, 0.0)).b;\n"
        "    float a = texColor.a;\n"
        "    vec4 c = vec4(r, g, b, a);\n"
        "\n"
        "    // --- 效果5: 边缘光晕脉冲 ---\n"
        "    float dist = length(fragTexCoord - 0.5);\n"
        "    float glow = 0.5 + 0.5 * sin(dist * 8.0 - t * 4.0);\n"
        "    c.rgb += glow * 0.1;\n"
        "\n"
        "    if (showAlpha == 1)\n"
        "        finalColor = vec4(c.aaa, 1.0);\n"
        "    else\n"
        "        finalColor = c * colDiffuse * fragColor;\n"
        "}\n";

    Shader shader = LoadShaderFromMemory(NULL, fsCode);
    int locTime      = GetShaderLocation(shader, "uTime");
    int locShowAlpha = GetShaderLocation(shader, "showAlpha");

    int showAlpha = 0;
    int pauseAnim = 0;
    float time = 0.0f;

    // 相机：让纹理填满窗口
    Camera2D camera = { 0 };
    camera.zoom = (float)screenWidth / texW;

    SetTargetFPS(60);

    // ---------------------------------------------------------------
    // 4. 主循环
    // ---------------------------------------------------------------
    while (!WindowShouldClose())
    {
        // 更新动画时间
        if (!pauseAnim)
            time += GetFrameTime();

        // 按 S 切换显示模式
        if (IsKeyPressed(KEY_S))
        {
            showAlpha = !showAlpha;
            TraceLog(LOG_INFO, "showAlpha = %d", showAlpha);
        }

        // 按 Space 暂停/继续动画
        if (IsKeyPressed(KEY_SPACE))
        {
            pauseAnim = !pauseAnim;
            TraceLog(LOG_INFO, "pauseAnim = %d", pauseAnim);
        }

        // 按 R 重置
        if (IsKeyPressed(KEY_R))
        {
            camera.zoom = (float)screenWidth / texW;
            camera.target = (Vector2){ 0, 0 };
            camera.offset = (Vector2){ 0, 0 };
            time = 0.0f;
            pauseAnim = 0;
        }

        // 滚轮缩放
        float wheel = GetMouseWheelMove();
        if (wheel != 0)
        {
            camera.zoom += wheel * 0.1f;
            if (camera.zoom < 0.1f) camera.zoom = 0.1f;
        }

        // 鼠标拖拽平移
        if (IsMouseButtonDown(MOUSE_BUTTON_LEFT))
        {
            Vector2 delta = GetMouseDelta();
            camera.target.x -= delta.x / camera.zoom;
            camera.target.y -= delta.y / camera.zoom;
        }

        BeginDrawing();
        ClearBackground((Color){ 30, 30, 30, 255 });

        BeginMode2D(camera);

        // 用自定义 shader 绘制纹理
        BeginShaderMode(shader);
        SetShaderValue(shader, locShowAlpha, &showAlpha, SHADER_UNIFORM_INT);
        SetShaderValue(shader, locTime,      &time,      SHADER_UNIFORM_FLOAT);
        SetShaderValueTexture(shader, GetShaderLocation(shader, "texture0"), floatTex);

        DrawTexture(floatTex, 0, 0, WHITE);

        EndShaderMode();

        EndMode2D();

        // HUD 文字
        DrawText(TextFormat("Format: R32G32B32A32F | Size: %dx%d", texW, texH),
                 10, 10, 20, GREEN);
        DrawText(TextFormat("ShowAlpha: %s  [S]  [Space] pause",
                 showAlpha ? "YES" : "NO"),
                 10, 35, 20, GREEN);
        DrawText(TextFormat("Time: %.2f  [R] reset  [Mouse] pan/zoom", time),
                 10, 60, 20, GREEN);

        // 右下角显示当前 FPS
        DrawText(TextFormat("FPS: %d", GetFPS()),
                 screenWidth - 120, screenHeight - 30, 20, YELLOW);

        EndDrawing();
    }

    UnloadShader(shader);
    // 注意：rlUnloadTexture 释放 GPU 纹理
    rlUnloadTexture(texId);

    CloseWindow();
    return 0;
}
