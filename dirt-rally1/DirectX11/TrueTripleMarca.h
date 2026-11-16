#pragma once

// TrueTripleMarca.h, version 2: la marca de sesion FIRMADA de los mods de xPolz.
//
// Fuente canonica: el repo privado xpolz/xpolz-marca, cpp/TrueTripleMarca.h.
// Este archivo se COPIA identico, byte a byte, a cada mod (dll de 3Dmigoto,
// addon de ReShade, version.dll, driver de SteamVR). No se edita en el mod: se
// cambia alla y se vuelve a copiar. La especificacion es
// docs/31-marca-de-sesion-firmada.md del repo xpolz-global.
//
// Que hace. El launcher xPolz (o la app suelta del mod) escribe la marca al
// apretar Jugar, la renueva cada 10 minutos y la borra al cerrar. El binario
// del mod la lee UNA vez, al arrancar, con MarcaVerificarRuta o
// MarcaVerificarLocal: si no devuelve kMarcaOk, el mod queda en reposo y deja
// en su log el motivo (MarcaMotivo). La marca es una linea de JSON:
//
//   {"juego":"dr2","desde_epoch":1794300000,"vale_s":7200,"v":2,"kid":"m1",
//    "pc":"<32 hex>","firma":"<128 hex>"}
//
// `firma` es ed25519 sobre la cadena ASCII
//   xpolz-marca-v2|<kid>|<juego>|<desde_epoch>|<vale_s>|<pc>
// y se verifica con la clave PUBLICA de la tabla kMarcaClaves. `pc` es la
// huella de la PC (MarcaHuellaPc). La marca vale `vale_s` segundos desde
// `desde_epoch`, nunca mas de dos horas, con 10 minutos de tolerancia a un
// reloj corrido.
//
// Aca hay solo verificacion: la clave privada no esta ni se puede derivar de
// nada de este archivo, que por eso puede viajar en fuentes publicas (los dll
// de 3Dmigoto son GPL y los drivers LGPL).
//
// Solo cabecera. Sin dependencias fuera de Win32 (kernel32 y advapi32), sin
// memoria dinamica, sin excepciones, sin estado global, C++11. Compila en MSVC
// x64 y x86. Fuera de Windows compila para las pruebas (la huella de la PC es
// la de reserva).
//
// La verificacion ed25519, SHA-512 y SHA-256 vienen de TweetNaCl (version
// 20140427, D. J. Bernstein, B. van Gastel, W. Janssen, T. Lange, P. Schwabe,
// S. Smetsers), que es de dominio publico, con dos cambios: S canonico (S < L)
// y rechazo de R y A de orden pequeno, para aceptar exactamente lo mismo que
// `verify_strict` de ed25519-dalek.

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#pragma comment(lib, "advapi32.lib")
#else
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#endif

namespace truetriple {

// El resultado de mirar la marca. Todo lo que no es kMarcaOk es reposo.
enum MarcaEstado {
	kMarcaOk = 0,
	kMarcaSinMarca = 1,          // sin_marca: no hay archivo
	kMarcaVencida = 2,           // vencida: firmada y de esta PC, pero vieja
	kMarcaFirmaInvalida = 3,     // firma_invalida: la firma no corresponde
	kMarcaOtraPc = 4,            // otra_pc: firmada para otra PC
	kMarcaOtroJuego = 5,         // otro_juego: firmada para otro juego
	kMarcaFormato = 6,           // formato: vacia, enorme o rota
	kMarcaSinFirma = 7,          // sin_firma: marca v1, de un launcher viejo
	kMarcaClaveDesconocida = 8,  // clave_desconocida: kid que este binario no trae
	kMarcaIlegible = 9           // ilegible: el archivo esta y no se pudo leer
};

// Lo que vale una marca como mucho, aunque `vale_s` diga mas: dos horas.
static const long long kMarcaValeMaxSegundos = 2 * 60 * 60;
// Un reloj atrasado respecto del que escribio la marca: hasta 10 minutos.
static const long long kMarcaFuturoSegundos = 10 * 60;
// La marca real ronda los 260 bytes; algo mas grande que esto no es nuestro.
static const size_t kMarcaMaxBytes = 4096;
// Para el buffer de `detalle` de las funciones de abajo.
static const size_t kMarcaDetalleMax = 256;

// Las claves publicas que este binario acepta, por `kid`. Rotar es agregar una
// fila (la nueva) en una version del mod, cambiar la clave del lado Rust en una
// version del launcher, y sacar la fila vieja en la version siguiente del mod.
struct MarcaClave {
	const char *kid;
	unsigned char publica[32];
};
// CLAVES-INICIO
static const MarcaClave kMarcaClaves[] = {
	{"m1", {
		0xb9, 0x62, 0xfb, 0x17, 0x39, 0x39, 0xa1, 0x77,
		0xc6, 0x56, 0x92, 0x86, 0x31, 0x19, 0xcd, 0x65,
		0xbf, 0x9d, 0xad, 0x35, 0xb8, 0x56, 0x77, 0xf4,
		0xd0, 0xe4, 0xdb, 0x6b, 0x64, 0x23, 0xa3, 0x10,
	}},
};
// CLAVES-FIN

// El motivo, como va en el log (una palabra, sin espacios).
inline const char *MarcaMotivo(MarcaEstado e)
{
	switch (e) {
	case kMarcaOk: return "ok";
	case kMarcaSinMarca: return "sin_marca";
	case kMarcaVencida: return "vencida";
	case kMarcaFirmaInvalida: return "firma_invalida";
	case kMarcaOtraPc: return "otra_pc";
	case kMarcaOtroJuego: return "otro_juego";
	case kMarcaFormato: return "formato";
	case kMarcaSinFirma: return "sin_firma";
	case kMarcaClaveDesconocida: return "clave_desconocida";
	case kMarcaIlegible: return "ilegible";
	}
	return "formato";
}

// ---------------------------------------------------------------------------
// Criptografia: SHA-256, SHA-512 y verificacion ed25519 (TweetNaCl, dominio
// publico). Nada de aca hace falta llamarlo desde el mod.
// ---------------------------------------------------------------------------
namespace marca_detalle {

typedef uint8_t u8;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int64_t i64;
typedef i64 gf[16];

#define TTM_FOR(i, n) for (i = 0; i < (n); ++i)

static const u64 kK512[80] = {
	0x428a2f98d728ae22ULL, 0x7137449123ef65cdULL, 0xb5c0fbcfec4d3b2fULL, 0xe9b5dba58189dbbcULL,
	0x3956c25bf348b538ULL, 0x59f111f1b605d019ULL, 0x923f82a4af194f9bULL, 0xab1c5ed5da6d8118ULL,
	0xd807aa98a3030242ULL, 0x12835b0145706fbeULL, 0x243185be4ee4b28cULL, 0x550c7dc3d5ffb4e2ULL,
	0x72be5d74f27b896fULL, 0x80deb1fe3b1696b1ULL, 0x9bdc06a725c71235ULL, 0xc19bf174cf692694ULL,
	0xe49b69c19ef14ad2ULL, 0xefbe4786384f25e3ULL, 0x0fc19dc68b8cd5b5ULL, 0x240ca1cc77ac9c65ULL,
	0x2de92c6f592b0275ULL, 0x4a7484aa6ea6e483ULL, 0x5cb0a9dcbd41fbd4ULL, 0x76f988da831153b5ULL,
	0x983e5152ee66dfabULL, 0xa831c66d2db43210ULL, 0xb00327c898fb213fULL, 0xbf597fc7beef0ee4ULL,
	0xc6e00bf33da88fc2ULL, 0xd5a79147930aa725ULL, 0x06ca6351e003826fULL, 0x142929670a0e6e70ULL,
	0x27b70a8546d22ffcULL, 0x2e1b21385c26c926ULL, 0x4d2c6dfc5ac42aedULL, 0x53380d139d95b3dfULL,
	0x650a73548baf63deULL, 0x766a0abb3c77b2a8ULL, 0x81c2c92e47edaee6ULL, 0x92722c851482353bULL,
	0xa2bfe8a14cf10364ULL, 0xa81a664bbc423001ULL, 0xc24b8b70d0f89791ULL, 0xc76c51a30654be30ULL,
	0xd192e819d6ef5218ULL, 0xd69906245565a910ULL, 0xf40e35855771202aULL, 0x106aa07032bbd1b8ULL,
	0x19a4c116b8d2d0c8ULL, 0x1e376c085141ab53ULL, 0x2748774cdf8eeb99ULL, 0x34b0bcb5e19b48a8ULL,
	0x391c0cb3c5c95a63ULL, 0x4ed8aa4ae3418acbULL, 0x5b9cca4f7763e373ULL, 0x682e6ff3d6b2b8a3ULL,
	0x748f82ee5defb2fcULL, 0x78a5636f43172f60ULL, 0x84c87814a1f0ab72ULL, 0x8cc702081a6439ecULL,
	0x90befffa23631e28ULL, 0xa4506cebde82bde9ULL, 0xbef9a3f7b2c67915ULL, 0xc67178f2e372532bULL,
	0xca273eceea26619cULL, 0xd186b8c721c0c207ULL, 0xeada7dd6cde0eb1eULL, 0xf57d4f7fee6ed178ULL,
	0x06f067aa72176fbaULL, 0x0a637dc5a2c898a6ULL, 0x113f9804bef90daeULL, 0x1b710b35131c471bULL,
	0x28db77f523047d84ULL, 0x32caab7b40c72493ULL, 0x3c9ebe0a15c9bebcULL, 0x431d67c49c100d4cULL,
	0x4cc5d4becb3e42b6ULL, 0x597f299cfc657e2aULL, 0x5fcb6fab3ad6faecULL, 0x6c44198c4a475817ULL,
};

static const u64 kIV512[8] = {
	0x6a09e667f3bcc908ULL, 0xbb67ae8584caa73bULL, 0x3c6ef372fe94f82bULL, 0xa54ff53a5f1d36f1ULL,
	0x510e527fade682d1ULL, 0x9b05688c2b3e6c1fULL, 0x1f83d9abfb41bd6bULL, 0x5be0cd19137e2179ULL,
};

inline u64 Rot64(u64 x, int c) { return (x >> c) | (x << (64 - c)); }

inline void Bloque512(u64 h[8], const u8 *p)
{
	u64 w[80], a, b, c, d, e, f, g, hh, t1, t2;
	int i, j;
	TTM_FOR(i, 16)
	{
		w[i] = 0;
		TTM_FOR(j, 8) w[i] = (w[i] << 8) | p[8 * i + j];
	}
	for (i = 16; i < 80; ++i) {
		const u64 s0 = Rot64(w[i - 15], 1) ^ Rot64(w[i - 15], 8) ^ (w[i - 15] >> 7);
		const u64 s1 = Rot64(w[i - 2], 19) ^ Rot64(w[i - 2], 61) ^ (w[i - 2] >> 6);
		w[i] = w[i - 16] + s0 + w[i - 7] + s1;
	}
	a = h[0]; b = h[1]; c = h[2]; d = h[3]; e = h[4]; f = h[5]; g = h[6]; hh = h[7];
	TTM_FOR(i, 80)
	{
		t1 = hh + (Rot64(e, 14) ^ Rot64(e, 18) ^ Rot64(e, 41)) + ((e & f) ^ (~e & g)) + kK512[i] + w[i];
		t2 = (Rot64(a, 28) ^ Rot64(a, 34) ^ Rot64(a, 39)) + ((a & b) ^ (a & c) ^ (b & c));
		hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
	}
	h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
}

inline void Sha512(u8 salida[64], const u8 *datos, size_t largo)
{
	u64 h[8];
	u8 ultimo[256];
	size_t i, resto, total;
	int k;
	memcpy(h, kIV512, sizeof(h));
	for (i = 0; i + 128 <= largo; i += 128)
		Bloque512(h, datos + i);
	resto = largo - i;
	memset(ultimo, 0, sizeof(ultimo));
	memcpy(ultimo, datos + i, resto);
	ultimo[resto] = 0x80;
	total = (resto < 112) ? 128 : 256;
	{
		// Largo en 128 bits big-endian; los 64 altos quedan en cero.
		const u64 bits = (u64)largo << 3;
		TTM_FOR(k, 8) ultimo[total - 1 - k] = (u8)(bits >> (8 * k));
		ultimo[total - 9] = (u8)((u64)largo >> 61);
	}
	Bloque512(h, ultimo);
	if (total == 256)
		Bloque512(h, ultimo + 128);
	TTM_FOR(i, 8)
	{
		TTM_FOR(k, 8) salida[8 * i + k] = (u8)(h[i] >> (56 - 8 * k));
	}
}

static const u32 kK256[64] = {
	0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
	0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
	0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
	0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
	0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
	0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
	0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
	0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u,
};

static const u32 kIV256[8] = {
	0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au, 0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u,
};

inline u32 Rot32(u32 x, int c) { return (x >> c) | (x << (32 - c)); }

inline void Bloque256(u32 h[8], const u8 *p)
{
	u32 w[64], a, b, c, d, e, f, g, hh, t1, t2;
	int i;
	TTM_FOR(i, 16)
		w[i] = ((u32)p[4 * i] << 24) | ((u32)p[4 * i + 1] << 16) | ((u32)p[4 * i + 2] << 8) | p[4 * i + 3];
	for (i = 16; i < 64; ++i) {
		const u32 s0 = Rot32(w[i - 15], 7) ^ Rot32(w[i - 15], 18) ^ (w[i - 15] >> 3);
		const u32 s1 = Rot32(w[i - 2], 17) ^ Rot32(w[i - 2], 19) ^ (w[i - 2] >> 10);
		w[i] = w[i - 16] + s0 + w[i - 7] + s1;
	}
	a = h[0]; b = h[1]; c = h[2]; d = h[3]; e = h[4]; f = h[5]; g = h[6]; hh = h[7];
	TTM_FOR(i, 64)
	{
		t1 = hh + (Rot32(e, 6) ^ Rot32(e, 11) ^ Rot32(e, 25)) + ((e & f) ^ (~e & g)) + kK256[i] + w[i];
		t2 = (Rot32(a, 2) ^ Rot32(a, 13) ^ Rot32(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
		hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
	}
	h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
}

inline void Sha256(u8 salida[32], const u8 *datos, size_t largo)
{
	u32 h[8];
	u8 ultimo[128];
	size_t i, resto, total;
	int k;
	memcpy(h, kIV256, sizeof(h));
	for (i = 0; i + 64 <= largo; i += 64)
		Bloque256(h, datos + i);
	resto = largo - i;
	memset(ultimo, 0, sizeof(ultimo));
	memcpy(ultimo, datos + i, resto);
	ultimo[resto] = 0x80;
	total = (resto < 56) ? 64 : 128;
	{
		const u64 bits = (u64)largo << 3;
		TTM_FOR(k, 8) ultimo[total - 1 - k] = (u8)(bits >> (8 * k));
	}
	Bloque256(h, ultimo);
	if (total == 128)
		Bloque256(h, ultimo + 64);
	TTM_FOR(i, 8)
	{
		salida[4 * i] = (u8)(h[i] >> 24);
		salida[4 * i + 1] = (u8)(h[i] >> 16);
		salida[4 * i + 2] = (u8)(h[i] >> 8);
		salida[4 * i + 3] = (u8)h[i];
	}
}

// Campo y curva (TweetNaCl).
static const gf kGf0 = {0};
static const gf kGf1 = {1};
static const gf kD = {0x78a3, 0x1359, 0x4dca, 0x75eb, 0xd8ab, 0x4141, 0x0a4d, 0x0070,
                      0xe898, 0x7779, 0x4079, 0x8cc7, 0xfe73, 0x2b6f, 0x6cee, 0x5203};
static const gf kD2 = {0xf159, 0x26b2, 0x9b94, 0xebd6, 0xb156, 0x8283, 0x149a, 0x00e0,
                       0xd130, 0xeef3, 0x80f2, 0x198e, 0xfce7, 0x56df, 0xd9dc, 0x2406};
static const gf kBx = {0xd51a, 0x8f25, 0x2d60, 0xc956, 0xa7b2, 0x9525, 0xc760, 0x692c,
                       0xdc5c, 0xfdd6, 0xe231, 0xc0a4, 0x53fe, 0xcd6e, 0x36d3, 0x2169};
static const gf kBy = {0x6658, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666,
                       0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666};
static const gf kRaiz = {0xa0b0, 0x4a0e, 0x1b27, 0xc4ee, 0xe478, 0xad2f, 0x1806, 0x2f43,
                         0xd7a7, 0x3dfb, 0x0099, 0x2b4d, 0xdf0b, 0x4fc1, 0x2480, 0x2b83};

inline int Vn(const u8 *x, const u8 *y, int n)
{
	u32 i, d = 0;
	TTM_FOR(i, (u32)n) d |= x[i] ^ y[i];
	return (1 & ((d - 1) >> 8)) - 1;
}

inline void Set25519(gf r, const gf a)
{
	int i;
	TTM_FOR(i, 16) r[i] = a[i];
}

inline void Car25519(gf o)
{
	int i;
	i64 c;
	TTM_FOR(i, 16)
	{
		o[i] += ((i64)1 << 16);
		c = o[i] >> 16;
		o[(i + 1) * (i < 15)] += c - 1 + 37 * (c - 1) * (i == 15);
		o[i] -= c * 65536;
	}
}

inline void Sel25519(gf p, gf q, int b)
{
	i64 t, c = ~(i64)(b - 1);
	int i;
	TTM_FOR(i, 16)
	{
		t = c & (p[i] ^ q[i]);
		p[i] ^= t;
		q[i] ^= t;
	}
}

inline void Pack25519(u8 *o, const gf n)
{
	int i, j, b;
	gf m, t;
	TTM_FOR(i, 16) t[i] = n[i];
	Car25519(t);
	Car25519(t);
	Car25519(t);
	TTM_FOR(j, 2)
	{
		m[0] = t[0] - 0xffed;
		for (i = 1; i < 15; i++) {
			m[i] = t[i] - 0xffff - ((m[i - 1] >> 16) & 1);
			m[i - 1] &= 0xffff;
		}
		m[15] = t[15] - 0x7fff - ((m[14] >> 16) & 1);
		b = (int)((m[15] >> 16) & 1);
		m[14] &= 0xffff;
		Sel25519(t, m, 1 - b);
	}
	TTM_FOR(i, 16)
	{
		o[2 * i] = (u8)(t[i] & 0xff);
		o[2 * i + 1] = (u8)(t[i] >> 8);
	}
}

inline int Neq25519(const gf a, const gf b)
{
	u8 c[32], d[32];
	Pack25519(c, a);
	Pack25519(d, b);
	return Vn(c, d, 32);
}

inline u8 Par25519(const gf a)
{
	u8 d[32];
	Pack25519(d, a);
	return d[0] & 1;
}

inline void Unpack25519(gf o, const u8 *n)
{
	int i;
	TTM_FOR(i, 16) o[i] = n[2 * i] + ((i64)n[2 * i + 1] << 8);
	o[15] &= 0x7fff;
}

inline void Sum(gf o, const gf a, const gf b)
{
	int i;
	TTM_FOR(i, 16) o[i] = a[i] + b[i];
}

inline void Res(gf o, const gf a, const gf b)
{
	int i;
	TTM_FOR(i, 16) o[i] = a[i] - b[i];
}

inline void Mul(gf o, const gf a, const gf b)
{
	i64 t[31];
	int i, j;
	TTM_FOR(i, 31) t[i] = 0;
	TTM_FOR(i, 16) TTM_FOR(j, 16) t[i + j] += a[i] * b[j];
	TTM_FOR(i, 15) t[i] += 38 * t[i + 16];
	TTM_FOR(i, 16) o[i] = t[i];
	Car25519(o);
	Car25519(o);
}

inline void Cua(gf o, const gf a) { Mul(o, a, a); }

inline void Inv25519(gf o, const gf i)
{
	gf c;
	int a;
	TTM_FOR(a, 16) c[a] = i[a];
	for (a = 253; a >= 0; a--) {
		Cua(c, c);
		if (a != 2 && a != 4)
			Mul(c, c, i);
	}
	TTM_FOR(a, 16) o[a] = c[a];
}

inline void Pow2523(gf o, const gf i)
{
	gf c;
	int a;
	TTM_FOR(a, 16) c[a] = i[a];
	for (a = 250; a >= 0; a--) {
		Cua(c, c);
		if (a != 1)
			Mul(c, c, i);
	}
	TTM_FOR(a, 16) o[a] = c[a];
}

inline void SumaPuntos(gf p[4], gf q[4])
{
	gf a, b, c, d, t, e, f, g, h;
	Res(a, p[1], p[0]);
	Res(t, q[1], q[0]);
	Mul(a, a, t);
	Sum(b, p[0], p[1]);
	Sum(t, q[0], q[1]);
	Mul(b, b, t);
	Mul(c, p[3], q[3]);
	Mul(c, c, kD2);
	Mul(d, p[2], q[2]);
	Sum(d, d, d);
	Res(e, b, a);
	Res(f, d, c);
	Sum(g, d, c);
	Sum(h, b, a);
	Mul(p[0], e, f);
	Mul(p[1], h, g);
	Mul(p[2], g, f);
	Mul(p[3], e, h);
}

inline void Cswap(gf p[4], gf q[4], u8 b)
{
	int i;
	TTM_FOR(i, 4) Sel25519(p[i], q[i], b);
}

inline void PackPunto(u8 *r, gf p[4])
{
	gf tx, ty, zi;
	Inv25519(zi, p[2]);
	Mul(tx, p[0], zi);
	Mul(ty, p[1], zi);
	Pack25519(r, ty);
	r[31] ^= (u8)(Par25519(tx) << 7);
}

inline void Scalarmult(gf p[4], gf q[4], const u8 *s)
{
	int i;
	Set25519(p[0], kGf0);
	Set25519(p[1], kGf1);
	Set25519(p[2], kGf1);
	Set25519(p[3], kGf0);
	for (i = 255; i >= 0; --i) {
		u8 b = (s[i / 8] >> (i & 7)) & 1;
		Cswap(p, q, b);
		SumaPuntos(q, p);
		SumaPuntos(p, p);
		Cswap(p, q, b);
	}
}

inline void Scalarbase(gf p[4], const u8 *s)
{
	gf q[4];
	Set25519(q[0], kBx);
	Set25519(q[1], kBy);
	Set25519(q[2], kGf1);
	Mul(q[3], kBx, kBy);
	Scalarmult(p, q, s);
}

static const i64 kL[32] = {0xed, 0xd3, 0xf5, 0x5c, 0x1a, 0x63, 0x12, 0x58,
                           0xd6, 0x9c, 0xf7, 0xa2, 0xde, 0xf9, 0xde, 0x14,
                           0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x10};

inline void ModL(u8 *r, i64 x[64])
{
	i64 carry, i, j;
	for (i = 63; i >= 32; --i) {
		carry = 0;
		for (j = i - 32; j < i - 12; ++j) {
			x[j] += carry - 16 * x[i] * kL[j - (i - 32)];
			carry = (x[j] + 128) >> 8;
			x[j] -= carry * 256;
		}
		x[j] += carry;
		x[i] = 0;
	}
	carry = 0;
	TTM_FOR(j, 32)
	{
		x[j] += carry - (x[31] >> 4) * kL[j];
		carry = x[j] >> 8;
		x[j] &= 255;
	}
	TTM_FOR(j, 32) x[j] -= carry * kL[j];
	TTM_FOR(i, 32)
	{
		x[i + 1] += x[i] >> 8;
		r[i] = (u8)(x[i] & 255);
	}
}

inline void Reducir(u8 *r)
{
	i64 x[64];
	int i;
	TTM_FOR(i, 64) x[i] = (i64)(u64)r[i];
	TTM_FOR(i, 64) r[i] = 0;
	ModL(r, x);
}

inline int Unpackneg(gf r[4], const u8 p[32])
{
	gf t, chk, num, den, den2, den4, den6;
	Set25519(r[2], kGf1);
	Unpack25519(r[1], p);
	Cua(num, r[1]);
	Mul(den, num, kD);
	Res(num, num, r[2]);
	Sum(den, r[2], den);
	Cua(den2, den);
	Cua(den4, den2);
	Mul(den6, den4, den2);
	Mul(t, den6, num);
	Mul(t, t, den);
	Pow2523(t, t);
	Mul(t, t, num);
	Mul(t, t, den);
	Mul(t, t, den);
	Mul(r[0], t, den);
	Cua(chk, r[0]);
	Mul(chk, chk, den);
	if (Neq25519(chk, num))
		Mul(r[0], r[0], kRaiz);
	Cua(chk, r[0]);
	Mul(chk, chk, den);
	if (Neq25519(chk, num))
		return -1;
	if (Par25519(r[0]) == (p[31] >> 7))
		Res(r[0], kGf0, r[0]);
	Mul(r[3], r[0], r[1]);
	return 0;
}

// ¿El punto codificado en `p` es de orden pequeno (divide a 8) o no decodifica?
inline int OrdenPequenoOInvalido(const u8 p[32])
{
	gf q[4];
	int i;
	if (Unpackneg(q, p))
		return 1;
	TTM_FOR(i, 3) SumaPuntos(q, q); // [8](-P)
	// Identidad en coordenadas extendidas: X == 0 y Y == Z.
	return !Neq25519(q[0], kGf0) && !Neq25519(q[1], q[2]);
}

// S < L, comparando como entero de 256 bits little-endian.
inline int EscalarCanonico(const u8 s[32])
{
	int i;
	for (i = 31; i >= 0; --i) {
		if ((i64)s[i] < kL[i]) return 1;
		if ((i64)s[i] > kL[i]) return 0;
	}
	return 0; // S == L
}

// 1 si `firma` (R||S) es una firma ed25519 de `mensaje` con `publica`, con las
// reglas de `verify_strict` de ed25519-dalek. `largo` hasta 256 bytes.
inline int Ed25519Verificar(const u8 firma[64], const u8 *mensaje, size_t largo, const u8 publica[32])
{
	u8 t[32], h[64];
	gf p[4], q[4];
	u8 buf[64 + 256];

	if (largo > sizeof(buf) - 64)
		return 0;
	if (!EscalarCanonico(firma + 32))
		return 0;
	if (OrdenPequenoOInvalido(publica) || OrdenPequenoOInvalido(firma))
		return 0;
	if (Unpackneg(q, publica))
		return 0;
	memcpy(buf, firma, 32);
	memcpy(buf + 32, publica, 32);
	if (largo)
		memcpy(buf + 64, mensaje, largo);
	Sha512(h, buf, 64 + largo);
	Reducir(h);
	Scalarmult(p, q, h);
	Scalarbase(q, firma + 32);
	SumaPuntos(p, q);
	PackPunto(t, p);
	return Vn(firma, t, 32) == 0;
}

#undef TTM_FOR

// --- texto ------------------------------------------------------------------

// Agrega `s` a `out` (terminado en NUL, capacidad `cap`). false si no entra.
inline bool Agregar(char *out, size_t cap, size_t *n, const char *s, size_t largo)
{
	if (!out || *n + largo + 1 > cap)
		return false;
	memcpy(out + *n, s, largo);
	*n += largo;
	out[*n] = '\0';
	return true;
}

inline bool AgregarTexto(char *out, size_t cap, size_t *n, const char *s)
{
	return Agregar(out, cap, n, s, strlen(s));
}

inline bool AgregarNumero(char *out, size_t cap, size_t *n, unsigned long long v)
{
	char tmp[24];
	size_t i = sizeof(tmp);
	do {
		tmp[--i] = (char)('0' + (int)(v % 10));
		v /= 10;
	} while (v);
	return Agregar(out, cap, n, tmp + i, sizeof(tmp) - i);
}

inline void AHex(const u8 *b, size_t n, char *out)
{
	static const char kHex[] = "0123456789abcdef";
	for (size_t i = 0; i < n; ++i) {
		out[2 * i] = kHex[b[i] >> 4];
		out[2 * i + 1] = kHex[b[i] & 15];
	}
	out[2 * n] = '\0';
}

inline int DeHex(char c)
{
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	return -1;
}

inline bool EsNombre(char c)
{
	return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
}

inline bool EsBlanco(char c)
{
	return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

enum { kCampoFalta = 0, kCampoMalo = 1, kCampoNumero = 2, kCampoTexto = 3 };

// El valor de `"clave"` en el JSON: la primera aparicion de `"clave"` seguida
// (saltando blancos) de `:`. El valor es un entero de 1 a 18 digitos o un
// texto entre comillas de hasta 128 caracteres de a-z 0-9 _ -.
inline int Campo(const char *t, size_t n, const char *clave, unsigned long long *numero, const char **ini, size_t *largo)
{
	char aguja[40];
	const size_t lc = strlen(clave);
	if (lc + 2 >= sizeof(aguja))
		return kCampoMalo;
	aguja[0] = '"';
	memcpy(aguja + 1, clave, lc);
	aguja[lc + 1] = '"';
	const size_t la = lc + 2;
	for (size_t p = 0; p + la <= n; ++p) {
		if (memcmp(t + p, aguja, la) != 0)
			continue;
		size_t i = p + la;
		while (i < n && EsBlanco(t[i]))
			++i;
		if (i >= n || t[i] != ':')
			continue;
		++i;
		while (i < n && EsBlanco(t[i]))
			++i;
		if (i >= n)
			return kCampoMalo;
		if (t[i] >= '0' && t[i] <= '9') {
			const size_t desde = i;
			unsigned long long v = 0;
			while (i < n && t[i] >= '0' && t[i] <= '9') {
				if (i - desde >= 18)
					return kCampoMalo;
				v = v * 10 + (unsigned long long)(t[i] - '0');
				++i;
			}
			*numero = v;
			return kCampoNumero;
		}
		if (t[i] == '"') {
			const size_t desde = i + 1;
			size_t j = desde;
			while (j < n && t[j] != '"') {
				if (j - desde >= 128 || !EsNombre(t[j]))
					return kCampoMalo;
				++j;
			}
			if (j >= n)
				return kCampoMalo;
			*ini = t + desde;
			*largo = j - desde;
			return kCampoTexto;
		}
		return kCampoMalo;
	}
	return kCampoFalta;
}

inline bool TodoHex(const char *s, size_t n)
{
	for (size_t i = 0; i < n; ++i)
		if (DeHex(s[i]) < 0)
			return false;
	return true;
}

} // namespace marca_detalle

// ---------------------------------------------------------------------------
// La huella de la PC.
// ---------------------------------------------------------------------------

// La huella (32 hex y NUL) de un MachineGuid ya normalizado:
// hex(SHA-256("xpolz-pc-v1|" + guid)) truncado a 16 bytes.
inline void MarcaHuellaDeGuid(const char *guid, size_t largo, char out[33])
{
	unsigned char buf[12 + 64], d[32];
	if (largo > 64)
		largo = 64;
	memcpy(buf, "xpolz-pc-v1|", 12);
	memcpy(buf + 12, guid, largo);
	marca_detalle::Sha256(d, buf, 12 + largo);
	marca_detalle::AHex(d, 16, out);
}

// La huella de una PC cuyo MachineGuid no se puede leer.
inline void MarcaHuellaReserva(char out[33])
{
	MarcaHuellaDeGuid("sin-machineguid", 15, out);
}

// La huella de ESTA PC. true si se leyo el MachineGuid de
// HKLM\SOFTWARE\Microsoft\Cryptography (vista de 64 bits, tambien desde un
// proceso de 32 bits); false si no se pudo, y entonces `out` es la de reserva.
inline bool MarcaHuellaPc(char out[33])
{
#ifdef _WIN32
	HKEY clave = NULL;
	if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Cryptography", 0,
	                  KEY_QUERY_VALUE | KEY_WOW64_64KEY, &clave) == ERROR_SUCCESS) {
		// Hasta 258 unidades (516 bytes); un valor mas largo cuenta como no leido,
		// igual que del lado Rust.
		wchar_t crudo[258];
		DWORD tipo = 0, bytes = sizeof(crudo);
		const LSTATUS r = RegQueryValueExW(clave, L"MachineGuid", NULL, &tipo, (LPBYTE)crudo, &bytes);
		RegCloseKey(clave);
		if (r == ERROR_SUCCESS && (tipo == REG_SZ || tipo == REG_EXPAND_SZ)) {
			size_t fin = bytes / sizeof(wchar_t), ini = 0;
			// Sin blancos ni controles en las puntas (incluye el NUL final).
			while (ini < fin && crudo[ini] <= 0x20)
				++ini;
			while (fin > ini && crudo[fin - 1] <= 0x20)
				--fin;
			if (fin - ini > 64)
				fin = ini + 64;
			if (fin > ini) {
				char guid[64];
				for (size_t i = ini; i < fin; ++i) {
					const wchar_t u = crudo[i];
					guid[i - ini] = (u >= L'A' && u <= L'Z') ? (char)(u + 0x20)
					              : (u >= 0x21 && u <= 0x7E) ? (char)u
					              : '?';
				}
				MarcaHuellaDeGuid(guid, fin - ini, out);
				return true;
			}
		}
	}
#endif
	MarcaHuellaReserva(out);
	return false;
}

// ---------------------------------------------------------------------------
// La verificacion.
// ---------------------------------------------------------------------------

// Logica pura: con el texto de la marca, el juego que espera este binario, la
// huella de esta PC (NULL si no se pudo leer: entonces no se compara) y la
// hora. `detalle` (opcional, hasta kMarcaDetalleMax) recibe una linea corta
// para el log de soporte, sin datos que identifiquen a nadie.
inline MarcaEstado MarcaVerificarTexto(const char *texto, size_t n, const char *juego, const char *pc_propia,
                                       long long ahora, char *detalle = NULL, size_t cap = 0)
{
	using namespace marca_detalle;
	size_t nd = 0;
	if (detalle && cap)
		detalle[0] = '\0';
	else
		detalle = NULL;
	if (detalle) {
		AgregarTexto(detalle, cap, &nd, "bytes=");
		AgregarNumero(detalle, cap, &nd, n);
	}
	if (!texto || !juego || n == 0 || n > kMarcaMaxBytes)
		return kMarcaFormato;

	unsigned long long desde = 0, vale = 0, version = 0, nada = 0;
	const char *s = NULL, *s_juego = NULL, *s_kid = NULL, *s_pc = NULL, *s_firma = NULL;
	size_t l = 0, l_juego = 0, l_kid = 0, l_pc = 0, l_firma = 0;

	if (Campo(texto, n, "desde_epoch", &desde, &s, &l) != kCampoNumero || desde == 0)
		return kMarcaFormato;
	const int hay_firma = Campo(texto, n, "firma", &nada, &s_firma, &l_firma);
	if (hay_firma == kCampoFalta)
		return kMarcaSinFirma; // una v1: tiene la hora y no tiene firma
	if (hay_firma != kCampoTexto || l_firma != 128 || !TodoHex(s_firma, 128))
		return kMarcaFormato;
	if (Campo(texto, n, "v", &version, &s, &l) != kCampoNumero || version != 2)
		return kMarcaFormato;
	if (Campo(texto, n, "vale_s", &vale, &s, &l) != kCampoNumero || vale == 0)
		return kMarcaFormato;
	if (Campo(texto, n, "juego", &nada, &s_juego, &l_juego) != kCampoTexto || l_juego < 1 || l_juego > 32)
		return kMarcaFormato;
	if (Campo(texto, n, "kid", &nada, &s_kid, &l_kid) != kCampoTexto || l_kid < 1 || l_kid > 16)
		return kMarcaFormato;
	for (size_t i = 0; i < l_kid; ++i)
		if (s_kid[i] == '_' || s_kid[i] == '-')
			return kMarcaFormato;
	if (Campo(texto, n, "pc", &nada, &s_pc, &l_pc) != kCampoTexto || l_pc != 32 || !TodoHex(s_pc, 32))
		return kMarcaFormato;

	if (detalle) {
		AgregarTexto(detalle, cap, &nd, " kid=");
		Agregar(detalle, cap, &nd, s_kid, l_kid);
		AgregarTexto(detalle, cap, &nd, " juego=");
		Agregar(detalle, cap, &nd, s_juego, l_juego);
		AgregarTexto(detalle, cap, &nd, " desde=");
		AgregarNumero(detalle, cap, &nd, desde);
		AgregarTexto(detalle, cap, &nd, " vale=");
		AgregarNumero(detalle, cap, &nd, vale);
		AgregarTexto(detalle, cap, &nd, " ahora=");
		AgregarNumero(detalle, cap, &nd, ahora > 0 ? (unsigned long long)ahora : 0);
		AgregarTexto(detalle, cap, &nd, " pc_marca=");
		Agregar(detalle, cap, &nd, s_pc, 8);
		AgregarTexto(detalle, cap, &nd, " pc_propia=");
		if (pc_propia)
			Agregar(detalle, cap, &nd, pc_propia, strlen(pc_propia) < 8 ? strlen(pc_propia) : 8);
		else
			AgregarTexto(detalle, cap, &nd, "no_leida");
	}

	const MarcaClave *clave = NULL;
	for (size_t i = 0; i < sizeof(kMarcaClaves) / sizeof(kMarcaClaves[0]); ++i)
		if (strlen(kMarcaClaves[i].kid) == l_kid && memcmp(kMarcaClaves[i].kid, s_kid, l_kid) == 0)
			clave = &kMarcaClaves[i];
	if (!clave)
		return kMarcaClaveDesconocida;

	// La cadena que se firmo: xpolz-marca-v2|<kid>|<juego>|<desde>|<vale>|<pc>
	char canonica[192];
	size_t nc = 0;
	canonica[0] = '\0';
	bool entra = AgregarTexto(canonica, sizeof(canonica), &nc, "xpolz-marca-v2|");
	entra = entra && Agregar(canonica, sizeof(canonica), &nc, s_kid, l_kid);
	entra = entra && AgregarTexto(canonica, sizeof(canonica), &nc, "|");
	entra = entra && Agregar(canonica, sizeof(canonica), &nc, s_juego, l_juego);
	entra = entra && AgregarTexto(canonica, sizeof(canonica), &nc, "|");
	entra = entra && AgregarNumero(canonica, sizeof(canonica), &nc, desde);
	entra = entra && AgregarTexto(canonica, sizeof(canonica), &nc, "|");
	entra = entra && AgregarNumero(canonica, sizeof(canonica), &nc, vale);
	entra = entra && AgregarTexto(canonica, sizeof(canonica), &nc, "|");
	entra = entra && Agregar(canonica, sizeof(canonica), &nc, s_pc, l_pc);
	if (!entra)
		return kMarcaFormato;
	unsigned char firma[64];
	for (int i = 0; i < 64; ++i)
		firma[i] = (unsigned char)((DeHex(s_firma[2 * i]) << 4) | DeHex(s_firma[2 * i + 1]));
	if (!Ed25519Verificar(firma, (const unsigned char *)canonica, nc, clave->publica))
		return kMarcaFirmaInvalida;

	if (strlen(juego) != l_juego || memcmp(juego, s_juego, l_juego) != 0)
		return kMarcaOtroJuego;

	// La PC. Dos reglas para que quien paga nunca se quede sin mod: si este
	// proceso no pudo leer su MachineGuid no se compara, y una marca con la
	// huella de reserva (el que escribio no pudo leerlo) vale en cualquier PC.
	if (pc_propia) {
		char reserva[33];
		MarcaHuellaReserva(reserva);
		if ((strlen(pc_propia) != 32 || memcmp(pc_propia, s_pc, 32) != 0) && memcmp(reserva, s_pc, 32) != 0)
			return kMarcaOtraPc;
	}

	const unsigned long long ahora_u = ahora > 0 ? (unsigned long long)ahora : 0;
	if (vale > (unsigned long long)kMarcaValeMaxSegundos)
		vale = (unsigned long long)kMarcaValeMaxSegundos;
	if (desde > ahora_u)
		return desde - ahora_u <= (unsigned long long)kMarcaFuturoSegundos ? kMarcaOk : kMarcaVencida;
	return ahora_u - desde < vale ? kMarcaOk : kMarcaVencida;
}

namespace marca_detalle {

// Lee el archivo entero (hasta kMarcaMaxBytes + 1) en `buf`. Devuelve
// kMarcaOk, kMarcaSinMarca o kMarcaIlegible (y el error del sistema).
inline MarcaEstado LeerArchivo(const wchar_t *ruta_w, const char *ruta_a, char *buf, size_t cap, size_t *n, unsigned long *error)
{
	*n = 0;
	*error = 0;
#ifdef _WIN32
	// La marca se reemplaza con un rename: si justo esta tomada, se reintenta.
	for (int intento = 0; intento < 5; ++intento) {
		const DWORD compartir = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
		HANDLE h = ruta_w ? CreateFileW(ruta_w, GENERIC_READ, compartir, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL)
		                  : CreateFileA(ruta_a, GENERIC_READ, compartir, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
		if (h == INVALID_HANDLE_VALUE) {
			*error = GetLastError();
			if (*error == ERROR_FILE_NOT_FOUND || *error == ERROR_PATH_NOT_FOUND)
				return kMarcaSinMarca;
			Sleep(40);
			continue;
		}
		size_t total = 0;
		bool bien = true;
		while (total < cap) {
			DWORD leidos = 0;
			if (!ReadFile(h, buf + total, (DWORD)(cap - total), &leidos, NULL)) {
				*error = GetLastError();
				bien = false;
				break;
			}
			if (leidos == 0)
				break;
			total += leidos;
		}
		CloseHandle(h);
		if (bien) {
			*n = total;
			*error = 0;
			return kMarcaOk;
		}
		Sleep(40);
	}
	return kMarcaIlegible;
#else
	char angosta[1024];
	if (ruta_w) {
		size_t i = 0;
		for (; ruta_w[i] && i + 1 < sizeof(angosta); ++i)
			angosta[i] = (char)ruta_w[i];
		angosta[i] = '\0';
		ruta_a = angosta;
	}
	FILE *f = fopen(ruta_a, "rb");
	if (!f) {
		*error = (unsigned long)errno;
		return errno == ENOENT ? kMarcaSinMarca : kMarcaIlegible;
	}
	*n = fread(buf, 1, cap, f);
	const bool mal = ferror(f) != 0;
	fclose(f);
	return mal ? kMarcaIlegible : kMarcaOk;
#endif
}

inline MarcaEstado VerificarRuta(const wchar_t *ruta_w, const char *ruta_a, const char *juego, long long ahora, char *detalle, size_t cap)
{
	char texto[4096 + 1];
	size_t n = 0, nd = 0;
	unsigned long error = 0;
	if (detalle && cap)
		detalle[0] = '\0';
	else
		detalle = NULL;
	if ((!ruta_w && !ruta_a) || !juego)
		return kMarcaSinMarca;
	const MarcaEstado leido = LeerArchivo(ruta_w, ruta_a, texto, sizeof(texto), &n, &error);
	if (leido != kMarcaOk) {
		if (detalle && leido == kMarcaIlegible) {
			AgregarTexto(detalle, cap, &nd, "error=");
			AgregarNumero(detalle, cap, &nd, error);
		}
		return leido;
	}
	char pc[33];
	const bool leida = MarcaHuellaPc(pc);
	return MarcaVerificarTexto(texto, n, juego, leida ? pc : NULL, ahora, detalle, cap);
}

} // namespace marca_detalle

// Lee la marca de `ruta` y decide para `juego` (el nombre que espera ESTE
// binario: "dr2", "fh6", "vrto3d"...) con la hora `ahora` (segundos desde
// 1970, `time(NULL)`). Nunca lanza ni reserva memoria. Se llama UNA vez por
// proceso y la decision se guarda.
inline MarcaEstado MarcaVerificarRuta(const wchar_t *ruta, const char *juego, long long ahora, char *detalle = NULL, size_t cap = 0)
{
	return marca_detalle::VerificarRuta(ruta, NULL, juego, ahora, detalle, cap);
}

// Igual, con la ruta angosta (la pagina de codigos del proceso), como la arman
// los drivers de SteamVR.
inline MarcaEstado MarcaVerificarRutaA(const char *ruta, const char *juego, long long ahora, char *detalle = NULL, size_t cap = 0)
{
	return marca_detalle::VerificarRuta(NULL, ruta, juego, ahora, detalle, cap);
}

// `%LOCALAPPDATA%\TrueTriple\sesion-<juego>.json` en `out`. false si no hay
// LOCALAPPDATA o no entra.
inline bool MarcaRutaLocal(const char *juego, wchar_t *out, size_t cap)
{
	if (!juego || !out || cap == 0)
		return false;
	size_t n = 0;
#ifdef _WIN32
	const DWORD m = GetEnvironmentVariableW(L"LOCALAPPDATA", out, (DWORD)cap);
	if (m == 0 || m >= cap)
		return false;
	n = m;
	const wchar_t *medio = L"\\TrueTriple\\sesion-";
#else
	const char *env = getenv("LOCALAPPDATA");
	if (!env || !*env)
		return false;
	for (; env[n]; ++n) {
		if (n + 1 >= cap)
			return false;
		out[n] = (wchar_t)(unsigned char)env[n];
	}
	const wchar_t *medio = L"/TrueTriple/sesion-";
#endif
	const wchar_t *fin = L".json";
	for (const wchar_t *p = medio; *p; ++p) {
		if (n + 1 >= cap)
			return false;
		out[n++] = *p;
	}
	for (const char *p = juego; *p; ++p) {
		if (n + 1 >= cap)
			return false;
		out[n++] = (wchar_t)(unsigned char)*p;
	}
	for (const wchar_t *p = fin; *p; ++p) {
		if (n + 1 >= cap)
			return false;
		out[n++] = *p;
	}
	out[n] = L'\0';
	return true;
}

// MarcaVerificarRuta sobre `%LOCALAPPDATA%\TrueTriple\sesion-<juego>.json`: lo
// que usan los dll de 3Dmigoto, el addon de ReShade y los version.dll.
inline MarcaEstado MarcaVerificarLocal(const char *juego, long long ahora, char *detalle = NULL, size_t cap = 0)
{
	wchar_t ruta[520];
	if (detalle && cap)
		detalle[0] = '\0';
	if (!MarcaRutaLocal(juego, ruta, sizeof(ruta) / sizeof(ruta[0])))
		return kMarcaSinMarca;
	return MarcaVerificarRuta(ruta, juego, ahora, detalle, cap);
}

} // namespace truetriple
