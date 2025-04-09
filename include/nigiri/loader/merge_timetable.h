#pragma once

#include <mutex>

#include "nigiri/string_store.h"

namespace nigiri::loader::gtfs {
struct shape_loader_state;
struct trip_data;
}  // namespace nigiri::loader::gtfs

namespace nigiri {
struct timetable;
}  // namespace nigiri

namespace nigiri::loader {
void merge_tables(timetable& lhs,
                  timetable&& rhs,
                  gtfs::trip_data& lhs_trip_data,
                  gtfs::trip_data& rhs_trip_data,
                  string_cache_t& str_cache,
                  std::mutex* = nullptr);
}