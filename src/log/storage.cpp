#include "log/storage.h"

namespace faas {
namespace log {

Storage::Storage() {
}

Storage::~Storage() {
}

void Storage::Add(std::unique_ptr<LogEntry> log_entry) {
    uint64_t seqnum = log_entry->seqnum;
    entries_[seqnum] = std::move(log_entry);
}

bool Storage::Read(uint64_t log_seqnum, std::span<const char>* data) {
    if (!entries_.contains(log_seqnum)) {
        return false;
    }
    LogEntry* entry = entries_[log_seqnum].get();
    *data = std::span<const char>(entry->data.data(), entry->data.size());
    return true;
}

}  // namespace log
}  // namespace faas
