#version 330
// Subtract pressure gradient: v -= ∇p  (enforce incompressibility)
// texture0 = velocity, texture1 = pressure
in vec2 fragTexCoord;
out vec4 fragColor;
uniform sampler2D tex0;
uniform sampler2D tex1;
uniform vec2 fluidSize;

void main() {
    vec2 h = vec2(0.5) / fluidSize;
    float pR = texture(tex1, fragTexCoord + vec2( h.x, 0.0)).x;
    float pL = texture(tex1, fragTexCoord + vec2(-h.x, 0.0)).x;
    float pT = texture(tex1, fragTexCoord + vec2(0.0,  h.y)).x;
    float pB = texture(tex1, fragTexCoord + vec2(0.0, -h.y)).x;
    vec2 grad = vec2(pR - pL, pT - pB);
    vec2 vel  = texture(tex0, fragTexCoord).xy;
    fragColor = vec4(vel - grad, 0.0, 0.0);
}
