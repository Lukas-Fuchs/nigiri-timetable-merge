#pragma once

#include <queue>
#include <thread>

#include "nigiri/loader/dir.h"
#include "nigiri/loader/gtfs/loader.h"
#include "nigiri/loader/gtfs/shape.h"
#include "nigiri/loader/gtfs/trip.h"
#include "nigiri/loader/init_finish.h"
#include "nigiri/timetable.h"

namespace nigiri::loader {

struct loading_threadpool {
  struct non_threadsafe_loader_exception : std::exception {};

  loading_threadpool(unsigned int n_threads,
                     std::vector<std::unique_ptr<loader_interface>> const&,
                     std::vector<std::pair<std::string, loader_config>> const&,
                     interval<date::sys_days> const&,
                     assistance_times*,
                     shapes_storage*);

  timetable get_result();

private:
  struct loading_work_item {
    size_t table_idx_;
    size_t loader_idx_;

    std::unique_ptr<dir> dir_;
    std::string path_;
    loader_config config_;
  };

  void load(loading_work_item&& work);
  void merge(size_t l, size_t r);
  void wait_for_work();

  std::mutex work_mutex_;
  std::vector<std::thread> workers_;
  std::vector<loading_work_item> load_queue_;
  std::queue<size_t> mergable_tables_;
  std::vector<timetable> tables_;
  std::vector<gtfs::shape_loader_state> table_shapes_;
  std::vector<gtfs::trip_data> table_trip_data_;
  std::atomic<size_t> merge_ops_left_;

  interval<date::sys_days> const& date_range_;
  std::vector<std::unique_ptr<loader_interface>> const& loaders_;

  // shared data
  std::mutex table_mutex_;
  std::unique_ptr<string_cache_t> cache_;
  hash_map<bitfield, bitfield_idx_t> bitfields_;
  assistance_times* assistance_;
  shapes_storage* shapes_;
};

}  // namespace nigiri::loader