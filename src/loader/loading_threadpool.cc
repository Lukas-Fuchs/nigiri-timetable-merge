#include "nigiri/loader/loading_threadpool.h"

#include "utl/enumerate.h"

#include "nigiri/loader/gtfs/shape.h"
#include "nigiri/loader/gtfs/shape_prepare.h"
#include "nigiri/loader/merge_timetable.h"

namespace nigiri::loader {
loading_threadpool::loading_threadpool(
    unsigned int n_threads,
    std::vector<std::unique_ptr<loader_interface>> const& loaders,
    std::vector<std::pair<std::string, loader_config>> const& paths,
    interval<date::sys_days> const& date_range,
    assistance_times* a,
    shapes_storage* shapes)
    : date_range_(date_range),
      loaders_(loaders),
      assistance_(a),
      shapes_(shapes) {

  // 0 means "use as many threads as files".
  if (n_threads == 0 || n_threads > paths.size()) {
    n_threads = paths.size();
  }

  tables_.resize(paths.size());
  table_shapes_.resize(paths.size());
  table_trip_data_.resize(paths.size());
  tables_[0].date_range_ = date_range;
  register_special_stations(tables_[0]);

  bitfields_ = hash_map<bitfield, bitfield_idx_t>{};
  cache_ = std::make_unique<string_cache_t>(
      std::size_t{0U}, string_idx_hash{tables_[0].strings_.strings_},
      string_idx_equals{tables_[0].strings_.strings_});

  std::vector<std::unique_ptr<dir>> dirs;
  dirs.reserve(paths.size());

  for (auto const& [path, _] : paths) {
    dirs.emplace_back(path.starts_with("\n#")
                          ? std::make_unique<mem_dir>(mem_dir::read(path))
                          : make_dir(path));
    if (utl::find_if(loaders, [&](auto const& l) {
          return l->supports_parallel() && l->applicable(*dirs.back());
        }) == loaders.end()) {
      throw non_threadsafe_loader_exception();
    }
  }

  for (auto const [idx, in] : utl::enumerate(paths)) {
    auto const& [path, local_config] = in;
    auto const is_in_memory = path.starts_with("\n#");
    auto const it = utl::find_if(
        loaders, [&](auto const& l) { return l->applicable(*dirs[idx]); });
    if (it != end(loaders)) {
      if (!is_in_memory) {
        log(log_lvl::info, "loader.parallel_load", "loading {}", path);
      }

      load_queue_.emplace_back(idx, it - loaders.begin(), std::move(dirs[idx]),
                               path, local_config);
    }
  }

  merge_ops_left_ = tables_.size() - 1;

  workers_.reserve(n_threads);
  for (size_t i = 0; i < n_threads; ++i) {
    workers_.emplace_back(&loading_threadpool::wait_for_work, this);
  }
}

timetable loading_threadpool::get_result() {
  for (auto& t : workers_) {
    t.join();
  }
  assert(mergable_tables_.size() == 1);
  auto const tt_idx = mergable_tables_.front();
  auto& tt = tables_[tt_idx];
  if (shapes_ != nullptr) {
    std::sort(begin(table_shapes_), end(table_shapes_),
              [](auto const& l, auto const& r) {
                return l.index_offset_ < r.index_offset_;
              });
    auto& shapes = table_shapes_[0];
    assert(shapes.index_offset_ == 0);
    for (size_t i = 1; i < table_shapes_.size(); ++i) {
      auto& s = table_shapes_[i];
      for (auto&& d : s.distances_) {
        shapes.distances_.emplace_back(std::move(d));
      }

      // TODO: This may not be needed as the map seems to be only used as a
      // size indicator after this. If there is no other intended use just
      // add some other indicator.
      for (auto [id, idx] : s.id_map_) {
        if (shapes.id_map_.contains(id)) {
          id = id + std::to_string(uint32_t(idx));
          assert(!shapes.id_map_.contains(id));
        }
        shapes.id_map_[id] = idx;
      }
    }

    calculate_shape_offsets_and_bboxes(tt, *shapes_, shapes,
                                       table_trip_data_[tt_idx].data_);
  }
  return std::move(tt);
}

void loading_threadpool::load(loading_work_item&& work) {
  try {
    log(log_lvl::info, "loading_threadpool.load", "loading {}", work.path_);
    timetable& tt = tables_[work.table_idx_];
    tt.date_range_ = date_range_;
    loaders_[work.loader_idx_]->load_threadsafe(
        work.config_, source_idx_t(0), *work.dir_, tt, bitfields_, *cache_,
        assistance_, shapes_, &table_shapes_[work.table_idx_],
        &table_trip_data_[work.table_idx_], table_mutex_);
  } catch (std::exception const& e) {
    throw utl::fail("failed to load {}: {}", work.path_, e.what());
  }
  std::lock_guard g(work_mutex_);
  mergable_tables_.push(work.table_idx_);
}

void loading_threadpool::merge(size_t l, size_t r) {
  assert(l != r);
  // The first table contains the special stations and must be the leftmost one
  // in the tree.
  if (r == 0) {
    merge(r, l);
    return;
  }
  merge_tables(tables_[l], std::move(tables_[r]), table_trip_data_[l],
               table_trip_data_[r], *cache_, &table_mutex_);
  {
    std::lock_guard g(work_mutex_);
    mergable_tables_.push(l);
  }
}

void loading_threadpool::wait_for_work() {
  while (!load_queue_.empty()) {
    auto work = [&]() -> std::optional<loading_work_item> {
      std::lock_guard g(work_mutex_);
      if (!load_queue_.empty()) {
        auto w = std::move(load_queue_.back());
        load_queue_.pop_back();
        return w;
      }
      return std::nullopt;
    }();
    if (work) {
      load(std::move(*work));
    }
  }

  while (merge_ops_left_) {
    if (mergable_tables_.size() >= 2) {
      size_t l, r;
      {
        std::lock_guard g(work_mutex_);
        if (mergable_tables_.size() < 2) continue;
        l = mergable_tables_.front();
        mergable_tables_.pop();
        r = mergable_tables_.front();
        mergable_tables_.pop();

        merge_ops_left_--;
      }
      merge(l, r);
    } else {
      std::lock_guard g(work_mutex_);
      if (mergable_tables_.size() < 2) {
        return;
      }
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
}
}  // namespace nigiri::loader