// Two real processes share one hub: the child claims a slot and publishes; the parent reads it.
#include "Hub.h"
#include <cstdio>
#include <cstdlib>
#include <sys/mman.h>
#include <sys/wait.h>
#include <string>
#include <unistd.h>
using namespace clr;
int main()
{
    setenv("CLEARING_HUB", "/clearing_hub_selftest", 1);
    int fails = 0;
    { Hub h; h.open(); shm_unlink("/clearing_hub_selftest"); }
    pid_t pid = fork();
    if (pid == 0) {
        Hub h; if (!h.open()) _exit(2);
        int s = h.claim(0xABCDEF);
        auto& sl = h.get()->slots[s];
        seqWrite(sl.measSeq, [&] { std::snprintf(sl.name, sizeof sl.name, "Child Bass"); sl.role = 2; sl.cap[5] = -42.5f; });
        sl.heartbeatNs.store(Hub::nowNs());
        usleep(400000);
        _exit(0);
    }
    usleep(150000);
    Hub h; if (!h.open()) { std::printf("FAIL open\n"); return 1; }
    bool found = false;
    for (int i = 0; i < kSlots; ++i) {
        if (!h.live(i)) continue;
        auto& sl = h.get()->slots[i];
        char name[24]; float c5 = 0; int role = -1;
        if (seqRead(sl.measSeq, [&] { std::memcpy(name, sl.name, 24); c5 = sl.cap[5]; role = sl.role; }) && std::string(name) == "Child Bass" && c5 == -42.5f && role == 2) found = true;
    }
    if (!found) { std::printf("FAIL: parent did not see the child's slot across processes\n"); ++fails; }
    int st = 0; waitpid(pid, &st, 0);
    if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) { std::printf("FAIL child exit\n"); ++fails; }
    // After the child is gone its heartbeat goes stale.
    usleep(100000);
    shm_unlink("/clearing_hub_selftest");
    std::printf("hub cross-process check: %s\n", fails ? "FAILED" : "ok");
    return fails;
}
