#include "raylib.h"
#include <stdio.h>
#include <stdlib.h>

// ============================================================================
// GLSL Shaders 
// 注意：将主要纹理全部重命名为 texture0 以适配 Raylib 的默认绑定
// ============================================================================

const char* fs_splat = 
"#version 330\n"
"in vec2 fragTexCoord;\n"
"out vec4 finalColor;\n"
"uniform sampler2D texture0;\n" // 原 targetTex
"uniform vec2 point;\n"
"uniform vec3 color;\n"
"uniform float radius;\n"
"uniform int isVelocity;\n"   // 用 int 代替 bool 更安全
"const float offset = 128.0 / 255.0;\n"
"void main() {\n"
"    vec4 base = texture(texture0, fragTexCoord);\n"
"    float d = distance(fragTexCoord, point);\n"
"    if (d < radius) {\n"
"        float falloff = exp(-d * d / (radius * radius * 0.5));\n"
"        if (isVelocity == 1) {\n"
"            vec2 baseVel = (base.xy - offset) * 2.0;\n"
"            vec2 addVel = color.xy * falloff;\n"
"            vec2 newVel = baseVel + addVel;\n"
"            finalColor = vec4(newVel * 0.5 + offset, 0.0, 1.0);\n"
"        } else {\n"
"            finalColor = base + vec4(color * falloff, 1.0);\n"
"        }\n"
"    } else {\n"
"        finalColor = base;\n"
"    }\n"
"}\n";

const char* fs_advect = 
"#version 330\n"
"in vec2 fragTexCoord;\n"
"out vec4 finalColor;\n"
"uniform sampler2D texture0;\n"  // 原 sourceTex
"uniform sampler2D velocityTex;\n" // 额外绑定的纹理
"uniform float dt;\n"
"uniform float dissipation;\n"
"uniform int isVelocity;\n"
"const float offset = 128.0 / 255.0;\n"
"void main() {\n"
"    vec2 vel = (texture(velocityTex, fragTexCoord).xy - offset) * 2.0;\n"
"    vec2 srcCoord = fragTexCoord - vel * dt;\n"
"    vec4 advected = texture(texture0, srcCoord);\n"
"    if (isVelocity == 1) {\n"
"        vec2 decodedAdv = (advected.xy - offset) * 2.0 * dissipation;\n"
"        finalColor = vec4(decodedAdv * 0.5 + offset, 0.0, 1.0);\n"
"    } else {\n"
"        finalColor = advected * dissipation;\n"
"    }\n"
"}\n";

const char* fs_divergence = 
"#version 330\n"
"in vec2 fragTexCoord;\n"
"out vec4 finalColor;\n"
"uniform sampler2D texture0;\n" // 原 velocityTex
"uniform vec2 texelSize;\n"
"const float offset = 128.0 / 255.0;\n"
"void main() {\n"
"    vec2 L = (texture(texture0, fragTexCoord - vec2(texelSize.x, 0)).xy - offset) * 2.0;\n"
"    vec2 R = (texture(texture0, fragTexCoord + vec2(texelSize.x, 0)).xy - offset) * 2.0;\n"
"    vec2 B = (texture(texture0, fragTexCoord - vec2(0, texelSize.y)).xy - offset) * 2.0;\n"
"    vec2 T = (texture(texture0, fragTexCoord + vec2(0, texelSize.y)).xy - offset) * 2.0;\n"
"    \n"
"    float div = 0.5 * ((R.x - L.x) + (T.y - B.y));\n"
"    finalColor = vec4(div * 0.5 + offset, 0.0, 0.0, 1.0);\n"
"}\n";

const char* fs_jacobi = 
"#version 330\n"
"in vec2 fragTexCoord;\n"
"out vec4 finalColor;\n"
"uniform sampler2D texture0;\n" // 原 pressureTex
"uniform sampler2D divergenceTex;\n"
"uniform vec2 texelSize;\n"
"const float offset = 128.0 / 255.0;\n"
"void main() {\n"
"    float L = (texture(texture0, fragTexCoord - vec2(texelSize.x, 0)).x - offset) * 2.0;\n"
"    float R = (texture(texture0, fragTexCoord + vec2(texelSize.x, 0)).x - offset) * 2.0;\n"
"    float B = (texture(texture0, fragTexCoord - vec2(0, texelSize.y)).x - offset) * 2.0;\n"
"    float T = (texture(texture0, fragTexCoord + vec2(0, texelSize.y)).x - offset) * 2.0;\n"
"    \n"
"    float div = (texture(divergenceTex, fragTexCoord).x - offset) * 2.0;\n"
"    float p = (L + R + B + T - div) * 0.25;\n"
"    \n"
"    finalColor = vec4(p * 0.5 + offset, 0.0, 0.0, 1.0);\n"
"}\n";

const char* fs_subtract = 
"#version 330\n"
"in vec2 fragTexCoord;\n"
"out vec4 finalColor;\n"
"uniform sampler2D texture0;\n" // 原 velocityTex
"uniform sampler2D pressureTex;\n"
"uniform vec2 texelSize;\n"
"const float offset = 128.0 / 255.0;\n"
"void main() {\n"
"    float L = (texture(pressureTex, fragTexCoord - vec2(texelSize.x, 0)).x - offset) * 2.0;\n"
"    float R = (texture(pressureTex, fragTexCoord + vec2(texelSize.x, 0)).x - offset) * 2.0;\n"
"    float B = (texture(pressureTex, fragTexCoord - vec2(0, texelSize.y)).x - offset) * 2.0;\n"
"    float T = (texture(pressureTex, fragTexCoord + vec2(0, texelSize.y)).x - offset) * 2.0;\n"
"    \n"
"    vec2 vel = (texture(texture0, fragTexCoord).xy - offset) * 2.0;\n"
"    vel.xy -= vec2(R - L, T - B) * 0.5;\n"
"    \n"
"    finalColor = vec4(vel * 0.5 + offset, 0.0, 1.0);\n"
"}\n";

typedef struct {
    RenderTexture2D read;
    RenderTexture2D write;
} DoubleFBO;

DoubleFBO InitDoubleFBO(int width, int height) {
    DoubleFBO fbo;
    fbo.read = LoadRenderTexture(width, height);
    fbo.write = LoadRenderTexture(width, height);
    SetTextureFilter(fbo.read.texture, TEXTURE_FILTER_BILINEAR);
    SetTextureFilter(fbo.write.texture, TEXTURE_FILTER_BILINEAR);
    SetTextureWrap(fbo.read.texture, TEXTURE_WRAP_CLAMP);
    SetTextureWrap(fbo.write.texture, TEXTURE_WRAP_CLAMP);
    return fbo;
}

void SwapFBO(DoubleFBO* fbo) {
    RenderTexture2D temp = fbo->read;
    fbo->read = fbo->write;
    fbo->write = temp;
}

void ClearFBOToOffset(DoubleFBO* fbo) {
    Color offsetColor = (Color){128, 128, 128, 255}; // 更精准的 0.5 零向量偏移
    BeginTextureMode(fbo->read);  ClearBackground(offsetColor); EndTextureMode();
    BeginTextureMode(fbo->write); ClearBackground(offsetColor); EndTextureMode();
}

int main() {
    const int screenWidth = 800;
    const int screenHeight = 800;
    const int simWidth = 256; 
    const int simHeight = 256;

    SetConfigFlags(FLAG_MSAA_4X_HINT);
    InitWindow(screenWidth, screenHeight, "Raylib RGBA8 Navier-Stokes Fluid");
    SetTargetFPS(60);

    // 关键修正：第一个参数传 0，使用 Raylib 自带的 Vertex Shader 确保投影正确
    Shader splatShader   = LoadShaderFromMemory(0, fs_splat);
    Shader advectShader  = LoadShaderFromMemory(0, fs_advect);
    Shader divShader     = LoadShaderFromMemory(0, fs_divergence);
    Shader jacobiShader  = LoadShaderFromMemory(0, fs_jacobi);
    Shader subShader     = LoadShaderFromMemory(0, fs_subtract);

    int splatPointLoc   = GetShaderLocation(splatShader, "point");
    int splatColorLoc   = GetShaderLocation(splatShader, "color");
    int splatRadiusLoc  = GetShaderLocation(splatShader, "radius");
    int splatIsVelLoc   = GetShaderLocation(splatShader, "isVelocity");

    int advVelLoc       = GetShaderLocation(advectShader, "velocityTex");
    int advDtLoc        = GetShaderLocation(advectShader, "dt");
    int advDissLoc      = GetShaderLocation(advectShader, "dissipation");
    int advIsVelLoc     = GetShaderLocation(advectShader, "isVelocity");

    int divTexelSizeLoc = GetShaderLocation(divShader, "texelSize");

    int jacDivLoc       = GetShaderLocation(jacobiShader, "divergenceTex");
    int jacTexelSizeLoc = GetShaderLocation(jacobiShader, "texelSize");

    int subPresLoc      = GetShaderLocation(subShader, "pressureTex");
    int subTexelSizeLoc = GetShaderLocation(subShader, "texelSize");

    DoubleFBO velocity = InitDoubleFBO(simWidth, simHeight);
    DoubleFBO dye      = InitDoubleFBO(simWidth, simHeight);
    DoubleFBO pressure = InitDoubleFBO(simWidth, simHeight);
    RenderTexture2D divergence = LoadRenderTexture(simWidth, simHeight);
    SetTextureFilter(divergence.texture, TEXTURE_FILTER_BILINEAR);

    ClearFBOToOffset(&velocity);
    ClearFBOToOffset(&pressure);
    
    BeginTextureMode(dye.read); ClearBackground(BLACK); EndTextureMode();
    BeginTextureMode(dye.write); ClearBackground(BLACK); EndTextureMode();

    float texelSize[2] = { 1.0f / simWidth, 1.0f / simHeight };
    Vector2 lastMousePos = GetMousePosition();

    // 所有FBO操作统一使用负高度，鼠标坐标翻转一次匹配
    Rectangle simRect = { 0, 0, (float)simWidth, -(float)simHeight };

    while (!WindowShouldClose()) {
        float dt = 0.016f;
        Vector2 mousePos = GetMousePosition();
        Vector2 mouseDelta = { mousePos.x - lastMousePos.x, mousePos.y - lastMousePos.y };
        lastMousePos = mousePos;

        // 1. Splat 注入
        if (IsMouseButtonDown(MOUSE_BUTTON_LEFT) || GetMouseWheelMove() != 0) {
            // FBO 统一负高度 → 需要鼠标 Y 翻转一次
            float mX = mousePos.x / screenWidth;
            float mY = 1.0f - mousePos.y / screenHeight; 
            float pt[2] = {mX, mY};
            float radius = 0.06f;   // 原来的 4倍
            
            float forceX = (mouseDelta.x / screenWidth) * 20.0f;
            float forceY = -(mouseDelta.y / screenHeight) * 20.0f; 
            float velColor[3] = {forceX, forceY, 0.0f};
            float rgbColor[3] = {0.2f, 0.8f, 1.0f}; // 青蓝色

            BeginTextureMode(velocity.write);
                BeginShaderMode(splatShader);
                    int isVel = 1;
                    SetShaderValue(splatShader, splatPointLoc, pt, SHADER_UNIFORM_VEC2);
                    SetShaderValue(splatShader, splatRadiusLoc, &radius, SHADER_UNIFORM_FLOAT);
                    SetShaderValue(splatShader, splatColorLoc, velColor, SHADER_UNIFORM_VEC3);
                    SetShaderValue(splatShader, splatIsVelLoc, &isVel, SHADER_UNIFORM_INT);
                    DrawTextureRec(velocity.read.texture, simRect, (Vector2){ 0, 0 }, WHITE);
                EndShaderMode();
            EndTextureMode();
            SwapFBO(&velocity);

            BeginTextureMode(dye.write);
                BeginShaderMode(splatShader);
                    isVel = 0;
                    SetShaderValue(splatShader, splatColorLoc, rgbColor, SHADER_UNIFORM_VEC3);
                    SetShaderValue(splatShader, splatIsVelLoc, &isVel, SHADER_UNIFORM_INT);
                    DrawTextureRec(dye.read.texture, simRect, (Vector2){ 0, 0 }, WHITE);
                EndShaderMode();
            EndTextureMode();
            SwapFBO(&dye);
        }

        // 2. Advection
        BeginTextureMode(velocity.write);
            BeginShaderMode(advectShader);
                int isVel = 1;
                float dissVel = 0.98f;  // 更强衰减，跨过 RGBA8 量化台阶
                SetShaderValue(advectShader, advDtLoc, &dt, SHADER_UNIFORM_FLOAT);
                SetShaderValue(advectShader, advDissLoc, &dissVel, SHADER_UNIFORM_FLOAT);
                SetShaderValue(advectShader, advIsVelLoc, &isVel, SHADER_UNIFORM_INT);
                SetShaderValueTexture(advectShader, advVelLoc, velocity.read.texture);
                DrawTextureRec(velocity.read.texture, simRect, (Vector2){ 0, 0 }, WHITE);
            EndShaderMode();
        EndTextureMode();
        SwapFBO(&velocity);

        BeginTextureMode(dye.write);
            BeginShaderMode(advectShader);
                isVel = 0;
                float dissDye = 0.99f; 
                SetShaderValue(advectShader, advDissLoc, &dissDye, SHADER_UNIFORM_FLOAT);
                SetShaderValue(advectShader, advIsVelLoc, &isVel, SHADER_UNIFORM_INT);
                SetShaderValueTexture(advectShader, advVelLoc, velocity.read.texture);
                DrawTextureRec(dye.read.texture, simRect, (Vector2){ 0, 0 }, WHITE);
            EndShaderMode();
        EndTextureMode();
        SwapFBO(&dye);

        // 3. Divergence
        BeginTextureMode(divergence);
            BeginShaderMode(divShader);
                SetShaderValue(divShader, divTexelSizeLoc, texelSize, SHADER_UNIFORM_VEC2);
                DrawTextureRec(velocity.read.texture, simRect, (Vector2){ 0, 0 }, WHITE);
            EndShaderMode();
        EndTextureMode();

        Color offsetColor = (Color){128, 128, 128, 255};
        BeginTextureMode(pressure.read); ClearBackground(offsetColor); EndTextureMode();

        // 4. Jacobi Iteration
        for (int i = 0; i < 30; i++) {
            BeginTextureMode(pressure.write);
                BeginShaderMode(jacobiShader);
                    SetShaderValue(jacobiShader, jacTexelSizeLoc, texelSize, SHADER_UNIFORM_VEC2);
                    SetShaderValueTexture(jacobiShader, jacDivLoc, divergence.texture);
                    DrawTextureRec(pressure.read.texture, simRect, (Vector2){ 0, 0 }, WHITE);
                EndShaderMode();
            EndTextureMode();
            SwapFBO(&pressure);
        }

        // 5. Gradient Subtraction
        BeginTextureMode(velocity.write);
            BeginShaderMode(subShader);
                SetShaderValue(subShader, subTexelSizeLoc, texelSize, SHADER_UNIFORM_VEC2);
                SetShaderValueTexture(subShader, subPresLoc, pressure.read.texture);
                DrawTextureRec(velocity.read.texture, simRect, (Vector2){ 0, 0 }, WHITE);
            EndShaderMode();
        EndTextureMode();
        SwapFBO(&velocity);

        // 6. Draw to Screen
        BeginDrawing();
            ClearBackground(BLACK);
            // 只有最后这步，为了适配屏幕呈现视角，我们需要翻转一次高 `-simHeight`
            DrawTexturePro(dye.read.texture, 
                           (Rectangle){ 0, 0, (float)simWidth, -(float)simHeight }, 
                           (Rectangle){ 0, 0, (float)screenWidth, (float)screenHeight }, 
                           (Vector2){ 0, 0 }, 0.0f, WHITE);
            
            DrawText("Drag Mouse to move fluid!", 10, 10, 20, RAYWHITE);
            DrawFPS(10, 30);
        EndDrawing();
    }

    // Unload... (省略清理代码)
    CloseWindow();
    return 0;
}