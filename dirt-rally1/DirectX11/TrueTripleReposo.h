#pragma once

// TrueTripleReposo.h: lo que la cabecera v1 de la marca traia SOLO para los dll
// de 3Dmigoto (DR2, DR1, Wreckfest) y que no es parte de la marca. Va al lado
// de TrueTripleMarca.h en esos tres mods; los demas no lo usan.

namespace truetriple {

// El ini con que queda el dll sin sesion: el mismo contenido que
// launcher/TrueTriple.idle.ini (el inerte que deja la app al cerrar el juego),
// sin la linea de comentario. Mantenerlos iguales: es el estado que ya se
// probo en pista.
static const char kIniReposo[] =
	"[Logging]\n"
	"calls=0\n"
	"input=0\n"
	"debug=0\n"
	"unbuffered=0\n"
	"force_cpu_affinity=0\n"
	"crash=0\n"
	"[Rendering]\n"
	"override_directory=ShaderFixes\n"
	"[Hunting]\n"
	"hunting=0\n";

// La configuracion del usuario (d3dx_user.ini, las variables `persist`) segun
// la decision de la marca (11/11/2026). `sesion` es lo que guarda
// IniHandler.cpp: -1 = todavia no se decidio, 0 = reposo, 1 = sesion. Solo con
// sesion se lee, se guarda o se borra d3dx_user.ini: en reposo el ini inerte
// no declara ninguna variable y 3Dmigoto lo reescribia vacio al salir.
inline bool SesionUsaUserConfig(int sesion)
{
	return sesion == 1;
}

} // namespace truetriple
