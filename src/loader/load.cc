#include "nigiri/loader/load.h"

#include <thread>

#include "fmt/std.h"

#include "utl/enumerate.h"

#include "nigiri/loader/dir.h"
#include "nigiri/loader/gtfs/loader.h"
#include "nigiri/loader/gtfs/merge_timetable.h"
#include "nigiri/loader/hrd/loader.h"
#include "nigiri/loader/init_finish.h"
#include "nigiri/timetable.h"

namespace nigiri::loader {

std::vector<std::unique_ptr<loader_interface>> get_loaders() {
  auto loaders = std::vector<std::unique_ptr<loader_interface>>{};
  loaders.emplace_back(std::make_unique<gtfs::gtfs_loader>());
  loaders.emplace_back(std::make_unique<hrd::hrd_5_00_8_loader>());
  loaders.emplace_back(std::make_unique<hrd::hrd_5_20_26_loader>());
  loaders.emplace_back(std::make_unique<hrd::hrd_5_20_39_loader>());
  loaders.emplace_back(std::make_unique<hrd::hrd_5_20_avv_loader>());
  return loaders;
}

timetable load(std::vector<std::pair<std::string, loader_config>> const& paths,
               finalize_options const& finalize_opt,
               interval<date::sys_days> const& date_range,
               assistance_times* a,
               shapes_storage* shapes,
               bool ignore) {
  return parallel_load(paths, finalize_opt, date_range, a, shapes, ignore);
}

timetable serial_load(
    std::vector<std::pair<std::string, loader_config>> const& paths,
    finalize_options const& finalize_opt,
    interval<date::sys_days> const& date_range,
    assistance_times* a,
    shapes_storage* shapes,
    bool ignore) {

  auto const loaders = get_loaders();

  auto tt = timetable{};
  tt.date_range_ = date_range;
  register_special_stations(tt);

  auto bitfields = hash_map<bitfield, bitfield_idx_t>{};
  auto cache =
      string_cache_t{std::size_t{0U}, string_idx_hash{tt.strings_.strings_},
                     string_idx_equals{tt.strings_.strings_}};

  for (auto const [idx, in] : utl::enumerate(paths)) {
    auto const& [path, local_config] = in;
    auto const is_in_memory = path.starts_with("\n#");
    auto const src = source_idx_t{idx};
    auto const dir = is_in_memory
                         // hack to load strings in integration tests
                         ? std::make_unique<mem_dir>(mem_dir::read(path))
                         : make_dir(path);
    auto const it =
        utl::find_if(loaders, [&](auto&& l) { return l->applicable(*dir); });
    if (it != end(loaders)) {
      if (!is_in_memory) {
        log(log_lvl::info, "loader.serial_load", "loading {}", path);
      }
      try {
        (*it)->load(local_config, src, *dir, tt, bitfields, cache, a, shapes);
      } catch (std::exception const& e) {
        throw utl::fail("failed to load {}: {}", path, e.what());
      }
    } else if (!ignore) {
      throw utl::fail("no loader for {} found", path);
    } else {
      log(log_lvl::error, "loader.serial_load", "no loader for {} found", path);
    }
  }

  finalize(tt, finalize_opt);

  return tt;
}

timetable parallel_load(
    std::vector<std::pair<std::string, loader_config>> const& paths,
    finalize_options const& finalize_opt,
    interval<date::sys_days> const& date_range,
    assistance_times* a,
    shapes_storage* shapes,
    bool ignore) {

  auto const loaders = get_loaders();

  std::vector<timetable> tables;
  tables.resize(paths.size());
  tables[0].date_range_ = date_range;
  register_special_stations(tables[0]);

  auto bitfields = hash_map<bitfield, bitfield_idx_t>{};
  auto cache = string_cache_t{std::size_t{0U},
                              string_idx_hash{tables[0].strings_.strings_},
                              string_idx_equals{tables[0].strings_.strings_}};

  std::vector<std::unique_ptr<dir>> dirs;

  for (auto const& [path, _] : paths) {
    dirs.emplace_back(path.starts_with("\n#")
                          ? std::make_unique<mem_dir>(mem_dir::read(path))
                          : make_dir(path));
    if (utl::find_if(loaders, [&](auto const& l) {
          return l->supports_parallel() && l->applicable(*dirs.back());
        }) == loaders.end()) {
      log(log_lvl::info, "loader.parallel_load",
          "Some loaders do not support parallel loading. Falling back to "
          "serial loading.");
      return serial_load(paths, finalize_opt, date_range, a, shapes, ignore);
    }
  }

  {

    std::mutex mtx;

    auto const work = [&tables, date_range, &bitfields, &cache, a, shapes,
                       &mtx](size_t idx, size_t loader_idx,
                             std::unique_ptr<dir> dir,
                             loader_config local_config, std::string path) {
      try {
        timetable& tt = tables[idx];
        tt.date_range_ = date_range;
        get_loaders()[loader_idx]->load_threadsafe(
            local_config, source_idx_t(0), *dir, tt, bitfields, cache, a,
            shapes, mtx);
      } catch (std::exception const& e) {
        throw utl::fail("failed to load {}: {}", path, e.what());
      }
    };

    std::vector<std::jthread> threads;
    threads.reserve(dirs.size());
    for (auto const [idx, in] : utl::enumerate(paths)) {
      auto const& [path, local_config] = in;
      auto const is_in_memory = path.starts_with("\n#");
      auto const it = utl::find_if(
          loaders, [&](auto const& l) { return l->applicable(*dirs[idx]); });
      if (it != end(loaders)) {
        if (!is_in_memory) {
          log(log_lvl::info, "loader.parallel_load", "loading {}", path);
        }

        threads.emplace_back(work, idx, it - loaders.begin(),
                             std::move(dirs[idx]), local_config, path);
      } else if (!ignore) {
        throw utl::fail("no loader for {} found", path);
      } else {
        log(log_lvl::error, "loader.parallel_load", "no loader for {} found",
            path);
      }
    }
  }

  for (size_t i = 1; i < tables.size(); ++i) {
    gtfs::merge_tables(tables[0], std::move(tables[i]), cache);
  }

  finalize(tables[0], finalize_opt);

  return std::move(tables[0]);
}

}  // namespace nigiri::loader