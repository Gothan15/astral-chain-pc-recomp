#version 450
// El último paso del preset ELZELDAPA (runtime/vk_preset.c): la imagen ya procesada por Colourfulness y
// ExposureFusion (RGBA16F) se dibuja tal cual sobre la imagen terminada del juego, con el triángulo de
// pantalla completa de hdr_fullscreen.vert y sin mezcla, para que el HUD que el juego dibuja después caiga
// sobre la escena tratada y no al revés. Mismo tamaño a los dos lados: el muestreo es texel a texel.
layout(binding = 0) uniform sampler2D src_tex;
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 frag;

void main() {
    frag = texture(src_tex, uv);
}
