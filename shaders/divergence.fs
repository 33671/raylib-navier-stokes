#version 330
// Divergence: ∇·v using central differences (half-pixel offsets)
in vec2 fragTexCoord;
out vec4 fragColor;
uniform sampler2D texture0;  // velocity
uniform vec2 fluidSize;

void main() {
    vec2 h = vec2(0.5) / fluidSize;
    float vr = texture(texture0, fragTexCoord + vec2( h.x, 0.0)).x;
    float vl = texture(texture0, fragTexCoord + vec2(-h.x, 0.0)).x;
    float vt = texture(texture0, fragTexCoord + vec2(0.0,  h.y)).y;
    float vb = texture(texture0, fragTexCoord + vec2(0.0, -h.y)).y;
    fragColor = vec4(0.5 * (vr - vl + vt - vb), 0.0, 0.0, 0.0);
}
