#include "TrueTripleTaaLock.h"

#include <cstdint>
#include <cstring>

namespace TrueTripleTaaLock
{
namespace
{
// DiRT Rally 2.0 1.18.130.9032, SHA256
// C119F509FC315D8168AA7AAA5D0A1EE8E3570C7220BD6CBB2D337D6725D73442.
constexpr DWORD kExpectedTimestamp = 0x605CBAE3;
constexpr DWORD kExpectedSizeOfImage = 0x02255000;
constexpr std::uintptr_t kTaaSetterRva = 0x005F5340;

// Render TAA state setter:
//   original: mov byte ptr [rcx], dl ; ret ; int3
//   patched:  mov byte ptr [rcx], 0  ; ret
// The four-byte site is naturally aligned, so the code transition itself can
// be one atomic DWORD exchange after the page protection has changed.
constexpr LONG kOriginalDword = static_cast<LONG>(0xCCC31188u);
constexpr LONG kPatchedDword = static_cast<LONG>(0xC30001C6u);
constexpr std::uint8_t kExpectedSite[8] = {
	0x88, 0x11, 0xC3, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC,
};
constexpr std::uint8_t kExpectedPatchedSite[8] = {
	0xC6, 0x01, 0x00, 0xC3, 0xCC, 0xCC, 0xCC, 0xCC,
};
static_assert((kTaaSetterRva & 3u) == 0, "patch site must be DWORD aligned");

volatile LONG g_state = 0; // 0 absent, 1 installed, 2 transition in progress

struct ExchangeOutcome
{
	Result result;
	LONG final_dword;
};

bool IsExpectedExecutable(std::uint8_t *base)
{
	__try {
		auto dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(base);
		if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0 ||
			dos->e_lfanew > 0x100000)
			return false;
		auto nt = reinterpret_cast<const IMAGE_NT_HEADERS64 *>(base + dos->e_lfanew);
		return nt->Signature == IMAGE_NT_SIGNATURE &&
			nt->FileHeader.Machine == IMAGE_FILE_MACHINE_AMD64 &&
			nt->OptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC &&
			nt->FileHeader.TimeDateStamp == kExpectedTimestamp &&
			nt->OptionalHeader.SizeOfImage == kExpectedSizeOfImage &&
			kTaaSetterRva + sizeof(LONG) <= nt->OptionalHeader.SizeOfImage;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return false;
	}
}

LONG ReadSite(volatile LONG *site)
{
	// Naturally aligned 32-bit reads are atomic on x64.  Do not use an
	// Interlocked read here: CMPXCHG requires write access and the code page is RX.
	return *site;
}

bool SiteMatches(const volatile LONG *site, const std::uint8_t (&expected)[8])
{
	__try {
		return std::memcmp(const_cast<const LONG *>(site), expected,
			sizeof(expected)) == 0;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return false;
	}
}

bool RestoreProtection(volatile LONG *site, DWORD protection)
{
	DWORD ignored = 0;
	if (VirtualProtect(const_cast<LONG *>(site), sizeof(LONG), protection, &ignored))
		return true;
	// A transient failure must not be silently treated as success.
	return VirtualProtect(const_cast<LONG *>(site), sizeof(LONG), protection, &ignored) != FALSE;
}

bool RollBackSite(volatile LONG *site, LONG replacement, LONG expected,
	DWORD original_protection)
{
	DWORD current_protection = 0;
	if (!VirtualProtect(const_cast<LONG *>(site), sizeof(LONG),
		PAGE_EXECUTE_READWRITE, &current_protection))
		return false;

	LONG observed = InterlockedCompareExchange(site, expected, replacement);
	bool bytes_restored = observed == replacement || observed == expected;
	bool cache_flushed = bytes_restored &&
		FlushInstructionCache(GetCurrentProcess(), const_cast<LONG *>(site),
			sizeof(LONG)) != FALSE;
	bool protection_restored = RestoreProtection(site, original_protection);
	return bytes_restored && cache_flushed && protection_restored &&
		ReadSite(site) == expected;
}

ExchangeOutcome ExchangeSite(volatile LONG *site, LONG expected, LONG replacement)
{
	DWORD old_protection = 0;
	if (!VirtualProtect(const_cast<LONG *>(site), sizeof(LONG),
		PAGE_EXECUTE_READWRITE, &old_protection))
		return { Result::ProtectFailed, ReadSite(site) };

	LONG observed = InterlockedCompareExchange(site, replacement, expected);
	if (observed != expected) {
		bool protection_restored = RestoreProtection(site, old_protection);
		return {
			protection_restored ? Result::ConcurrentChange
				: Result::ProtectionRestoreFailed,
			ReadSite(site),
		};
	}

	bool cache_flushed = FlushInstructionCache(GetCurrentProcess(),
		const_cast<LONG *>(site), sizeof(LONG)) != FALSE;
	bool protection_restored = RestoreProtection(site, old_protection);
	LONG final_dword = ReadSite(site);
	if (cache_flushed && protection_restored && final_dword == replacement)
		return {
			replacement == kPatchedDword ? Result::Installed : Result::Removed,
			final_dword,
		};
	if (cache_flushed && protection_restored)
		return { Result::ConcurrentChange, final_dword };

	Result original_failure = cache_flushed
		? Result::ProtectionRestoreFailed : Result::CacheFlushFailed;
	if (RollBackSite(site, replacement, expected, old_protection))
		return { original_failure, ReadSite(site) };
	return { Result::RollbackFailed, ReadSite(site) };
}

void SetStateFromBytes(LONG value)
{
	if (value == kOriginalDword)
		InterlockedExchange(&g_state, 0);
	else if (value == kPatchedDword)
		InterlockedExchange(&g_state, 1);
	else
		InterlockedExchange(&g_state, 2);
}
}

Result Install()
{
	LONG previous = InterlockedCompareExchange(&g_state, 2, 0);
	if (previous == 1) {
		auto base = reinterpret_cast<std::uint8_t *>(GetModuleHandleW(nullptr));
		if (!base || !IsExpectedExecutable(base))
			return Result::UnsupportedExecutable;
		auto site = reinterpret_cast<volatile LONG *>(base + kTaaSetterRva);
		return SiteMatches(site, kExpectedPatchedSite)
			? Result::AlreadyInstalled : Result::UnexpectedBytes;
	}
	if (previous != 0)
		return Result::ConcurrentChange;

	auto base = reinterpret_cast<std::uint8_t *>(GetModuleHandleW(nullptr));
	if (!base || !IsExpectedExecutable(base)) {
		InterlockedExchange(&g_state, 0);
		return Result::UnsupportedExecutable;
	}

	auto site = reinterpret_cast<volatile LONG *>(base + kTaaSetterRva);
	if (!SiteMatches(site, kExpectedSite) || ReadSite(site) != kOriginalDword) {
		InterlockedExchange(&g_state, 0);
		return Result::UnexpectedBytes;
	}

	ExchangeOutcome outcome = ExchangeSite(site, kOriginalDword, kPatchedDword);
	SetStateFromBytes(outcome.final_dword);
	return outcome.result;
}

Result Remove()
{
	LONG previous = InterlockedCompareExchange(&g_state, 2, 1);
	if (previous == 0)
		return Result::NotInstalled;
	if (previous != 1)
		return Result::ConcurrentChange;

	auto base = reinterpret_cast<std::uint8_t *>(GetModuleHandleW(nullptr));
	if (!base || !IsExpectedExecutable(base)) {
		InterlockedExchange(&g_state, 1);
		return Result::UnsupportedExecutable;
	}

	auto site = reinterpret_cast<volatile LONG *>(base + kTaaSetterRva);
	if (!SiteMatches(site, kExpectedPatchedSite) || ReadSite(site) != kPatchedDword) {
		InterlockedExchange(&g_state, 1);
		return Result::UnexpectedBytes;
	}

	ExchangeOutcome outcome = ExchangeSite(site, kPatchedDword, kOriginalDword);
	SetStateFromBytes(outcome.final_dword);
	return outcome.result;
}

const wchar_t *Describe(Result result)
{
	switch (result) {
	case Result::Installed: return L"TAA lock installed";
	case Result::AlreadyInstalled: return L"TAA lock already installed";
	case Result::Removed: return L"TAA lock removed";
	case Result::NotInstalled: return L"TAA lock is not installed";
	case Result::UnsupportedExecutable: return L"unsupported DiRT Rally 2.0 executable";
	case Result::UnexpectedBytes: return L"TAA setter bytes do not match";
	case Result::ProtectFailed: return L"could not change TAA setter page protection";
	case Result::CacheFlushFailed: return L"instruction cache flush failed; patch rolled back";
	case Result::ProtectionRestoreFailed: return L"page protection restoration failed; abort launch";
	case Result::RollbackFailed: return L"TAA patch rollback incomplete; abort launch";
	case Result::ConcurrentChange: return L"TAA setter changed concurrently";
	default: return L"unknown TAA lock result";
	}
}
}
