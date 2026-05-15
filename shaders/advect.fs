#version 330
// Advection: backward particle trace through velocity field
//   isVel=1 → self-advect velocity (texture0=vel)
//   isVel=0 → advect dye             (texture0=dye, texture1=vel)
in vec2 fragTexCoord;
out vec4 fragColor;
uniform sampler2D texture0;
uniform sampler2D texture1;
uniform vec2  fluidSize;
uniform float velScale;  // = 2*VEL_SCALE, decodes [0,1]→[-VEL,+VEL] pixels/frame
uniform float isVel;

void main() {
    vec2 rawVel;
    if (isVel > 0.5) {
        rawVel = texture(texture0, fragTexCoord).xy;
    } else {
        rawVel = texture(texture1, fragTexCoord).xy;
    }
    vec2 vel = (rawVel - 0.5) * velScale;
    vec2 pos = fragTexCoord * fluidSize - vel;

    // Boundary check (half-pixel margin)
    if (pos.x < 0.5 || pos.x > fluidSize.x - 0.5 ||
        pos.y < 0.5 || pos.y > fluidSize.y - 0.5) {
        fragColor = vec4(0.0);
    } else {
        vec2 uv = pos / fluidSize;
        fragColor = texture(texture0, uv);
    }
}
