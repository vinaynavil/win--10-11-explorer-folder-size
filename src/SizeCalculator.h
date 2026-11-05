#pragma once
#include <windows.h>
#include <string>
#include <cstdint>
#include <functional>
#include <unordered_map>
#include <deque>
#include <vector>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>

enum class CalculationStatus {
    Pending,
    Calculating,
    Ready,
    Error
};

struct FolderSizeResult {
    uint64_t sizeBytes = 0;
    CalculationStatus status = CalculationStatus::Pending;
    std::wstring formattedText;
};

class SizeCalculator {
public:
    using UpdateCallback = std::function<void()>;
    using PathUpdateCallback = std::function<void(const std::wstring&)>;

    explicit SizeCalculator(UpdateCallback onUpdateCallback);
    explicit SizeCalculator(PathUpdateCallback onPathUpdateCallback);
    ~SizeCalculator();

    FolderSizeResult RequestFolderSize(const std::wstring& folderPath);
    void ClearCache();
    static std::wstring FormatSize(uint64_t bytes);

private:
    struct CacheEntry {
        uint64_t sizeBytes = 0;
        CalculationStatus status = CalculationStatus::Pending;
        std::wstring formattedText;
        uint64_t folderWriteTime = 0;
        ULONGLONG calculatedTick = 0;
    };

    void WorkerLoop();
    uint64_t ComputeDirectorySize(const std::wstring& rootPath) const;
    static uint64_t GetFolderWriteTime(const std::wstring& folderPath);
    static std::wstring ToLongPath(const std::wstring& path);

    UpdateCallback m_onUpdate;
    PathUpdateCallback m_onPathUpdate;
    mutable std::mutex m_mutex;
    std::condition_variable m_cv;
    std::unordered_map<std::wstring, CacheEntry> m_cache;
    std::deque<std::wstring> m_queue;
    std::vector<std::thread> m_workers;
    std::atomic<bool> m_stop{ false };

    static constexpr size_t kMaxCacheEntries = 4096;
    static constexpr ULONGLONG kCacheTtlMs = 120000; // 2 minutes TTL before background re-check
};
