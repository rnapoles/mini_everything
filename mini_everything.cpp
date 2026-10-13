// mini_everything.cpp
// Compile with: cl /EHsc /W4 mini_everything.cpp
// Or with MSYS/MinGW: g++ -std=c++17 -municode mini_everything.cpp -lntdll -o mini_everything.exe
//
// Run as Administrator.

#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <winioctl.h>
#include <iostream>
#include <string>
#include <vector>
#include <unordered_map>
#include <algorithm>
#include <locale>
#include <codecvt>
#include <sstream>
#include <clocale>
#include <cwctype>
#include <iostream>

using ull = unsigned long long;
using FRN = unsigned long long; // File Reference Number (64-bit)

struct Entry {
    FRN frn;
    FRN parentFrn;
    std::wstring name; // filename only, not full path
    bool isDirectory;
};

static std::wstring to_wlower(const std::wstring& s) {
    std::wstring out = s;
    std::transform(out.begin(), out.end(), out.begin(), [](wchar_t c) { return std::towlower(c); });
    return out;
}

std::wstring GetLastErrorMessage(DWORD error = GetLastError()) {
    LPWSTR buffer = nullptr;

    DWORD size = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER |
        FORMAT_MESSAGE_FROM_SYSTEM |
        FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr,
        error,
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        (LPWSTR)&buffer,
        0,
        nullptr
    );

    std::wstring message;
    if (size > 0 && buffer) {
        message.assign(buffer, size);

        // Remove trailing CR/LF that FormatMessage usually adds
        while (!message.empty() && (message.back() == L'\r' || message.back() == L'\n')) {
            message.pop_back();
        }

        LocalFree(buffer);
    } else {
        message = L"Unknown error (" + std::to_wstring(error) + L")";
    }

    return message;
}

// Read all USN_RECORDs from the volume (major work done here)
bool enumerate_volume(const wchar_t volumeLetter, std::unordered_map<FRN, Entry>& outEntries, std::wstring& errMsg) {
    // volume path like "\\.\C:"
    wchar_t volPath[8] = L"\\\\.\\C:";
    volPath[4] = volumeLetter;
    volPath[5] = L'\0';

    std::wstring devicePath = volPath;
    // Open volume
    HANDLE hVol = CreateFileW(devicePath.c_str(),
                              GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_WRITE,
                              nullptr,
                              OPEN_EXISTING,
                              0,
                              nullptr);
    if (hVol == INVALID_HANDLE_VALUE) {
        DWORD err = GetLastError();
        //std::wcerr << L"CreateFileW failed: " << GetLastErrorMessage(err) << L"\n";
        errMsg = L"CreateFileW failed for volume " + devicePath + L" (need Admin?) Code: " + GetLastErrorMessage(err);
        return false;
    }

    // Prepare MFT_ENUM_DATA structure
    MFT_ENUM_DATA_V0 med{};
    med.StartFileReferenceNumber = 0; // start at beginning
    med.LowUsn = 0;
    med.HighUsn = MAXLONGLONG;

    const DWORD outBufSize = 1 << 20; // 1MB buffer
    std::vector<BYTE> buffer(outBufSize);
    DWORD bytesReturned = 0;

    while (true) {
        BOOL ok = DeviceIoControl(hVol,
                                  FSCTL_ENUM_USN_DATA,
                                  &med,
                                  sizeof(med),
                                  buffer.data(),
                                  (DWORD)buffer.size(),
                                  &bytesReturned,
                                  nullptr);
        if (!ok) {
            DWORD err = GetLastError();
            if (err == ERROR_MORE_DATA) {
                // buffer too small (rare), increase and retry
                buffer.resize(buffer.size() * 2);
                continue;
            } else if (err == ERROR_HANDLE_EOF || err == ERROR_NO_MORE_FILES) {
                break; // done
            } else {
                // other error
                errMsg = L"FSCTL_ENUM_USN_DATA failed. Code: " + std::to_wstring(err);
                CloseHandle(hVol);
                return false;
            }
        }

        if (bytesReturned < sizeof(USN)) break;
        // first 8 bytes are USN start in buffer (per docs)
        BYTE* p = buffer.data();
        DWORD offset = sizeof(USN);

        while (offset < bytesReturned) {
            USN_RECORD* rec = (USN_RECORD*)(p + offset);
            // Safety: ensure we don't go beyond bytesReturned in variable-length name
            // USN_RECORD has variable length filename starting at FileNameOffset
            // Use FileNameLength / 2 to get WCHAR count.
            UINT nameLen = rec->FileNameLength / sizeof(WCHAR);
            const wchar_t* namePtr = (const wchar_t*)(((BYTE*)rec) + rec->FileNameOffset);
            std::wstring filename(namePtr, nameLen);

            Entry e;
            // FileReferenceNumber is 64-bit (stored as ULONGLONG)
            e.frn = rec->FileReferenceNumber;
            e.parentFrn = rec->ParentFileReferenceNumber;
            e.name = filename;
            e.isDirectory = !!(rec->FileAttributes & FILE_ATTRIBUTE_DIRECTORY);

            outEntries[e.frn] = std::move(e);

            // advance by RecordLength
            DWORD recLen = rec->RecordLength;
            if (recLen == 0) break; // safety
            offset += recLen;
        }

        // Set next start FRN for continuation
        // When DeviceIoControl returns, the buffer begins with the starting USN for the next call.
        // The call updates med.StartFileReferenceNumber automatically? Historically you set med.StartFileReferenceNumber
        // to the last returned FRN. Simpler: use the last record's FRN + 1 to continue.
        // Find last record's FRN:
        {
            // iterate last record quickly:
            DWORD off = sizeof(USN);
            FRN lastFrn = 0;
            while (off < bytesReturned) {
                USN_RECORD* rec = (USN_RECORD*)(p + off);
                lastFrn = rec->FileReferenceNumber;
                DWORD recLen = rec->RecordLength;
                if (recLen == 0) break;
                off += recLen;
            }
            if (lastFrn == 0) break;
            med.StartFileReferenceNumber = lastFrn + 1;
        }
    }

    CloseHandle(hVol);
    return true;
}

// Build full path for a FRN by walking parent chain. Cache results to avoid recomputing.
std::wstring build_full_path_for(FRN frn,
                                 std::unordered_map<FRN, Entry>& entries,
                                 std::unordered_map<FRN, std::wstring>& cache) {
    auto itc = cache.find(frn);
    if (itc != cache.end()) return itc->second;

    std::vector<std::wstring> parts;
    FRN cur = frn;
    int safety = 0;
    while (true) {
        if (safety++ > 10000) break; // prevent infinite loop in malformed data
        auto it = entries.find(cur);
        if (it == entries.end()) {
            // unknown - break
            break;
        }
        parts.push_back(it->second.name);
        FRN parent = it->second.parentFrn;
        if (parent == cur) break; // root or self-parented
        // check if cached
        auto cit = cache.find(parent);
        if (cit != cache.end()) {
            // append cached parent path parts
            std::wstring parentPath = cit->second;
            // build final path by reversing parts and appending to parentPath
            std::wstringstream ss;
            ss << parentPath;
            for (int i = (int)parts.size() - 1; i >= 0; --i) {
                ss << L"\\" << parts[i];
            }
            std::wstring full = ss.str();
            cache[frn] = full;
            return full;
        }
        cur = parent;
    }

    // if nothing cached, build with drive root assumed (we don't have volume letter here)
    std::wstringstream ss;
    // Put a leading backslash to indicate absolute-ish: "\path\to\file"
    for (int i = (int)parts.size() - 1; i >= 0; --i) {
        ss << L"\\" << parts[i];
    }
    std::wstring full = ss.str();
    cache[frn] = full;
    return full;
}

// Simple search functions
std::vector<std::wstring> search_entries_prefix(const std::wstring& queryLower,
                                                std::unordered_map<FRN, Entry>& entries,
                                                std::unordered_map<FRN, std::wstring>& cache,
                                                size_t maxResults = 200) {
    std::vector<std::wstring> results;
    results.reserve(256);
    for (auto &kv : entries) {
        const Entry &e = kv.second;
        std::wstring nameLower = to_wlower(e.name);
        if (nameLower.rfind(queryLower, 0) == 0) { // starts_with
            results.push_back(build_full_path_for(e.frn, entries, cache));
            if (results.size() >= maxResults) break;
        }
    }
    return results;
}

std::vector<std::wstring> search_entries_contains(const std::wstring& queryLower,
                                                  std::unordered_map<FRN, Entry>& entries,
                                                  std::unordered_map<FRN, std::wstring>& cache,
                                                  size_t maxResults = 200) {
    std::vector<std::wstring> results;
    results.reserve(256);
    for (auto &kv : entries) {
        const Entry &e = kv.second;
        std::wstring nameLower = to_wlower(e.name);
        if (nameLower.find(queryLower) != std::wstring::npos) {
            results.push_back(build_full_path_for(e.frn, entries, cache));
            if (results.size() >= maxResults) break;
        }
    }
    return results;
}

// Print basic stats
void print_stats(const std::unordered_map<FRN, Entry>& entries) {
    size_t totalFiles = 0, totalDirs = 0;
    for (auto &kv : entries) {
        if (kv.second.isDirectory) ++totalDirs;
        else ++totalFiles;
    }
    std::wcout << L"Indexed entries: " << entries.size() << L" (files: " << totalFiles << L", dirs: " << totalDirs << L")\n";
}

int wmain(int argc, wchar_t* argv[]) {
    std::locale::global(std::locale(""));
    std::wcout.imbue(std::locale());
    std::wcin.imbue(std::locale());

    if (argc < 2) {
        std::wcout << L"Usage: mini_everything <VolumeLetter>\n";
        std::wcout << L"Example: mini_everything C\n";
        return 1;
    }
    wchar_t vol = argv[1][0];
    if (!iswalpha(vol)) {
        std::wcerr << L"Invalid volume letter\n";
        return 1;
    }
    vol = towupper(vol);

    std::wcout << L"Indexing volume " << vol << L":\\ (this may take a few seconds)...\n";

    std::unordered_map<FRN, Entry> entries;
    std::wstring err;
    bool ok = enumerate_volume(vol, entries, err);
    if (!ok) {
        std::wcerr << L"Failed to enumerate volume: " << err << L"\n";
        return 2;
    }

    // Build a few cached roots — we don't have explicit drive root FRN. Some parent chains end at 0.
    // We'll set cache[0] = "X:" to prepend drive letter when building final paths.
    std::unordered_map<FRN, std::wstring> cache;
    std::wstring driveRoot;
    {
        std::wstringstream s;
        s << vol << L":";
        driveRoot = s.str();
    }
    cache[0] = driveRoot;

    print_stats(entries);

    // Provide REPL for searches
    std::wcout << L"Type a query and press Enter. Commands:\n";
    std::wcout << L"  :q       Quit\n";
    std::wcout << L"  prefix:* Search prefix\n";
    std::wcout << L"  *suffix  Simple wildcard (prefix or contains)\n";
    std::wcout << L"Examples: 'win' (prefix), '*log' (suffix), '*report*' (contains)\n";

    std::wstring line;
    while (true) {
        std::wcout << L"> ";
        if (!std::getline(std::wcin, line)) break;
        if (line.size() == 0) continue;
        if (line == L":q" || line == L"quit" || line == L"exit") break;

        bool containsWildcard = false;
        bool prefixSearch = false;
        std::wstring q = line;
        // trim spaces
        while (!q.empty() && iswspace(q.front())) q.erase(q.begin());
        while (!q.empty() && iswspace(q.back())) q.pop_back();
        if (q.empty()) continue;

        // Simple wildcard handling: if q starts and ends with '*', do contains.
        if (q.front() == L'*' && q.back() == L'*' && q.size() >= 3) {
            containsWildcard = true;
            q = q.substr(1, q.size()-2);
        } else if (q.front() == L'*' && q.size() >= 2) {
            // suffix search: name endswith q.substr(1) -> we implement as contains (cheap).
            containsWildcard = true;
            q = q.substr(1);
        } else if (q.back() == L'*' && q.size() >= 2) {
            // prefix search
            prefixSearch = true;
            q = q.substr(0, q.size()-1);
        } else {
            // default: prefix search (like Everything)
            prefixSearch = true;
        }

        std::wstring qLower = to_wlower(q);

        std::vector<std::wstring> results;
        if (containsWildcard) {
            results = search_entries_contains(qLower, entries, cache, 500);
        } else if (prefixSearch) {
            results = search_entries_prefix(qLower, entries, cache, 500);
        } else {
            results = search_entries_prefix(qLower, entries, cache, 500);
        }

        std::wcout << L"Found: " << results.size() << L" (showing up to 500)\n";
        for (size_t i = 0; i < results.size(); ++i) {
            // Prepend drive letter if path doesn't already have it (we used cache[0] = "C:" as root)
            std::wstring p = results[i];
            if (!p.empty() && p[0] == L'\\') {
                p = driveRoot + p;
            }
            std::wcout << p << L"\n";
        }
    }

    std::wcout << L"Goodbye\n";
    return 0;
}
