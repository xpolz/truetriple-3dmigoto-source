#pragma once

// TrueTriple: dll de DIAGNOSTICO de rendimiento (rama diag/dr2-amd-map-unmap,
// 24/11/2026, caso de una RX 9070 XT que queda con la GPU al 30-50% con el mod
// enganchado aunque F7 apague el triple).
//
// Nada de esto entra en un build normal: las dos macros valen 0 salvo que el
// build las pase por la variable de entorno CL (ver tools/build-diag-amd.ps1):
//
//   TT_DIAG_INSTRUMENTAR=1  cuenta por cuadro Map/Unmap, draws, listas de
//                           comandos y el tiempo de Present, y cada 10 s escribe
//                           una linea en TrueTriple_diag.txt al lado del dll.
//   TT_DIAG_VACIO=1         variante A: 3Dmigoto "vacio". No carga ningun ini
//                           de la carpeta (ni el armado ni el inerte), no carga
//                           ShaderFixes y saltea nuestros agregados en los
//                           caminos calientes del contexto. Es el intermediario
//                           solo, para ver si ya eso frena.
//
// Sin VACIO, el build con INSTRUMENTAR es la variante B: el dll normal con el
// arreglo candidato (variables del ini leidas una vez por cuadro y sin los
// diagnosticos viejos en Map/Unmap) mas los contadores.
//
// Los contadores son por hilo (cada contexto diferido del juego es un hilo):
// sin candados ni atomicos en el camino caliente. El hilo de Present los suma
// cada 10 s; una lectura corrida en un contador no importa para un promedio.

#ifndef TT_DIAG_INSTRUMENTAR
#define TT_DIAG_INSTRUMENTAR 0
#endif
#ifndef TT_DIAG_VACIO
#define TT_DIAG_VACIO 0
#endif

#include <windows.h>
#include <intrin.h>
#include <d3d11.h>

namespace ttdiag {

enum Contador {
	MAPS = 0,          // Map de cualquier tipo
	MAPS_ESCRITURA,    // WRITE / WRITE_DISCARD / WRITE_NO_OVERWRITE
	MAPS_LECTURA,      // READ / READ_WRITE: la CPU espera a la GPU si el dato no esta
	MAPS_CAMARA,       // constant buffer de 256 a 320 bytes mapeado para escribir
	TSC_MAP_DRIVER,    // ciclos adentro del Map del driver (todos)
	TSC_MAP_LECTURA,   // ciclos adentro del Map del driver, solo lecturas
	TSC_UNMAP_DRIVER,  // ciclos adentro del Unmap del driver
	TSC_EXTRAS,        // ciclos de NUESTRO codigo en Map/Unmap (no el del driver)
	DRAWS,             // Draw, DrawIndexed, DrawInstanced, DrawIndexedInstanced
	EXECUTES,          // ExecuteCommandList (contexto inmediato)
	TSC_EXECUTE,
	FINISHES,          // FinishCommandList (contextos diferidos)
	TSC_FINISH,
	INI_CANDADO,       // lecturas del ini que tomaron el candado global
	N
};

struct alignas(64) Hilo {
	volatile LONG64 c[N];
};

Hilo *HiloActual();   // HackerContext.cpp

inline void Sumar(Contador k, LONG64 v)
{
#if TT_DIAG_INSTRUMENTAR
	Hilo *h = HiloActual();
	h->c[k] = h->c[k] + v;
#else
	(void)k; (void)v;
#endif
}

inline unsigned long long Tsc()
{
#if TT_DIAG_INSTRUMENTAR
	return __rdtsc();
#else
	return 0;
#endif
}

// Llamado desde HackerSwapChain::Present/Present1, una vez por cuadro, con los
// ciclos que tardaron RunFrameActions (nuestro [Present] incluido) y el
// Present del driver.
void Cuadro(unsigned long long tsc_acciones, unsigned long long tsc_present);   // HackerContext.cpp
// Una vez por proceso, desde Present: placa y soporte de listas de comandos.
void InfoDispositivo(ID3D11Device *dev);   // HackerContext.cpp

} // namespace ttdiag

// IniHandler.cpp: 1 = sesion abierta (marca valida), 0 = reposo, -1 = sin decidir.
int TrueTripleDiagSesion();
