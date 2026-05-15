#version 330
// Jacobi iteration: one step of ∇²p = divergence
// texture0 = pressure (prev), texture1 = divergence
in vec2 fragTexCoord;
out vec4 fragColor;
uniform sampler2D tex0;
uniform sampler2D tex1;
uniform vec2 fluidSize;

void main() {
    vec2 h = vec2(1.0) / fluidSize;
    float pL = texture(tex0, fragTexCoord + vec2(-h.x, 0.0)).x;
    float pR = texture(tex0, fragTexCoord + vec2( h.x, 0.0)).x;
    float pB = texture(tex0, fragTexCoord + vec2(0.0, -h.y)).x;
    float pT = texture(tex0, fragTexCoord + vec2(0.0,  h.y)).x;
    float div = texture(tex1, fragTexCoord).x;
    fragColor = vec4(0.25 * (pL + pR + pB + pT - div), 0.0, 0.0, 0.0);
}
