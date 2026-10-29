#include "SizeCalculator.h"
#include <cwchar>

SizeCalculator::SizeCalculator(UpdateCallback onUpdateCallback)
    : m_onUpdate(std::move(onUpdateCallback)) {}

SizeCalculator::~SizeCalculator() = default;

FolderSizeResult SizeCalculator::RequestFolderSize(const std::wstring& /*folderPath*/) {
    // Phase 3 will implement the multi-threaded FindFirstFileExW worker pool & LRU cache
    return FolderSizeResult{ 0, CalculationStatus::Calculating, L"..." };
}

void SizeCalculator::ClearCache() {
    // Phase 3 will clear the thread-safe LRU cache
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
