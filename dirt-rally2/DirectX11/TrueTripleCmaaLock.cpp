#include "TrueTripleCmaaLock.h"

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace TrueTripleCmaaLock
{
namespace
{
constexpr DWORD kExpectedTimestamp = 0x605CBAE3;
constexpr DWORD kExpectedSizeOfImage = 0x02255000;
constexpr std::size_t kMaxPatchBytes = 4;

struct Site
{
	std::uintptr_t rva;
	std::size_t size;
	std::uint8_t original[kMaxPatchBytes];
	std::uint8_t patched[kMaxPatchBytes];
};

// Force the two-part native representation CMAA=(MSAA sample count 0, flag 1)
// in both the startup path and the ChangeMSAASettings receiver.
constexpr Site kSites[] = {
	{ 0x004735DA, 3, { 0x41, 0x8B, 0xFC }, { 0x33, 0xFF, 0x90 } },
	{ 0x004735D6, 2, { 0x8B, 0xF8 }, { 0x33, 0xFF } },
	{ 0x009C8DF1, 3, { 0xC6, 0x01, 0x00 }, { 0xC6, 0x01, 0x01 } },
	{ 0x009C8E5C, 3, { 0x0F, 0x94, 0xC0 }, { 0xB0, 0x01, 0x90 } },
	{ 0x0047E24C, 3, { 0x8B, 0x52, 0x10 }, { 0x31, 0xD2, 0x90 } },
	{ 0x0047E276, 4, { 0x0F, 0xB6, 0x53, 0x14 }, { 0xB2, 0x01, 0x90, 0x90 } },
};

volatile LONG g_state = 0; // 0 absent, 1 installed, 2 transition/unknown

bool IsExpectedExecutable(std::uint8_t *base)
{
	__try {
		auto dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(base);
		if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0 ||
			dos->e_lfanew > 0x100000)
			return false;
		auto nt = reinterpret_cast<const IMAGE_NT_HEADERS64 *>(base + dos->e_lfanew);
		if (nt->Signature != IMAGE_NT_SIGNATURE ||
			nt->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||
			nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
			nt->FileHeader.TimeDateStamp != kExpectedTimestamp ||
			nt->OptionalHeader.SizeOfImage != kExpectedSizeOfImage)
			return false;
		for (const Site &site : kSites)
			if (site.rva + site.size > nt->OptionalHeader.SizeOfImage)
				return false;
		return true;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return false;
	}
}

bool Matches(const std::uint8_t *base, const Site &site, bool patched)
{
	__try {
		return std::memcmp(base + site.rva,
			patched ? site.patched : site.original, site.size) == 0;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return false;
	}
}

bool AllMatch(const std::uint8_t *base, bool patched)
{
	for (const Site &site : kSites)
		if (!Matches(base, site, patched))
			return false;
	return true;
}

bool RestoreProtection(void *address, std::size_t size, DWORD protection)
{
	DWORD ignored = 0;
	if (VirtualProtect(address, size, protection, &ignored))
		return true;
	return VirtualProtect(address, size, protection, &ignored) != FALSE;
}

struct WriteOutcome
{
	Result result;
	bool changed;
};

bool RollBackOne(std::uint8_t *base, const Site &site, bool install_direction,
	DWORD original_protection)
{
	auto address = base + site.rva;
	const auto *before = install_direction ? site.patched : site.original;
	const auto *after = install_direction ? site.original : site.patched;
	DWORD ignored_current = 0;
	if (!VirtualProtect(address, site.size, PAGE_EXECUTE_READWRITE, &ignored_current))
		return false;

	bool known = std::memcmp(address, before, site.size) == 0 ||
		std::memcmp(address, after, site.size) == 0;
	if (known)
		std::memcpy(address, after, site.size);
	bool flushed = known && FlushInstructionCache(GetCurrentProcess(), address,
		site.size) != FALSE;
	bool restored = RestoreProtection(address, site.size, original_protection);
	return known && flushed && restored &&
		std::memcmp(address, after, site.size) == 0;
}

WriteOutcome WriteOne(std::uint8_t *base, const Site &site, bool install_direction)
{
	auto address = base + site.rva;
	const auto *before = install_direction ? site.original : site.patched;
	const auto *after = install_direction ? site.patched : site.original;
	DWORD old_protection = 0;
	if (!VirtualProtect(address, site.size, PAGE_EXECUTE_READWRITE, &old_protection))
		return { Result::ProtectFailed, false };

	if (std::memcmp(address, before, site.size) != 0) {
		bool restored = RestoreProtection(address, site.size, old_protection);
		return { restored ? Result::UnexpectedBytes
			: Result::ProtectionRestoreFailed, false };
	}

	std::memcpy(address, after, site.size);
	bool flushed = FlushInstructionCache(GetCurrentProcess(), address,
		site.size) != FALSE;
	bool restored = RestoreProtection(address, site.size, old_protection);
	bool exact = std::memcmp(address, after, site.size) == 0;
	if (flushed && restored && exact)
		return { install_direction ? Result::Installed : Result::Removed, true };

	Result first_failure = !flushed ? Result::CacheFlushFailed
		: !restored ? Result::ProtectionRestoreFailed : Result::ConcurrentChange;
	if (RollBackOne(base, site, install_direction, old_protection))
		return { first_failure, false };
	return { Result::RollbackFailed, exact };
}

bool RollBackCompleted(std::uint8_t *base, std::size_t count,
	bool install_direction)
{
	bool ok = true;
	for (std::size_t i = count; i > 0; --i) {
		const Site &site = kSites[i - 1];
		// WriteOne in the opposite direction has its own protection, flush and
		// rollback handling. Continue through every site even after one failure.
		WriteOutcome outcome = WriteOne(base, site, !install_direction);
		if (outcome.result != (install_direction ? Result::Removed : Result::Installed))
			ok = false;
	}
	return ok && AllMatch(base, !install_direction);
}

void SetStateFromSites(const std::uint8_t *base)
{
	if (AllMatch(base, false))
		InterlockedExchange(&g_state, 0);
	else if (AllMatch(base, true))
		InterlockedExchange(&g_state, 1);
	else
		InterlockedExchange(&g_state, 2);
}

Result Transition(bool install_direction)
{
	LONG wanted = install_direction ? 0 : 1;
	LONG previous = InterlockedCompareExchange(&g_state, 2, wanted);
	if (previous != wanted) {
		if (previous == 2)
			return Result::ConcurrentChange;
		auto existing_base = reinterpret_cast<std::uint8_t *>(GetModuleHandleW(nullptr));
		if (!existing_base || !IsExpectedExecutable(existing_base))
			return Result::UnsupportedExecutable;
		if (install_direction && previous == 1 && AllMatch(existing_base, true))
			return Result::AlreadyInstalled;
		if (!install_direction && previous == 0 && AllMatch(existing_base, false))
			return Result::NotInstalled;
		SetStateFromSites(existing_base);
		return Result::UnexpectedBytes;
	}

	auto base = reinterpret_cast<std::uint8_t *>(GetModuleHandleW(nullptr));
	if (!base || !IsExpectedExecutable(base)) {
		InterlockedExchange(&g_state, wanted);
		return Result::UnsupportedExecutable;
	}
	if (!AllMatch(base, !install_direction)) {
		SetStateFromSites(base);
		return Result::UnexpectedBytes;
	}

	for (std::size_t i = 0; i < _countof(kSites); ++i) {
		WriteOutcome outcome = WriteOne(base, kSites[i], install_direction);
		Result success = install_direction ? Result::Installed : Result::Removed;
		if (outcome.result == success)
			continue;

		bool rolled_back = RollBackCompleted(base, i, install_direction);
		SetStateFromSites(base);
		if (!rolled_back || outcome.result == Result::RollbackFailed)
			return Result::RollbackFailed;
		return outcome.result;
	}

	bool exact = AllMatch(base, install_direction);
	if (!exact) {
		bool rolled_back = RollBackCompleted(base, _countof(kSites), install_direction);
		SetStateFromSites(base);
		return rolled_back ? Result::ConcurrentChange : Result::RollbackFailed;
	}
	SetStateFromSites(base);
	return install_direction ? Result::Installed : Result::Removed;
}
}

Result Install()
{
	return Transition(true);
}

Result Remove()
{
	return Transition(false);
}

const wchar_t *Describe(Result result)
{
	switch (result) {
	case Result::Installed: return L"CMAA lock installed";
	case Result::AlreadyInstalled: return L"CMAA lock already installed";
	case Result::Removed: return L"CMAA lock removed";
	case Result::NotInstalled: return L"CMAA lock is not installed";
	case Result::UnsupportedExecutable: return L"unsupported DiRT Rally 2.0 executable";
	case Result::UnexpectedBytes: return L"CMAA lock bytes do not match";
	case Result::ProtectFailed: return L"could not change CMAA patch page protection";
	case Result::CacheFlushFailed: return L"instruction cache flush failed; changes rolled back";
	case Result::ProtectionRestoreFailed: return L"page protection restoration failed; abort launch";
	case Result::RollbackFailed: return L"CMAA transaction rollback incomplete; abort launch";
	case Result::ConcurrentChange: return L"CMAA patch sites changed concurrently";
	default: return L"unknown CMAA lock result";
	}
}
}
