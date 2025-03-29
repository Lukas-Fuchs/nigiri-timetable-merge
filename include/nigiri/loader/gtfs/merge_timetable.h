#pragma once

#include <mutex>

#include "nigiri/string_store.h"

namespace nigiri {
struct timetable;
}  // namespace nigiri

namespace nigiri::loader::gtfs {
void merge_tables(timetable& lhs,
                  timetable&& rhs,
                  string_cache_t& str_cache,
                  std::mutex* = nullptr);
}