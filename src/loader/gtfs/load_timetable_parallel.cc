#include "nigiri/loader/gtfs/load_timetable.h"

#include <charconv>
#include <filesystem>
#include <numeric>
#include <string>

#include "utl/get_or_create.h"
#include "utl/progress_tracker.h"

#include "cista/hash.h"
#include "cista/mmap.h"

#include "wyhash.h"

#include "nigiri/loader/get_index.h"
#include "nigiri/loader/gtfs/agency.h"
#include "nigiri/loader/gtfs/calendar.h"
#include "nigiri/loader/gtfs/calendar_date.h"
#include "nigiri/loader/gtfs/fares.h"
#include "nigiri/loader/gtfs/files.h"
#include "nigiri/loader/gtfs/local_to_utc.h"
#include "nigiri/loader/gtfs/noon_offsets.h"
#include "nigiri/loader/gtfs/route.h"
#include "nigiri/loader/gtfs/route_key.h"
#include "nigiri/loader/gtfs/services.h"
#include "nigiri/loader/gtfs/shape.h"
#include "nigiri/loader/gtfs/shape_prepare.h"
#include "nigiri/loader/gtfs/stop.h"
#include "nigiri/loader/gtfs/stop_seq_number_encoding.h"
#include "nigiri/loader/gtfs/stop_time.h"
#include "nigiri/loader/gtfs/trip.h"
#include "nigiri/loader/loader_interface.h"
#include "nigiri/common/sort_by.h"
#include "nigiri/logging.h"
#include "nigiri/shapes_storage.h"
#include "nigiri/timetable.h"

#include "../thirdparty/tracy/public/tracy/Tracy.hpp"

namespace fs = std::filesystem;

namespace nigiri::loader::gtfs {

struct idx_offsets {
  using idx_offset_t = int64_t;
  idx_offsets(timetable const& tt) {
    bitfield_offset = tt.bitfields_.size();
    location_offset = tt.n_locations();
    route_offset = tt.n_routes();
    transport_offset = tt.transport_traffic_days_.size();
    timezone_offset = tt.locations_.timezones_.size();
    source_file_offset = tt.source_file_names_.size();
    trip_line_offset = tt.trip_lines_.size();
    attribute_offset = tt.attributes_.size();
    provider_offset = tt.providers_.size();
    area_offset = tt.areas_.size();
  }

  void correct_idx(bitfield_idx_t& idx) { idx += bitfield_offset; }
  void correct_idx(location_idx_t& idx) { idx += location_offset; }
  void correct_idx(route_idx_t& idx) { idx += route_offset; }
  void correct_idx(transport_idx_t& idx) { idx += transport_offset; }
  void correct_idx(timezone_idx_t& idx) { idx += timezone_offset; }
  void correct_idx(source_file_idx_t& idx) { idx += source_file_offset; }
  void correct_idx(trip_line_idx_t& idx) { idx += trip_line_offset; }
  void correct_idx(attribute_idx_t& idx) { idx += attribute_offset; }
  void correct_idx(provider_idx_t& idx) { idx += provider_offset; }
  void correct_idx(area_idx_t& idx) { idx += area_offset; }

private:
  idx_offset_t bitfield_offset{0};
  idx_offset_t location_offset{0};
  idx_offset_t route_offset{0};
  idx_offset_t transport_offset{0};
  idx_offset_t timezone_offset{0};
  idx_offset_t source_file_offset{0};
  idx_offset_t trip_line_offset{0};
  idx_offset_t attribute_offset{0};
  idx_offset_t provider_offset{0};
  idx_offset_t area_offset{0};
};

struct unfinished_timetable {
  timetable finish();
  void merge(unfinished_timetable&& other);

  timetable tt_;
  std::vector<source_idx_t> src_;

  agency_map_t agencies_;
  locations_map stops_;
  route_map_t routes_;
  trip_data trip_data_;

  tz_map timezones_;
  hash_map<bitfield, bitfield_idx_t> bitfield_indices_;
  assistance_times* assistance_{nullptr};

  shapes_storage* shapes_data_;
  shape_loader_state shape_states_;

  std::filesystem::path path_;
  utl::progress_tracker_ptr progress_tracker_;

private:
  bool finished_{false};
};

timetable unfinished_timetable::finish() {
  ZoneScoped;
  // TODO: Postprocess foot paths

  {
    assert(!finished_);
    ZoneScopedN("sorting trips");
    auto const timer = scoped_timer{"loader.gtfs.trips.sort"};
    for (auto& t : trip_data_.data_) {
      if (t.requires_sorting_) {
        t.stop_headsigns_.resize(t.seq_numbers_.size());
        std::tie(t.seq_numbers_, t.stop_seq_, t.event_times_, t.stop_headsigns_,
                 t.distance_traveled_) =
            sort_by(t.seq_numbers_, t.stop_seq_, t.event_times_,
                    t.stop_headsigns_, t.distance_traveled_);
      }
    }
  }

  {
    ZoneScopedN("Interpolating");
    auto const timer = scoped_timer{"loader.gtfs.trips.interpolate"};
    for (auto& t : trip_data_.data_) {
      t.interpolate();
    }
  }

  hash_map<route_key_t, std::vector<std::vector<utc_trip>>, route_key_hash,
           route_key_equals>
      route_services;
  auto const noon_offsets = precompute_noon_offsets(tt_, agencies_);

  stop_seq_t stop_seq_cache;
  bitvec bikes_allowed_seq_cache;
  auto const get_bikes_allowed_seq =
      [&](std::basic_string<gtfs_trip_idx_t> const& trips) -> bitvec const* {
    ZoneScopedN("get_bikes_allowed_seq");
    if (trips.size() == 1U) {
      return trip_data_.get(trips.front()).bikes_allowed_
                 ? &kSingleTripBikesAllowed
                 : &kSingleTripBikesNotAllowed;
    } else {
      bikes_allowed_seq_cache.resize(0);
      for (auto const [i, t_idx] : utl::enumerate(trips)) {
        auto const& trp = trip_data_.get(t_idx);
        auto const stop_count = trp.stop_seq_.size();
        auto const offset = bikes_allowed_seq_cache.size();
        bikes_allowed_seq_cache.resize(
            static_cast<bitvec::size_type>(offset + stop_count - 1));
        for (auto j = 0U; j < stop_count - 1; ++j) {
          bikes_allowed_seq_cache.set(offset + j, trp.bikes_allowed_);
        }
      }
      return &bikes_allowed_seq_cache;
    }
  };

  auto const add_trip = [&](std::basic_string<gtfs_trip_idx_t> const& trips,
                            bitfield const* traffic_days) {
    ZoneScopedN("add_trip");
    // ZoneScopedNC("assistance", 0xFF0000);

    // std::cout << "Adding " << trips.size() << " trips" << std::endl;
    // std::cout << "Adding " << trip_data_.data_.size() << " trips" <<
    // std::endl;

    expand_trip(
        trip_data_, noon_offsets, tt_, trips, traffic_days, tt_.date_range_,
        assistance_, [&](utc_trip&& s) {
          auto const* stop_seq = get_stop_seq(trip_data_, s, stop_seq_cache);
          auto const clasz = trip_data_.get(s.trips_.front()).get_clasz(tt_);
          auto const* bikes_allowed_seq = get_bikes_allowed_seq(s.trips_);
          auto const it = route_services.find(
              route_key_ptr_t{clasz, stop_seq, bikes_allowed_seq});
          if (it != end(route_services)) {
            for (auto& r : it->second) {
              auto const idx = get_index(r, s);
              if (idx.has_value()) {
                r.insert(std::next(begin(r), static_cast<int>(*idx)), s);
                return;
              }
            }
            it->second.emplace_back(std::vector<utc_trip>{std::move(s)});
          } else {
            route_services.emplace(
                route_key_t{clasz, *stop_seq, *bikes_allowed_seq},
                std::vector<std::vector<utc_trip>>{{s}});
          }
        });
  };

  {
    // progress_tracker_->status("Expand Trips")
    //.out_bounds(68.F, 83.F).in_high(trip_data_.data_.size());
    auto const timer = scoped_timer{"loader.gtfs.trips.expand"};

    for (auto const [i, t] : utl::enumerate(trip_data_.data_)) {
      if (t.block_ != nullptr) {
        continue;
      }
      add_trip({gtfs_trip_idx_t{i}}, t.service_);
      // progress_tracker_->increment();
    }
  }

  {
    // progress_tracker_->status("Stay Seated")
    //.out_bounds(83.F, 85.F).in_high(route_services.size());
    auto const timer = scoped_timer{"loader.gtfs.trips.block_id"};

    for (auto const& [_, blk] : trip_data_.blocks_) {
      for (auto const& [trips, traffic_days] : blk->rule_services(trip_data_)) {
        add_trip(trips, &traffic_days);
      }
    }
  }

  {
    ZoneScopedN("Write Trips");
    // progress_tracker_->status("Write Trips")
    //.out_bounds(85.F, 98.F).in_high(route_services.size());

    auto const is_train_number = [](auto const& s) {
      return !s.empty() && std::all_of(begin(s), end(s), [](auto&& c) -> bool {
        return std::isdigit(c);
      });
    };

    auto stop_seq_numbers = std::basic_string<stop_idx_t>{};
    auto const source_file_idx =
        tt_.register_source_file((path_ / kStopTimesFile).generic_string());
    for (auto& trp : trip_data_.data_) {
      std::uint32_t train_nr = 0U;
      if (is_train_number(trp.short_name_)) {
        train_nr = static_cast<std::uint32_t>(std::stoul(trp.short_name_));
      } else if (auto const headsign = tt_.trip_direction(trp.headsign_);
                 is_train_number(headsign)) {
        std::from_chars(headsign.data(), headsign.data() + headsign.size(),
                        train_nr);
      }
      encode_seq_numbers(trp.seq_numbers_, stop_seq_numbers);
      trp.trip_idx_ = tt_.register_trip_id(
          trp.id_, trp.route_->route_id_idx_, src_[0], trp.display_name(),
          {source_file_idx, trp.from_line_, trp.to_line_}, train_nr,
          stop_seq_numbers);
    }

    auto const timer = scoped_timer{"loader.gtfs.routes.build"};
    auto const attributes = std::basic_string<attribute_combination_idx_t>{};
    auto lines = hash_map<std::string, trip_line_idx_t>{};
    auto section_directions = std::basic_string<trip_direction_idx_t>{};
    auto section_lines = std::basic_string<trip_line_idx_t>{};
    auto route_colors = std::basic_string<route_color>{};
    auto external_trip_ids = std::basic_string<merged_trips_idx_t>{};
    auto location_routes = mutable_fws_multimap<location_idx_t, route_idx_t>{};
    for (auto const& [key, sub_routes] : route_services) {
      for (auto const& services : sub_routes) {
        auto const route_idx =
            tt_.register_route(key.stop_seq_, {key.clasz_}, key.bikes_allowed_);

        for (auto const& s : key.stop_seq_) {
          auto s_routes = location_routes[stop{s}.location_idx()];
          if (s_routes.empty() || s_routes.back() != route_idx) {
            s_routes.emplace_back(route_idx);
          }
        }

        for (auto const& s : services) {
          auto const& first = trip_data_.get(s.trips_.front());

          external_trip_ids.clear();
          section_directions.clear();
          section_lines.clear();
          route_colors.clear();
          auto prev_end = std::uint16_t{0U};
          for (auto const [i, t] : utl::enumerate(s.trips_)) {
            auto& trp = trip_data_.get(t);

            auto const end =
                static_cast<std::uint16_t>(prev_end + trp.stop_seq_.size());

            trp.transport_ranges_.emplace_back(
                transport_range_t{tt_.next_transport_idx(), {prev_end, end}});
            prev_end = end - 1;

            auto const line =
                utl::get_or_create(lines, trp.route_->short_name_, [&]() {
                  auto const idx = trip_line_idx_t{tt_.trip_lines_.size()};
                  tt_.trip_lines_.emplace_back(trp.route_->short_name_);
                  return idx;
                });

            auto const merged_trip = tt_.register_merged_trip({trp.trip_idx_});
            if (s.trips_.size() == 1U) {
              external_trip_ids.push_back(merged_trip);
              section_directions.push_back(trp.headsign_);
              section_lines.push_back(line);
              route_colors.push_back(
                  {trp.route_->color_, trp.route_->text_color_});
            } else {
              for (auto section = 0U; section != trp.stop_seq_.size() - 1;
                   ++section) {
                external_trip_ids.push_back(merged_trip);
                section_directions.push_back(trp.headsign_);
                section_lines.push_back(line);
                route_colors.push_back(
                    {trp.route_->color_, trp.route_->text_color_});
              }
            }
          }

          tt_.add_transport(timetable::transport{
              .bitfield_idx_ = utl::get_or_create(
                  bitfield_indices_, s.utc_traffic_days_,
                  [&]() { return tt_.register_bitfield(s.utc_traffic_days_); }),
              .route_idx_ = route_idx,
              .first_dep_offset_ = s.first_dep_offset_,
              .external_trip_ids_ = external_trip_ids,
              .section_attributes_ = attributes,
              .section_providers_ = {first.route_->agency_},
              .section_directions_ = section_directions,
              .section_lines_ = section_lines,
              .stop_seq_numbers_ = stop_seq_numbers,
              .route_colors_ = route_colors});
        }

        tt_.finish_route();

        auto const stop_times_begin = tt_.route_stop_times_.size();
        for (auto const [from, to] :
             utl::pairwise(interval{std::size_t{0U}, key.stop_seq_.size()})) {
          // Write departure times of all route services at stop i.
          for (auto const& s : services) {
            tt_.route_stop_times_.emplace_back(s.utc_times_[from * 2]);
          }

          // Write arrival times of all route services at stop i+1.
          for (auto const& s : services) {
            tt_.route_stop_times_.emplace_back(s.utc_times_[to * 2 - 1]);
          }
        }
        auto const stop_times_end = tt_.route_stop_times_.size();
        tt_.route_stop_time_ranges_.emplace_back(
            interval{stop_times_begin, stop_times_end});
      }

      // progress_tracker_->increment();
    }

    if (shapes_data_) {
      ZoneScopedNC("shapes_data", 0xFF0000);
      calculate_shape_offsets_and_bboxes(tt_, *shapes_data_, shape_states_,
                                         trip_data_.data_);
    }

    // Build location_routes map
    for (auto l = tt_.location_routes_.size(); l != tt_.n_locations(); ++l) {
      tt_.location_routes_.emplace_back(location_routes[location_idx_t{l}]);
      assert(tt_.location_routes_.size() == l + 1U);
    }

    // Build transport ranges.
    for (auto const& t : trip_data_.data_) {
      tt_.trip_transport_ranges_.emplace_back(t.transport_ranges_);
    }
  }

  finished_ = true;
  return std::move(tt_);
}

void unfinished_timetable::merge(unfinished_timetable&& other) {
  // TODO: Merge timetables
  // TODO: Do I even need full timetables here? If the data stored in there by
  // the loading function is limited enough I may just use it directly and
  // incorporate it during finalization.

  idx_offsets ofs(tt_);

  for (auto p : other.tt_.providers_) {
    tt_.register_provider(std::move(p));
  }

  for (auto const& [name, agency] : other.agencies_) {
    auto new_agency = agency;
    ofs.correct_idx(new_agency);
    assert(new_agency < tt_.providers_.size());
    agencies_[name] = new_agency;
  }

  // Timezone names for the merged timetable
  hash_map<timezone_idx_t, std::string> timezone_names;

  for (auto const& [tz_name, tz_idx] : other.timezones_) {
    if (auto const it = timezones_.find(tz_name); it != timezones_.end()) {
      // TODO: Make sure timezones with the same name are actually the same
      timezone_names[it->second] = tz_name;
      continue;
    }
    auto const new_tz_idx = tt_.locations_.register_timezone(
        other.tt_.locations_.timezones_[tz_idx]);

    timezones_[tz_name] = new_tz_idx;
    timezone_names[new_tz_idx] = tz_name;
  }

  auto& locs = other.tt_.locations_;

  for (size_t loc_idx = 0; loc_idx < locs.names_.size(); ++loc_idx) {
    // Key location indices are corrected by the insertion, timezone index is
    // explicitly corrected here.
    location_idx_t new_idx(loc_idx);
    ofs.correct_idx(new_idx);
    location loc(other.tt_, location_idx_t(loc_idx));

    loc.timezone_idx_ = timezones_[timezone_names[loc.timezone_idx_]];
    // ofs.correct_idx(loc.timezone_idx_);
    tt_.locations_.register_location(loc);
  }

  if (assistance_) {
    // TODO: Assistance times
  }

  // Bitfield indices are created from the merged timetable during finalization.

  auto transfer_headsign = [&](trip_direction_idx_t headsign) {
    return trip_data_.get_or_create_direction(
        tt_, other.tt_.trip_direction(headsign));
  };

  for (auto const& [k, direction] : other.trip_data_.directions_) {
    transfer_headsign(direction);
  }

  // TODO: Trips
  auto const trip_offset = trip_data_.data_.size();
  for (auto& trp : other.trip_data_.data_) {
    assert(trp.route_->agency_ < tt_.providers_.size());
    trp.trip_idx_ += trip_offset;
    trp.headsign_ = transfer_headsign(trp.headsign_);
    for (auto& stop_headsign : trp.stop_headsigns_) {
      stop_headsign = transfer_headsign(stop_headsign);
    }
    // The trip's shape index does not need correcting because shape storage is
    // central and timetables know their shape offsets at load time.
    trip_data_.data_.emplace_back(std::move(trp));
  }

  for (auto const& [k, trip_idx] : other.trip_data_.trips_) {
    trip_data_.trips_.emplace(k, trip_idx + trip_offset);
  }

  for (auto& [k, block] : other.trip_data_.blocks_) {
    for (auto& trip : block->trips_) {
      trip += trip_offset;
    }
    for (auto& [trip, bitfield] : block->rule_services(trip_data_)) {
      for (auto& t : trip) {
        t += trip_offset;
      }
    }

    trip_data_.blocks_.emplace(k, std::move(block));
  }

  for (auto& trp : trip_data_.data_) {
    assert(trp.route_->agency_ < tt_.providers_.size());
  }

  // Route loading only affects the timetable's agencies so only the auxiliary
  // route map needs merging.
  for (auto& [k, r] : other.routes_) {
    if (routes_.contains(k)) {
      std::cout << "Route already exists: " << k << std::endl;
      continue;
    }
    ofs.correct_idx(r->agency_);
    routes_.emplace(k, std::move(r));
  }

  for (auto& trp : trip_data_.data_) {
    assert(trp.route_->agency_ < tt_.providers_.size());
  }

  if (other.src_.size() == 1) {
    // Single-source tables only have a single fares entry, which is assigned to
    // the appropriate source index in the target table.
    // Note that this leaves holes in the fare vector that are filled in
    // subsequent merge operations.
    tt_.fares_.resize(std::max<source_idx_t::value_t>(
        tt_.fares_.size(), source_idx_t::value_t(other.src_[0]) + 1));
    tt_.fares_[other.src_[0]] = other.tt_.fares_.front();
  } else {
    // Multi-source tables have fares for a set of source indices. Since the
    // source indices in timetables are contiguous, fares are copied according
    // to the set of source indices known to the source timetable.
    tt_.fares_.resize(std::max(tt_.fares_.size(), other.tt_.fares_.size()));
    for (auto const& src_idx : other.src_) {
      tt_.fares_[src_idx] = other.tt_.fares_[src_idx];
    }
  }

  for (auto const& area : other.tt_.areas_) {
    tt_.areas_.emplace_back(area);
  }

  // TODO: This accesses some private field. Uncomment and fix.
  /*for (auto const& [loc, area] : other.tt_.location_areas_) {
    auto new_idx = area;
    ofs.correct_idx(new_idx);
    tt_.location_areas_.emplace_back(new_idx);
  }*/

  // TODO: Combine path / source index information meaningfully

  // TODO: Continue merging the stuff loading produced. This may not be
  // everything in the timetable since finish also does a bunch of stuff.
}

static unfinished_timetable load_tt_unfinished(loader_config const& config,
                                               source_idx_t const src,
                                               dir const& d,
                                               string_cache_t& str_cache,
                                               shapes_storage* shapes_data) {
  ZoneScoped;
  static std::mutex shape_storage_mutex;
  static std::mutex str_cache_mutex;

  unfinished_timetable utt;
  utt.path_ = d.path();
  utt.src_ = {src};
  utt.progress_tracker_ = utl::get_active_progress_tracker();
  utt.shapes_data_ = shapes_data;

  auto const load = [&](std::string_view file_name) -> file {
    return d.exists(file_name) ? d.get_file(file_name) : file{};
  };

  utt.agencies_ =
      read_agencies(utt.tt_, utt.timezones_, load(kAgencyFile).data());
  utt.stops_ =
      read_stops(src, utt.tt_, utt.timezones_, load(kStopFile).data(),
                 load(kTransfersFile).data(), config.link_stop_distance_);
  utt.routes_ = read_routes(utt.tt_, utt.timezones_, utt.agencies_,
                            load(kRoutesFile).data(), config.default_tz_);

  for (auto& trp : utt.trip_data_.data_) {
    assert(trp.route_->agency_ < utt.tt_.providers_.size());
  }

  auto const calendar = read_calendar(load(kCalenderFile).data());
  auto const dates = read_calendar_date(load(kCalendarDatesFile).data());
  // TODO: This uses information from the timetable. Derivatives of this may
  // need merging.
  auto const service =
      merge_traffic_days(utt.tt_.internal_interval_days(), calendar, dates);

  {
    // shapes_storage uses the same storage file for all time tables and time
    // tables determine their relevant set of shapes by offset so interleaving
    // is not permitted. The loading routine must therefore be guarded.
    std::unique_lock lk(shape_storage_mutex);
    ZoneScopedNC("shapes_data", 0xFF0000);
    utt.shape_states_ =
        (utt.shapes_data_ != nullptr)
            ? parse_shapes(load(kShapesFile).data(), *utt.shapes_data_)
            : shape_loader_state{};
  }

  utt.trip_data_ =
      read_trips(utt.tt_, utt.routes_, service, utt.shape_states_,
                 load(kTripsFile).data(), config.bikes_allowed_default_);

  for (auto& trp : utt.trip_data_.data_) {
    assert(trp.route_->agency_ < utt.tt_.providers_.size());
  }

  read_frequencies(utt.trip_data_, load(kFrequenciesFile).data());
  read_stop_times(utt.tt_, utt.trip_data_, utt.stops_,
                  load(kStopTimesFile).data(), utt.shapes_data_ != nullptr);

  {
    std::unique_lock lk(str_cache_mutex);
    load_fares(utt.tt_, str_cache, d, service, utt.routes_, utt.stops_);
  }
  /*utl::verify(utt.tt_.fares_.size() == to_idx(src) + 1U,
              "fares: size={} src={}", utt.tt_.fares_.size(), src);*/

  return utt;
}

void load_timetable(
    std::vector<std::pair<std::string, loader_config>> const& configs,
    std::vector<std::unique_ptr<const dir>> const& directories,
    timetable& tt,
    hash_map<bitfield, bitfield_idx_t>& bitfields,
    string_cache_t& str_cache,
    assistance_times* assistance,
    shapes_storage* shapes_data) {

  unfinished_timetable dst;
  dst.tt_ = tt;
  dst.bitfield_indices_ = bitfields;
  dst.assistance_ = assistance;
  dst.shapes_data_ = shapes_data;

  std::vector<unfinished_timetable> utts;
  utts.reserve(directories.size());

  source_idx_t src(0);
  for (auto const& [d, config] : utl::zip(directories, configs)) {
    utts.emplace_back(
        load_tt_unfinished(config.second, src++, *d, str_cache, shapes_data));
  }

  for (auto& utt : utts) {
    dst.merge(std::move(utt));
  }

  tt = std::move(dst.finish());
}

}  // namespace nigiri::loader::gtfs