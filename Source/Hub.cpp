#include "Hub.h"
#include <chrono>
#include <cstdlib>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace clr {

uint64_t Hub::nowNs()
{
    return (uint64_t) std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}

bool Hub::open()
{
    if (data) return true;
    if (const char* e = std::getenv("CLEARING_HUB")) useName(e); // tests use a private segment
    fd = shm_open(segName, O_CREAT | O_RDWR, 0666);
    if (fd < 0) return false;
    struct stat st {};
    if (fstat(fd, &st) == 0 && (size_t) st.st_size < sizeof(HubData))
        if (ftruncate(fd, (off_t) sizeof(HubData)) != 0) { ::close(fd); fd = -1; return false; }
    void* p = mmap(nullptr, sizeof(HubData), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (p == MAP_FAILED) { ::close(fd); fd = -1; return false; }
    data = static_cast<HubData*>(p);
    uint32_t expected = 0;
    // First mapper stamps the segment; the kernel zero-fills new pages.
    if (data->magic.compare_exchange_strong(expected, kHubMagic)) {
        for (auto& s : data->slots) { s.envDb.store(-120.0f); }
    } else if (expected != kHubMagic) { // someone else's data: refuse
        munmap(data, sizeof(HubData)); data = nullptr; ::close(fd); fd = -1;
        return false;
    }
    return true;
}

void Hub::close()
{
    if (data) { munmap(data, sizeof(HubData)); data = nullptr; }
    if (fd >= 0) { ::close(fd); fd = -1; }
}

bool Hub::live(int slot) const
{
    if (!data || slot < 0 || slot >= kSlots) return false;
    const auto& s = data->slots[slot];
    const uint64_t o = s.owner.load();
    return o != 0 && nowNs() - s.heartbeatNs.load() < kStaleNs;
}

int Hub::claim(uint64_t id)
{
    if (!data) return -1;
    const uint64_t now = nowNs();
    for (int i = 0; i < kSlots; ++i) {
        auto& s = data->slots[i];
        uint64_t o = 0;
        if (s.owner.compare_exchange_strong(o, id)) { s.heartbeatNs.store(now); return i; }
    }
    // None free: take over an abandoned one (an instance that was killed).
    for (int i = 0; i < kSlots; ++i) {
        auto& s = data->slots[i];
        uint64_t o = s.owner.load();
        if (o != 0 && now - s.heartbeatNs.load() > kStaleNs * 3 && s.owner.compare_exchange_strong(o, id)) {
            s.heartbeatNs.store(now); return i;
        }
    }
    return -1;
}

int Hub::find(uint64_t uid) const
{
    if (!data || uid == 0) return -1;
    for (int i = 0; i < kSlots; ++i)
        if (data->slots[i].owner.load() == uid && live(i)) return i;
    return -1;
}

void Hub::release(int slot, uint64_t id)
{
    if (!data || slot < 0) return;
    uint64_t o = id;
    data->slots[slot].owner.compare_exchange_strong(o, 0);
}

} // namespace clr
