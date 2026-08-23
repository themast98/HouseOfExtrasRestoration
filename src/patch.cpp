#include "patch.h"
#include "log.h"
#include <windows.h>
#include <string.h>

namespace hoe {

uintptr_t Base() {
    return reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
}

size_t ImageSize() {
    auto b = Base();
    auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(b);
    auto nt  = reinterpret_cast<IMAGE_NT_HEADERS64*>(b + dos->e_lfanew);
    return nt->OptionalHeader.SizeOfImage;
}

bool WriteBytes(uintptr_t rva, const void* data, size_t n) {
    void* addr = reinterpret_cast<void*>(Base() + rva);
    DWORD old = 0;
    if (!VirtualProtect(addr, n, PAGE_EXECUTE_READWRITE, &old)) return false;
    memcpy(addr, data, n);
    DWORD tmp = 0;
    VirtualProtect(addr, n, old, &tmp);
    FlushInstructionCache(GetCurrentProcess(), addr, n);
    return true;
}

bool Patch::Apply(uintptr_t rva, const void* data, size_t n, const char* label) {
    if (applied_ || n > sizeof(orig_)) return false;
    memcpy(orig_, reinterpret_cast<void*>(Base() + rva), n);
    if (!WriteBytes(rva, data, n)) {
        Log("patch FAILED: %s at +0x%llX", label, (unsigned long long)rva);
        return false;
    }
    rva_ = rva; len_ = n; label_ = label; applied_ = true;
    Log("patched %s at +0x%llX (%zu bytes)", label, (unsigned long long)rva, n);
    return true;
}

bool Patch::Revert() {
    if (!applied_) return false;
    bool ok = WriteBytes(rva_, orig_, len_);
    if (ok) { applied_ = false; Log("reverted %s", label_); }
    return ok;
}

}  // namespace hoe
