#pragma once

// TrueTriple: OPCIONES DE RENDIMIENTO (nacieron en la version de prueba DR2
// 1.9.18.1 del 25/11/2026, rama fuente/dr2-amd-casillas; pasaron a la version
// normal el 26/11/2026). Con una RX 9070 XT y el triple, la CPU quedaba como
// limite: de 69 a 106 FPS con las tres prendidas (97 solo con LIVIANO).
//
// Cada opcion es una casilla de la seccion Rendimiento de la app, todas
// apagadas por defecto. Se guardan en el perfil (perfil.ini: REND_LIVIANO,
// REND_RASTER, REND_REDIBUJO) y el plugin las agrega en cada Jugar como una
// linea de comentario al final del cartel de F3, TrueTriple.menu.txt, al lado
// del d3dx.ini (el contrato del cartel ignora las lineas que empiezan con
// '#'; un dll anterior no la ve):
//
//   #tt-rendimiento: liviano raster redibujo
//
// El dll la lee al cargar el ini (LoadConfigFile, solo con la marca de sesion
// valida; tambien con Ctrl+Alt+Shift+F10) y la deja en g_bits. Sin la linea,
// o sin sesion, todo apagado: el dll hace exactamente lo de siempre.
//
//   LIVIANO   Map/Unmap sin los diagnosticos viejos (censos con spinlock
//             global, volcados, 3 o 4 GetDesc por Map), las variables del ini
//             que se leian con el candado global de 3Dmigoto en cada constant
//             buffer leidas una vez por cuadro, y el prologo de la captura de
//             camara (replay, sonda, ganchos, reporte) una vez por cuadro.
//   RASTER    El redibujo de cada objeto (CustomShaderTripleRedraw, con
//             rasterizer_state_merge) ya no crea un estado del rasterizador
//             con ID3D11Device::CreateRasterizerState en CADA objeto (unas
//             900 veces por cuadro, desde los 7 hilos, con el candado del
//             dispositivo): se crea una vez por combinacion y se reusa.
//   REDIBUJO  El redibujo de cada objeto deja de guardar y reponer los render
//             targets, UAVs y viewports (no los cambia), no bindea la textura
//             de parametros al domain shader si no hay ninguno, y mide el
//             render target una vez por objeto y no una por tercio. Son
//             llamadas que, en una placa sin listas de comandos del driver
//             (AMD en DX11), el runtime repite en el hilo principal.

#include <windows.h>

namespace ttrend {

enum Opcion {
	LIVIANO  = 1,
	RASTER   = 2,
	REDIBUJO = 4,
};

extern volatile LONG g_bits;   // IniHandler.cpp
// Sube en cada carga del ini (tambien Ctrl+Alt+Shift+F10): los custom shaders
// se vuelven a crear y un puntero viejo puede reaparecer en otra seccion, asi
// que los caches por hilo que usan ese puntero se vacian al verlo cambiar.
extern volatile LONG g_generacion;

inline bool Activa(Opcion o)
{
	return (g_bits & (LONG)o) != 0;
}

// IniHandler.cpp: lee la linea "#tt-rendimiento:" de TrueTriple.menu.txt (al lado
// del dll) y la deja en g_bits. Lo llama LoadConfigFile con la sesion abierta.
void Leer();
// "liviano raster" o "ninguna", para el log.
const char *Texto();

} // namespace ttrend
