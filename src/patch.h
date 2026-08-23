#pragma once
#include <stdint.h>
#include <stddef.h>

namespace hoe {

uintptr_t Base();
size_t    ImageSize();

// Reversible byte patch. Keeps the original bytes so Revert() can undo it,
// mirroring the Enable/Disable pattern used by Like A Brawler 8's NopPatch.
class Patch {
public:
    bool Apply(uintptr_t rva, const void* data, size_t n, const char* label);
    bool Revert();
    bool applied() const { return applied_; }
private:
    uintptr_t rva_ = 0;
    size_t    len_ = 0;
    unsigned char orig_[32] = {};
    bool      applied_ = false;
    const char* label_ = "";
};

bool WriteBytes(uintptr_t rva, const void* data, size_t n);

}  // namespace hoe
