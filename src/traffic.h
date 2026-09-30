#pragma once

#include <string>
#include <vector>

// Cumulative byte counters of one network adapter or drive; the caller turns two readings into a rate.
struct TrafficCounter {
    std::wstring key;         // stable identity between readings
    std::wstring name;        // adapter description or drive name
    std::wstring kind;        // "Network adapter" or "Drive"
    std::wstring instanceId;  // device instance ID (drives only)
    std::wstring detail;      // "link 1000 Mbit/s", "not connected", "bus: USB", ...
    unsigned long long received = 0;  // network: bytes received; drive: bytes read
    unsigned long long sent = 0;      // network: bytes sent; drive: bytes written
    bool connected = true;            // network adapters: media connected
};

// Physical network adapters (GetIfTable2) and disks (IOCTL_DISK_PERFORMANCE); no administrator rights needed.
std::vector<TrafficCounter> ReadTrafficCounters();
