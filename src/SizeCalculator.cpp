#include "SizeCalculator.h"
#include <cwchar>
#include <algorithm>
#include <processthreadsapi.h>

SizeCalculator::SizeCalculator(UpdateCallback onUpdateCallback)
    : m_onUpdate(std::move(onUpdateCallback)) {
    // 2 background threads saturate NVMe directory metadata scans while minimizing thread stack RAM
    constexpr size_t workerCount = 2;
    m_workers.reserve(workerCount);
    for (size_t i = 0; i < workerCount; ++i) {
        m_workers.emplace_back(&SizeCalculator::WorkerLoop, this);
    }
}

SizeCalculator::SizeCalculator(PathUpdateCallback onPathUpdateCallback)
    : m_onPathUpdate(std::move(onPathUpdateCallback)) {
    constexpr size_t workerCount = 2;
    m_workers.reserve(workerCount);
    for (size_t i = 0; i < workerCount; ++i) {
        m_workers.emplace_back(&SizeCalculator::WorkerLoop, this);
    }
}

SizeCalculator::~SizeCalculator() {
    m_stop.store(true, std::memory_order_release);
    m_cv.notify_all();
    for (auto& t : m_workers) {
        if (t.joinable()) {
            t.join();
        }
    }
}

uint64_t SizeCalculator::GetFolderWriteTime(const std::wstring& folderPath) {
    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (GetFileAttributesExW(folderPath.c_str(), GetFileExInfoStandard, &fad)) {
        ULARGE_INTEGER uli{};
        uli.LowPart = fad.ftLastWriteTime.dwLowDateTime;
        uli.HighPart = fad.ftLastWriteTime.dwHighDateTime;
        return uli.QuadPart;
    }
    return 0;
}

std::wstring SizeCalculator::ToLongPath(const std::wstring& path) {
    if (path.rfind(L"\\\\?\\", 0) == 0) {
        return path;
    }
    if (path.rfind(L"\\\\", 0) == 0) {
        return L"\\\\?\\UNC\\" + path.substr(2);
    }
    return L"\\\\?\\" + path;
}

FolderSizeResult SizeCalculator::RequestFolderSize(const std::wstring& folderPath) {
    // Ignore UNC network paths (\\server\share) to prevent network stalls
    if (folderPath.size() >= 2 && folderPath[0] == L'\\' && folderPath[1] == L'\\') {
        if (folderPath.rfind(L"\\\\?\\", 0) != 0) {
            return FolderSizeResult{ 0, CalculationStatus::Error, L"" };
        }
    }

    const ULONGLONG now = GetTickCount64();

    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_cache.find(folderPath);
    if (it != m_cache.end()) {
        CacheEntry& entry = it->second;
        if (entry.status == CalculationStatus::Ready) {
            if ((now - entry.calculatedTick) < kCacheTtlMs) {
                // Pure RAM lookup (~20ns, 0 disk syscalls on caller UI thread)
                return FolderSizeResult{ entry.sizeBytes, CalculationStatus::Ready, entry.formattedText };
            }
            // TTL expired: queue background re-check while returning cached size immediately
            entry.status = CalculationStatus::Calculating;
            if (std::find(m_queue.begin(), m_queue.end(), folderPath) == m_queue.end()) {
                m_queue.push_front(folderPath);
                if (m_queue.size() > 128) {
                    m_queue.pop_back();
                }
                m_cv.notify_one();
            }
            return FolderSizeResult{ entry.sizeBytes, CalculationStatus::Ready, entry.formattedText };
        }
        return FolderSizeResult{ entry.sizeBytes, entry.status, entry.formattedText };
    }

    if (m_cache.size() >= kMaxCacheEntries) {
        m_cache.clear();
    }

    CacheEntry newEntry{};
    newEntry.sizeBytes = 0;
    newEntry.status = CalculationStatus::Calculating;
    newEntry.formattedText = L"...";
    newEntry.folderWriteTime = 0;
    newEntry.calculatedTick = now;
    m_cache.emplace(folderPath, newEntry);

    if (std::find(m_queue.begin(), m_queue.end(), folderPath) == m_queue.end()) {
        m_queue.push_front(folderPath);
        if (m_queue.size() > 128) {
            m_queue.pop_back();
        }
        m_cv.notify_one();
    }

    return FolderSizeResult{ 0, CalculationStatus::Calculating, L"..." };
}

void SizeCalculator::ClearCache() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_cache.clear();
    m_queue.clear();
    m_queue.shrink_to_fit();
}

void SizeCalculator::WorkerLoop() {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);

    PROCESS_POWER_THROTTLING_STATE powerThrottling{};
    powerThrottling.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
    powerThrottling.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
    powerThrottling.StateMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
    SetThreadInformation(GetCurrentThread(), ThreadPowerThrottling, &powerThrottling, sizeof(powerThrottling));

    while (!m_stop.load(std::memory_order_acquire)) {
        std::wstring targetPath;
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_cv.wait(lock, [this]() {
                return m_stop.load(std::memory_order_acquire) || !m_queue.empty();
            });
            if (m_stop.load(std::memory_order_acquire)) {
                return;
            }
            targetPath = std::move(m_queue.front());
            m_queue.pop_front();
        }

        const uint64_t currentWriteTime = GetFolderWriteTime(targetPath);
        bool canSkip = false;
        uint64_t totalBytes = 0;
        std::wstring formatted;

        {
            std::lock_guard<std::mutex> lock(m_mutex);
            auto it = m_cache.find(targetPath);
            if (it != m_cache.end() && it->second.sizeBytes > 0 && it->second.folderWriteTime != 0 && it->second.folderWriteTime == currentWriteTime) {
                canSkip = true;
                totalBytes = it->second.sizeBytes;
                formatted = it->second.formattedText;
                it->second.status = CalculationStatus::Ready;
                it->second.calculatedTick = GetTickCount64();
            }
        }

        if (!canSkip) {
            totalBytes = ComputeDirectorySize(targetPath);
            formatted = FormatSize(totalBytes);
            const uint64_t updatedWriteTime = GetFolderWriteTime(targetPath);

            {
                std::lock_guard<std::mutex> lock(m_mutex);
                auto it = m_cache.find(targetPath);
                if (it != m_cache.end()) {
                    it->second.sizeBytes = totalBytes;
                    it->second.status = CalculationStatus::Ready;
                    it->second.formattedText = formatted;
                    it->second.folderWriteTime = updatedWriteTime;
                    it->second.calculatedTick = GetTickCount64();
                }
            }
        }

        if (!m_stop.load(std::memory_order_acquire)) {
            if (m_onPathUpdate) {
                m_onPathUpdate(targetPath);
            }
            if (m_onUpdate) {
                m_onUpdate();
            }
        }
    }
}

uint64_t SizeCalculator::ComputeDirectorySize(const std::wstring& rootPath) const {
    uint64_t totalSize = 0;
    std::vector<std::wstring> dirStack;
    dirStack.reserve(64);
    dirStack.push_back(ToLongPath(rootPath));

    WIN32_FIND_DATAW findData{};

    while (!dirStack.empty() && !m_stop.load(std::memory_order_relaxed)) {
        std::wstring currentDir = std::move(dirStack.back());
        dirStack.pop_back();

        if (!currentDir.empty() && currentDir.back() != L'\\') {
            currentDir.push_back(L'\\');
        }

        std::wstring searchPattern = currentDir + L"*";
        HANDLE hFind = FindFirstFileExW(
            searchPattern.c_str(),
            FindExInfoBasic,
            &findData,
            FindExSearchNameMatch,
            nullptr,
            FIND_FIRST_EX_LARGE_FETCH);

        if (hFind == INVALID_HANDLE_VALUE) {
            continue;
        }

        do {
            const wchar_t* name = findData.cFileName;
            if (name[0] == L'.' && (name[1] == L'\0' || (name[1] == L'.' && name[2] == L'\0'))) {
                continue;
            }

            const DWORD attrs = findData.dwFileAttributes;
            // Skip reparse points, offline files, and cloud placeholder recall attributes (OneDrive / Dropbox)
            if ((attrs & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_OFFLINE | 0x00040000 /*FILE_ATTRIBUTE_RECALL_ON_OPEN*/ | 0x00400000 /*FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS*/)) != 0) {
                continue;
            }

            if ((attrs & FILE_ATTRIBUTE_DIRECTORY) != 0) {
                dirStack.push_back(currentDir + name);
            } else {
                ULARGE_INTEGER fileSize{};
                fileSize.LowPart = findData.nFileSizeLow;
                fileSize.HighPart = findData.nFileSizeHigh;
                totalSize += fileSize.QuadPart;
            }
        } while (FindNextFileW(hFind, &findData) != 0);

        FindClose(hFind);
    }

    return totalSize;
}

std::wstring SizeCalculator::FormatSize(uint64_t bytes) {
    wchar_t buf[64]{};
    const double kb = 1024.0;
    const double mb = kb * 1024.0;
    const double gb = mb * 1024.0;
    const double tb = gb * 1024.0;

    if (bytes >= static_cast<uint64_t>(tb)) {
        swprintf_s(buf, L"%.2f TB", static_cast<double>(bytes) / tb);
    } else if (bytes >= static_cast<uint64_t>(gb)) {
        swprintf_s(buf, L"%.2f GB", static_cast<double>(bytes) / gb);
    } else if (bytes >= static_cast<uint64_t>(mb)) {
        swprintf_s(buf, L"%.1f MB", static_cast<double>(bytes) / mb);
    } else if (bytes >= static_cast<uint64_t>(kb)) {
        swprintf_s(buf, L"%.0f KB", static_cast<double>(bytes) / kb);
    } else {
        swprintf_s(buf, L"%llu B", static_cast<unsigned long long>(bytes));
    }
    return std::wstring(buf);
}
