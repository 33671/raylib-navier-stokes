#version 330
// Divergence: ∇·v using central differences (half-pixel offsets)
in vec2 fragTexCoord;
out vec4 fragColor;
uniform sampler2D tex0;  // velocity
uniform vec2 fluidSize;

void main() {
    vec2 h = vec2(0.5) / fluidSize;
    float vr = texture(tex0, fragTexCoord + vec2( h.x, 0.0)).x;
    float vl = texture(tex0, fragTexCoord + vec2(-h.x, 0.0)).x;
    float vt = texture(tex0, fragTexCoord + vec2(0.0,  h.y)).y;
    float vb = texture(tex0, fragTexCoord + vec2(0.0, -h.y)).y;
    // Velocity is encoded as v/160 + 0.5.  vr-vl = (vx_right-vx_left)/160.
    // Actual divergence = (vr-vl + vt-vb) in encoded space = div_actual/160.
    // Jacobi works in encoded space, so we output without extra scaling.
    fragColor = vec4(vr - vl + vt - vb, 0.0, 0.0, 0.0);
}
