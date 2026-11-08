#pragma once

#include <windows.h>

// Included once by D3D11Wrapper.cpp; installed during early proxy initialization
// when the active mod configuration enables force_native_aa.
namespace TrueTripleCmaaLock
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

// PRECONDITION: execution of every patched RVA must be excluded for the entire
// call. Install belongs in early serialized proxy initialization; Remove only
// in serialized teardown. These functions are not hot-patch APIs.
Result Install();
Result Remove();
const wchar_t *Describe(Result result);
}
