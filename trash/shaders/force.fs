#version 330
// Inject external force into velocity field.
// Uses vertex color (tint) to encode force direction.
//   tint.rg in [0,1] -> direction in [-1,1]
// Output is encoded as 0.5 + force/velScale
in vec2 fragTexCoord;
in vec4 fragColor;
out vec4 fragColorOut;
uniform float maxFluidForce;
uniform float velScale;

void main() {
    // Map vertex color [0,1] -> [-1,1]
    vec2 dir = fragColor.rg * 2.0 - 1.0;
    // HLSL original: o0.xy = 20 * maxFluidForce * dir
    vec2 force = 20.0 * maxFluidForce * dir;
    // Encode into [0,1] storage
    vec2 encoded = 0.5 + force / velScale;
    fragColorOut = vec4(encoded, 0.0, 1.0);
}
