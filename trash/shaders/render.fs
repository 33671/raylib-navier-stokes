#version 330
// Final composite: dye + velocity-based glow
in vec2 fragTexCoord;
out vec4 fragColor;
uniform sampler2D tex0; // velocity
uniform sampler2D tex1; // dye
uniform float velScale;

void main() {
    vec4 dye = texture(tex1, fragTexCoord);
    vec2 vel = (texture(tex0, fragTexCoord).xy - 0.5) * velScale;
    float speed = length(vel) * 0.005;
    // Mix dye with a subtle blue velocity glow
    vec3 col = dye.rgb + vec3(speed * 0.2, speed * 0.5, speed * 0.9);
    fragColor = vec4(col, 1.0);
}
