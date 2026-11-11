#pragma once

// TrueTriple: la marca de sesion (10/11/2026).
//
// El plugin (la app suelta o el launcher xPolz) escribe
// `%LOCALAPPDATA%\TrueTriple\sesion-<juego>.json` al apretar Jugar, la renueva
// cada 10 minutos mientras el juego corre y la borra al cerrarse el juego, al
// desactivar, en el saneo y en Jugar sin mod (app/shell/src-tauri/src/
// sesion_marca.rs). El dll la lee UNA sola vez, al cargar la configuracion
// (IniHandler.cpp, LoadConfigFile): sin una marca fresca NO se carga el
// d3dx.ini y el dll queda como con el ini inerte (kIniReposo), aunque el ini de
// la carpeta del juego este armado. La decision se guarda: una recarga del ini
// en la misma partida no vuelve a mirar la marca (como el driver de WRC).
//
// Por que: un d3dx.ini armado copiado una vez a otra PC servia para siempre,
// porque el dll no verificaba nada. Con la marca, ademas hace falta la app.
// Nivel "molestia razonable para un mod de Patreon", no DRM: el formato es
// texto plano y escribir la marca a mano alcanza; nada de cifrado.
//
// Formato (lo escribe serde_json, una sola linea):
//   {"juego":"dr2","desde_epoch":1794300000,"vale_s":7200}
// Vale `vale_s` segundos desde `desde_epoch` (nunca mas de dos horas, diga lo
// que diga el archivo) y tolera un reloj atrasado respecto del que la escribio
// de hasta 10 minutos (marca "en el futuro"). `juego` no se mira: el nombre del
// archivo ya lo dice.
//
// Solo cabecera, sin dependencias fuera de Win32 y la CRT, en C++11 (el
// proyecto de 3Dmigoto compila sin /std:c++17). Fuera de Windows (el test,
// tools/verify_marca_sesion.cpp) la carpeta sale de getenv("LOCALAPPDATA").
// El mismo archivo, identico, vive en el repo de DR1 y en el de DR2: el
// nombre del juego entra por parametro.

#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>

#ifdef _WIN32
#include <windows.h>
#include <share.h>
#endif

namespace truetriple {

// Lo que vale una marca como mucho, aunque `vale_s` diga mas: dos horas
// (VALE_S del lado Rust).
static const long long kMarcaValeMaxSegundos = 2 * 60 * 60;
// Un reloj atrasado respecto del que escribio la marca: hasta 10 min.
static const long long kMarcaFuturoSegundos = 10 * 60;
// La marca real tiene unos 60 bytes; algo mas grande que esto no es nuestro.
static const size_t kMarcaMaxBytes = 4096;

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

// La configuracion del usuario (d3dx_user.ini, las variables `persist`)
// segun la decision de la marca (11/11/2026). `sesion` es lo que guarda
// IniHandler.cpp: -1 = todavia no se decidio, 0 = reposo, 1 = sesion. Solo
// con sesion se lee, se guarda o se borra d3dx_user.ini. Por que: en reposo
// el ini inerte no declara ninguna variable, asi que 3Dmigoto tomaba las
// lineas del archivo por "entradas desconocidas", lo marcaba para reescribir
// (user_config_dirty |= 2) y al salir (DestroyDLL -> SavePersistentSettings)
// lo dejaba con la cabecera y [Constants] solos: se perdian el asiento y los
// presets del usuario.
inline bool SesionUsaUserConfig(int sesion)
{
	return sesion == 1;
}

// El entero de `"clave": 123` en `texto` (terminado en NUL). `false` si la
// clave no esta, si su valor no empieza con un digito (string, negativo,
// null) o si no entra en un long long.
inline bool MarcaEnteroDe(const char *texto, const char *clave, long long *valor)
{
	if (!texto || !clave || !valor)
		return false;
	std::string aguja = std::string("\"") + clave + "\"";
	const char *p = std::strstr(texto, aguja.c_str());
	if (!p)
		return false;
	p += aguja.size();
	while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
		++p;
	if (*p != ':')
		return false;
	++p;
	while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
		++p;
	if (*p < '0' || *p > '9')
		return false;
	errno = 0;
	char *fin = NULL;
	long long v = std::strtoll(p, &fin, 10);
	if (errno == ERANGE || fin == p)
		return false;
	*valor = v;
	return true;
}

// Logica pura: con el texto de la marca y la hora, ¿vale?
inline bool MarcaValidaTexto(const char *texto, long long ahora)
{
	long long desde = 0, vale = 0;
	if (!MarcaEnteroDe(texto, "desde_epoch", &desde) || desde <= 0)
		return false;
	if (!MarcaEnteroDe(texto, "vale_s", &vale) || vale <= 0)
		return false;
	if (vale > kMarcaValeMaxSegundos)
		vale = kMarcaValeMaxSegundos;
	if (desde > ahora)
		return desde - ahora <= kMarcaFuturoSegundos;
	return ahora - desde < vale;
}

// `%LOCALAPPDATA%\TrueTriple\sesion-<juego>.json`, o vacio si no hay
// LOCALAPPDATA.
inline std::wstring MarcaRuta(const wchar_t *juego)
{
	std::wstring base;
#ifdef _WIN32
	DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", NULL, 0);
	if (n == 0)
		return std::wstring();
	std::wstring buf(n, L'\0');
	DWORD m = GetEnvironmentVariableW(L"LOCALAPPDATA", &buf[0], n);
	if (m == 0 || m >= n)
		return std::wstring();
	buf.resize(m);
	base = buf;
	const wchar_t *sep = L"\\";
#else
	const char *env = std::getenv("LOCALAPPDATA");
	if (!env || !*env)
		return std::wstring();
	for (const char *c = env; *c; ++c)
		base.push_back(static_cast<wchar_t>(static_cast<unsigned char>(*c)));
	const wchar_t *sep = L"/";
#endif
	return base + sep + L"TrueTriple" + sep + L"sesion-" + (juego ? juego : L"") + L".json";
}

// Lee la marca de `juego` (como mucho kMarcaMaxBytes) y decide. Archivo
// ausente, ilegible, demasiado grande o roto: no vale. Nunca lanza.
inline bool MarcaDeSesionValida(const wchar_t *juego, time_t ahora)
{
	try {
		std::wstring ruta = MarcaRuta(juego);
		if (ruta.empty())
			return false;
#ifdef _WIN32
		FILE *f = _wfsopen(ruta.c_str(), L"rb", _SH_DENYNO);
#else
		std::string angosta;
		for (size_t i = 0; i < ruta.size(); ++i)
			angosta.push_back(static_cast<char>(ruta[i]));
		FILE *f = std::fopen(angosta.c_str(), "rb");
#endif
		if (!f)
			return false;
		std::string texto(kMarcaMaxBytes + 1, '\0');
		size_t n = std::fread(&texto[0], 1, texto.size(), f);
		bool error = std::ferror(f) != 0;
		std::fclose(f);
		if (error || n > kMarcaMaxBytes)
			return false;
		texto.resize(n);
		return MarcaValidaTexto(texto.c_str(), static_cast<long long>(ahora));
	} catch (...) {
		return false;
	}
}

} // namespace truetriple
