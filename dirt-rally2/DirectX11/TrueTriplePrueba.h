#pragma once

// TrueTriple: opciones de la VERSION DE PRUEBA de rendimiento en AMD (rama
// fuente/dr2-amd-casillas, 25/11/2026, tarea "DR2: el triple deja la CPU como
// limite en AMD").
//
// Cada opcion es una casilla de la app (Graficos > Pruebas de rendimiento). La
// app no puede cambiar el plugin de Rust en una version de prueba, asi que las
// casillas viajan por un archivo que el plugin YA escribe en cada Jugar: el
// cartel de F3, TrueTriple.menu.txt, al lado del d3dx.ini. La web le agrega
// una linea de comentario (el contrato del cartel ignora las que empiezan con
// '#'; un dll estable no la ve):
//
//   #tt-prueba: liviano raster redibujo
//
// El dll la lee al cargar el ini (LoadConfigFile, solo con la marca de sesion
// valida) y la deja en g_bits. Sin la linea, o sin sesion, todo apagado: el
// dll hace exactamente lo de la estable.
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

namespace ttprueba {

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

// IniHandler.cpp: lee la linea "#tt-prueba:" de TrueTriple.menu.txt (al lado
// del dll) y la deja en g_bits. Lo llama LoadConfigFile con la sesion abierta.
void Leer();
// "liviano raster" o "ninguna", para el log.
const char *Texto();

} // namespace ttprueba
