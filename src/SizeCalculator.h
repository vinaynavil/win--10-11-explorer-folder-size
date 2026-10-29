#pragma once
#include <windows.h>
#include <string>
#include <cstdint>
#include <functional>

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

    explicit SizeCalculator(UpdateCallback onUpdateCallback);
    ~SizeCalculator();

    FolderSizeResult RequestFolderSize(const std::wstring& folderPath);
    void ClearCache();
    static std::wstring FormatSize(uint64_t bytes);

private:
    UpdateCallback m_onUpdate;
};
