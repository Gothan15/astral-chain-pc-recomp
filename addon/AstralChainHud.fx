// AstralChainHud.fx — la técnica que compone el HUD de Astral Chain sobre la escena ya pasada por los
// efectos de ReShade. Es la compañera de la opción «reshade» del fork y de su addon
// (reshade_hud.addon64): el addon enlaza la capa del HUD a la textura de la semántica HUDLAYER y pone a
// 1 el uniforme HudLayerActive; sin addon, la técnica no pinta nada.
//
// Uso: deja este archivo junto a tus efectos, activa la técnica en tu preset y mantenla LA ÚLTIMA de la
// lista, para que ningún efecto posterior toque el HUD. No renombres el archivo ni la técnica: el addon
// los busca por este nombre.

uniform bool HudLayerActive;   // lo gobierna el addon; a 0 la técnica no pinta nada

// Una textura alimentada por un addon se declara SIN cuerpo: ReShade la crea y la rellena el addon en
// cada frame. Con cuerpo (Width/Height/Format) el compilador de efectos la rechaza (error X3000) y la
// técnica entera no compilaba: la separación del HUD nunca estuvo activa hasta este arreglo.
texture HudLayerTex : HUDLAYER;

sampler HudLayerSampler
{
	Texture = HudLayerTex;
	AddressU = CLAMP;
	AddressV = CLAMP;
	MinFilter = LINEAR;
	MagFilter = LINEAR;
	MipFilter = LINEAR;
};

// El triángulo de pantalla completa de siempre en ReShade: tres vértices generados por índice.
void HudVS(in uint id : SV_VertexID, out float4 pos : SV_Position, out float2 uv : TEXCOORD)
{
	uv = float2((id << 1) & 2, id & 2);
	pos = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}

// La capa trae el color ya mezclado por el propio juego dentro de la fase HUD, con el alfa como
// cobertura: componer es un «over» normal sobre la escena.
float4 HudPS(in float4 pos : SV_Position, in float2 uv : TEXCOORD) : SV_Target
{
	if (!HudLayerActive)
		discard;
	return tex2D(HudLayerSampler, uv);
}

technique AstralChainHudComposite < ui_label = "Astral Chain — HUD composite (dejar la última)";
	ui_tooltip = "Compone el HUD tras todos los efectos. Necesita la opción reshade del fork y su addon."; >
{
	pass
	{
		VertexShader = HudVS;
		PixelShader = HudPS;
		BlendEnable = true;
		SrcBlend = SRCALPHA;
		DestBlend = INVSRCALPHA;
		SrcBlendAlpha = ONE;
		DestBlendAlpha = INVSRCALPHA;
	}
}
