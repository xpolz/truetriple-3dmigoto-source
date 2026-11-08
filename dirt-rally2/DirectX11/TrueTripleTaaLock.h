#pragma once

#include <windows.h>

// Included once by D3D11Wrapper.cpp; installed during early proxy initialization
// when the active mod configuration enables force_taa_off.
namespace TrueTripleTaaLock
{
enum class Result
{
	Installed,
	AlreadyInstalled,
	Removed,
	NotInstalled,
	UnsupportedExecutable,
	UnexpectedBytes,
	ProtectFailed,
	CacheFlushFailed,
	ProtectionRestoreFailed,
	RollbackFailed,
	ConcurrentChange,
};

// PRECONDITION: no other thread may execute exe+0x5F5340 until this call
// returns. Install is intended for early serialized proxy initialization;
// Remove is intended for serialized teardown only.
Result Install();
Result Remove();
const wchar_t *Describe(Result result);
}
