#pragma once
#include <fstream>
#include <vector>
#include <string>
#include <algorithm>

// ADF is bytewise XOR, not a separate codec. Read/decrypt in large chunks,
// cancel obsolete workers before allocation/each chunk, and reject short reads.
template<class Byte, class Cancelled>
bool LoadDecryptedAdf(const std::string& path, std::vector<Byte>& output, Cancelled cancelled)
{
    if (cancelled()) return false;
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) return false;
    const std::streamoff length = file.tellg();
    if (length <= 0 || static_cast<unsigned long long>(length) > output.max_size() ||
        cancelled()) return false;
    file.seekg(0);
    try { output.resize(static_cast<size_t>(length)); }
    catch (...) { return false; }
    const size_t chunkSize = 1024 * 1024;
    for (size_t offset = 0; offset < output.size();) {
        if (cancelled()) { output.clear(); return false; }
        const size_t count = (std::min)(chunkSize, output.size() - offset);
        if (!file.read(reinterpret_cast<char*>(output.data() + offset),
            static_cast<std::streamsize>(count))) {
            output.clear(); return false;
        }
        for (size_t i = offset; i < offset + count; ++i) output[i] ^= 0x22;
        offset += count;
    }
    return !cancelled();
}
