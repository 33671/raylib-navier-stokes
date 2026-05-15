#version 330
// Dye dissipation
in vec2 fragTexCoord;
out vec4 fragColor;
uniform sampler2D texture0;
uniform float dissipation;

void main() {
    vec4 c = texture(texture0, fragTexCoord);
    fragColor = vec4(c.rgb * dissipation, c.a);
}
