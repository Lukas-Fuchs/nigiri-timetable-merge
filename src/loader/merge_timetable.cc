#include "nigiri/loader/merge_timetable.h"

#include <concepts>
#include <cstdint>

#include "nigiri/loader/gtfs/trip.h"
#include "nigiri/timetable.h"
#include "utl/enumerate.h"

namespace nigiri::loader {

using nigiri::timetable;
using nigiri::loader::gtfs::trip;

template <typename V>
concept Iterable = requires(V v) {
  v.begin();
  v.end();
};

struct index_manager {
  using idx_offset_t = int64_t;
  index_manager(timetable& tt,
                timetable const& other,
                string_cache_t& str_cache,
                std::mutex* cache_mutex) {
    auto const map_strings = [&]() {
      for (auto const& s : other.strings_.strings_) {
        string_map.emplace_back(
            tt.strings_.register_string(str_cache, s.view()));
      }
    };
    if (cache_mutex) {
      std::lock_guard g(*cache_mutex);
      map_strings();
    } else {
      map_strings();
    }
    bitfield_offset_ = tt.bitfields_.size();
    location_offset_ = tt.n_locations();
    route_offset_ = tt.n_routes();
    route_id_offset_ = idx_offset_t(uint32_t(tt.next_route_id_idx_));
    transport_offset_ = tt.transport_traffic_days_.size();
    merged_trips_offset_ = tt.merged_trips_.size();
    timezone_offset_ = tt.locations_.timezones_.size();
    source_offset_ = tt.fares_.size();
    source_file_offset_ = tt.source_file_names_.size();
    trip_offset_ = tt.trip_ids_.size();
    trip_id_offset_ = tt.trip_id_strings_.size();
    trip_direction_offset_ = tt.trip_directions_.size();
    trip_direction_string_offset_ = tt.trip_direction_strings_.size();
    trip_line_offset_ = tt.trip_lines_.size();
    attribute_offset_ = tt.attributes_.size();
    attribute_combination_offset_ = tt.attribute_combinations_.size();
    provider_offset_ = tt.providers_.size();
    area_offset_ = tt.areas_.size();
  }

  void correct_idx(string_idx_t& idx) const { idx = string_map[idx]; }
  void correct_idx(bitfield_idx_t& idx) const { idx += bitfield_offset_; }
  void correct_idx(location_idx_t& idx) const { idx += location_offset_; }
  void correct_idx(route_idx_t& idx) const { idx += route_offset_; }
  void correct_idx(route_id_idx_t& idx) const { idx += route_id_offset_; }
  void correct_idx(transport_idx_t& idx) const { idx += transport_offset_; }
  void correct_idx(merged_trips_idx_t& idx) const {
    idx += merged_trips_offset_;
  }
  void correct_idx(timezone_idx_t& idx) const { idx += timezone_offset_; }
  void correct_idx(source_idx_t& idx) const { idx += source_offset_; }
  void correct_idx(source_file_idx_t& idx) const { idx += source_file_offset_; }
  void correct_idx(trip_idx_t& idx) const { idx += trip_offset_; }
  void correct_idx(trip_id_idx_t& idx) const { idx += trip_id_offset_; }
  void correct_idx(trip_direction_idx_t& idx) const {
    idx += trip_direction_offset_;
  }
  void correct_idx(trip_direction_string_idx_t& idx) const {
    idx += trip_direction_string_offset_;
  }
  void correct_idx(trip_line_idx_t& idx) const { idx += trip_line_offset_; }
  void correct_idx(attribute_idx_t& idx) const { idx += attribute_offset_; }
  void correct_idx(attribute_combination_idx_t& idx) const {
    idx += attribute_combination_offset_;
  }
  void correct_idx(provider_idx_t& idx) const { idx += provider_offset_; }
  void correct_idx(area_idx_t& idx) const { idx += area_offset_; }

  // Shape indices are explicitly not corrected because they refer to shared
  // state and should remain stable.
  void correct_idx(shape_idx_t&) const {};

  uint32_t correct(uint32_t const a) const { return a; }
  string correct(string const& a) const { return a; }
  route_color correct(route_color const& a) const { return a; }
  bitfield correct(bitfield const& a) const { return a; }
  attribute correct(attribute const& a) const { return a; }
  delta correct(delta const& a) const { return a; }
  geo::latlng correct(geo::latlng const& a) const { return a; }
  duration_t correct(duration_t const& a) const { return a; }
  u8_minutes correct(u8_minutes const& a) const { return a; }
  location_type correct(location_type const& a) const { return a; }
  timezone correct(timezone const& a) const { return a; }

  location_id correct(location_id const& l) const {
    return {l.id_, correct(l.src_)};
  }

  footpath correct(footpath const& fp) const {
    return footpath{correct(fp.target()), fp.duration()};
  }

  area correct(area a) const { return area{correct(a.name_)}; }

  trip_debug correct(trip_debug const& td) const {
    return trip_debug{correct(td.source_file_idx_), td.line_number_from_,
                      td.line_number_to_};
  }

  provider correct(provider const& p) const {
    return provider{p.short_name_, p.long_name_, p.url_, correct(p.tz_)};
  }

  fares correct(fares&& f) const {
    // Fares contain mostly self-contained mappings so most of them don't need
    // correcting. Route IDs are from the timetable so they are corrected.
    auto const old_networks = std::move(f.route_networks_);
    f.route_networks_.clear();
    for (auto const& [k, v] : old_networks) {
      f.route_networks_.emplace(correct(k), v);
    };
    return f;
  }

  trip_direction_t correct(trip_direction_t const& td) const {
    return td.apply(
        [&](auto const& d) -> trip_direction_t { return correct(d); });
  }

  trip correct(trip&& t) const {
    t.headsign_ = correct(t.headsign_);
    t.shape_idx_ = correct(t.shape_idx_);
    t.stop_headsigns_ = correct(std::move(t.stop_headsigns_));
    t.trip_idx_ = correct(t.trip_idx_);
    t.transport_ranges_ = correct(std::move(t.transport_ranges_));
    for (auto& s : t.stop_seq_) {
      s += location_offset_;
    }
    return std::move(t);
  }

  template <typename T>
  interval<T> correct(interval<T> ival) const {
    return {correct(ival.from_), correct(ival.to_)};
  }

  template <typename T>
    requires requires(T& t, index_manager const ofs) { ofs.correct_idx(t); }
  T correct(T idx) const {
    if (idx == T::invalid()) return idx;
    T new_idx = idx;
    correct_idx(new_idx);
    return new_idx;
  }

  template <Iterable V>
  V correct(V&& vec) const {
    for (auto&& el : vec) {
      el = correct(el);
    }
    return std::move(vec);
  }

  template <typename T1, typename T2>
  pair<T1, T2> correct(pair<T1, T2> const& p) const {
    return pair<T1, T2>{correct(p.first), correct(p.second)};
  }

  template <typename T>
    requires std::is_scalar_v<T>
  T correct(T i) const {
    return i;
  }

  // Merge functions

  template <typename T>
  void merge_vector(vector<T>& lhs, vector<T>&& rhs) const {
    lhs.reserve(lhs.size() + rhs.size());
    for (auto&& el : rhs) {
      lhs.emplace_back(correct(el));
    }
  }

  template <typename K, typename V>
  void merge_fws_multimap(mutable_fws_multimap<K, V>& lhs,
                          mutable_fws_multimap<K, V>&& rhs) const {
    for (auto&& bkt : rhs) {
      lhs.emplace_back();
      for (auto&& el : bkt) {
        lhs.back().emplace_back(correct(std::move(el)));
      }
    }
  }

  template <typename K, typename V>
  void merge_vector_map(vector_map<K, V>& lhs, vector_map<K, V>&& rhs) const {
    for (auto&& el : rhs) {
      lhs.emplace_back(correct(std::move(el)));
    }
  }

  template <typename K, typename V>
  void merge_vecvec(vecvec<K, V>& lhs, vecvec<K, V>&& rhs) const {
    for (auto&& el : rhs) {
      lhs.emplace_back(correct(std::move(el)));
    }
  }

  template <typename K, typename V>
  void merge_paged_vecvec(paged_vecvec<K, V>& lhs,
                          paged_vecvec<K, V>&& rhs) const {
    for (auto&& el : rhs) {
      lhs.emplace_back(correct(std::move(el)));
    }
  }

  template <typename K, typename V>
  size_t merge_hashmap(hash_map<K, V>& lhs, hash_map<K, V>&& rhs) const {
    size_t n_duplicates = 0;
    for (auto&& [k, v] : rhs) {
      auto const new_k = correct(k);
      if (lhs.find(new_k) == lhs.end()) {
        lhs.emplace(new_k, correct(std::move(v)));
      } else {
        ++n_duplicates;
      }
    }
    return n_duplicates;
  }

  void merge_bitvec(bitvec& lhs, bitvec const& rhs) const {
    size_t base = lhs.size();
    lhs.resize(base + rhs.size());
    for (size_t i = 0; i < rhs.size(); i++) {
      lhs.set(base + i, rhs.test(i));
    }
  }

private:
  vector_map<string_idx_t, string_idx_t> string_map;
  idx_offset_t bitfield_offset_{0};
  idx_offset_t location_offset_{0};
  idx_offset_t route_offset_{0};
  idx_offset_t route_id_offset_{0};
  idx_offset_t transport_offset_{0};
  idx_offset_t merged_trips_offset_{0};
  idx_offset_t timezone_offset_{0};
  idx_offset_t source_offset_{0};
  idx_offset_t source_file_offset_{0};
  idx_offset_t trip_offset_{0};
  idx_offset_t trip_id_offset_{0};
  idx_offset_t trip_direction_offset_{0};
  idx_offset_t trip_direction_string_offset_{0};
  idx_offset_t trip_line_offset_{0};
  idx_offset_t attribute_offset_{0};
  idx_offset_t attribute_combination_offset_{0};
  idx_offset_t provider_offset_{0};
  idx_offset_t area_offset_{0};
};

void merge_tables(timetable& lhs,
                  timetable&& rhs,
                  gtfs::trip_data& lhs_trip_data,
                  gtfs::trip_data& rhs_trip_data,
                  string_cache_t& str_cache,
                  std::mutex* cache_mutex) {
  index_manager ofs(lhs, rhs, str_cache, cache_mutex);

  assert(lhs.date_range_ == rhs.date_range_);

  {
    auto const n_duplicates = ofs.merge_hashmap<location_id, location_idx_t>(
        lhs.locations_.location_id_to_idx_,
        std::move(rhs.locations_.location_id_to_idx_));

    if (n_duplicates) {
      log(log_lvl::error, "merge_tables",
          "merge skipped {} duplicate stations.", n_duplicates);
    }
  }

  ofs.merge_vecvec<location_idx_t, char>(lhs.locations_.names_,
                                         std::move(rhs.locations_.names_));

  ofs.merge_vecvec<location_idx_t, char>(lhs.locations_.ids_,
                                         std::move(rhs.locations_.ids_));

  ofs.merge_vector_map<location_idx_t, geo::latlng>(
      lhs.locations_.coordinates_, std::move(rhs.locations_.coordinates_));

  ofs.merge_vector_map<location_idx_t, source_idx_t>(
      lhs.locations_.src_, std::move(rhs.locations_.src_));

  ofs.merge_vector_map<location_idx_t, u8_minutes>(
      lhs.locations_.transfer_time_, std::move(rhs.locations_.transfer_time_));

  ofs.merge_vector_map<location_idx_t, location_type>(
      lhs.locations_.types_, std::move(rhs.locations_.types_));

  ofs.merge_vector_map<location_idx_t, location_idx_t>(
      lhs.locations_.parents_, std::move(rhs.locations_.parents_));

  ofs.merge_vector_map<location_idx_t, timezone_idx_t>(
      lhs.locations_.location_timezones_,
      std::move(rhs.locations_.location_timezones_));

  ofs.merge_fws_multimap<location_idx_t, location_idx_t>(
      lhs.locations_.equivalences_, std::move(rhs.locations_.equivalences_));

  ofs.merge_fws_multimap<location_idx_t, location_idx_t>(
      lhs.locations_.children_, std::move(rhs.locations_.children_));

  ofs.merge_fws_multimap<location_idx_t, footpath>(
      lhs.locations_.preprocessing_footpaths_out_,
      std::move(rhs.locations_.preprocessing_footpaths_out_));

  ofs.merge_fws_multimap<location_idx_t, footpath>(
      lhs.locations_.preprocessing_footpaths_in_,
      std::move(rhs.locations_.preprocessing_footpaths_in_));

  for (size_t p = 0; p < lhs.locations_.footpaths_out_.size(); ++p) {
    ofs.merge_vecvec<location_idx_t, footpath>(
        lhs.locations_.footpaths_out_[p],
        std::move(rhs.locations_.footpaths_out_[p]));
    ofs.merge_vecvec<location_idx_t, footpath>(
        lhs.locations_.footpaths_in_[p],
        std::move(rhs.locations_.footpaths_in_[p]));
  }

  ofs.merge_vector_map<timezone_idx_t, timezone>(
      lhs.locations_.timezones_, std::move(rhs.locations_.timezones_));

  ofs.merge_vector(lhs.trip_id_to_idx_, std::move(rhs.trip_id_to_idx_));

  ofs.merge_fws_multimap<trip_idx_t, trip_id_idx_t>(lhs.trip_ids_,
                                                    std::move(rhs.trip_ids_));

  ofs.merge_vecvec<trip_id_idx_t, char>(lhs.trip_id_strings_,
                                        std::move(rhs.trip_id_strings_));

  ofs.merge_vector_map<trip_id_idx_t, source_idx_t>(
      lhs.trip_id_src_, std::move(rhs.trip_id_src_));

  ofs.merge_vector_map<trip_id_idx_t, uint32_t>(lhs.trip_train_nr_,
                                                std::move(rhs.trip_train_nr_));

  ofs.merge_vector_map<trip_idx_t, route_id_idx_t>(
      lhs.trip_route_id_, std::move(rhs.trip_route_id_));
  lhs.next_route_id_idx_ = ofs.correct(rhs.next_route_id_idx_);

  ofs.merge_paged_vecvec<trip_idx_t, transport_range_t>(
      lhs.trip_transport_ranges_, std::move(rhs.trip_transport_ranges_));

  ofs.merge_vecvec<trip_idx_t, stop_idx_t>(
      lhs.trip_stop_seq_numbers_, std::move(rhs.trip_stop_seq_numbers_));

  ofs.merge_fws_multimap<trip_idx_t, trip_debug>(lhs.trip_debug_,
                                                 std::move(rhs.trip_debug_));

  ofs.merge_vecvec<source_file_idx_t, char>(lhs.source_file_names_,
                                            std::move(rhs.source_file_names_));

  ofs.merge_vecvec<trip_idx_t, char>(lhs.trip_display_names_,
                                     std::move(rhs.trip_display_names_));

  ofs.merge_vector_map<route_idx_t, interval<transport_idx_t>>(
      lhs.route_transport_ranges_, std::move(rhs.route_transport_ranges_));

  ofs.merge_vecvec<route_idx_t, stop::value_type>(
      lhs.route_location_seq_, std::move(rhs.route_location_seq_));

  ofs.merge_vector_map<route_idx_t, clasz>(lhs.route_clasz_,
                                           std::move(rhs.route_clasz_));

  ofs.merge_vecvec<route_idx_t, clasz>(lhs.route_section_clasz_,
                                       std::move(rhs.route_section_clasz_));

  ofs.merge_bitvec(lhs.route_bikes_allowed_, rhs.route_bikes_allowed_);

  ofs.merge_vecvec<route_idx_t, bool>(
      lhs.route_bikes_allowed_per_section_,
      std::move(rhs.route_bikes_allowed_per_section_));

  ofs.merge_vecvec<location_idx_t, route_idx_t>(
      lhs.location_routes_, std::move(rhs.location_routes_));

  ofs.merge_vector_map<route_idx_t, interval<std::uint32_t>>(
      lhs.route_stop_time_ranges_, std::move(rhs.route_stop_time_ranges_));

  ofs.merge_vector(lhs.route_stop_times_, std::move(rhs.route_stop_times_));

  ofs.merge_vector_map<transport_idx_t, duration_t>(
      lhs.transport_first_dep_offset_,
      std::move(rhs.transport_first_dep_offset_));

  ofs.merge_vector_map<transport_idx_t, std::uint8_t>(
      lhs.initial_day_offset_, std::move(rhs.initial_day_offset_));

  ofs.merge_vector_map<transport_idx_t, bitfield_idx_t>(
      lhs.transport_traffic_days_, std::move(rhs.transport_traffic_days_));

  ofs.merge_vector_map<bitfield_idx_t, bitfield>(lhs.bitfields_,
                                                 std::move(rhs.bitfields_));

  ofs.merge_vector_map<transport_idx_t, route_idx_t>(
      lhs.transport_route_, std::move(rhs.transport_route_));

  ofs.merge_vecvec<transport_idx_t, merged_trips_idx_t>(
      lhs.transport_to_trip_section_,
      std::move(rhs.transport_to_trip_section_));

  ofs.merge_vecvec<merged_trips_idx_t, trip_idx_t>(
      lhs.merged_trips_, std::move(rhs.merged_trips_));

  ofs.merge_vector_map<attribute_idx_t, attribute>(lhs.attributes_,
                                                   std::move(rhs.attributes_));

  ofs.merge_vecvec<attribute_combination_idx_t, attribute_idx_t>(
      lhs.attribute_combinations_, std::move(rhs.attribute_combinations_));

  ofs.merge_vector_map<provider_idx_t, provider>(lhs.providers_,
                                                 std::move(rhs.providers_));

  ofs.merge_vecvec<trip_direction_string_idx_t, char>(
      lhs.trip_direction_strings_, std::move(rhs.trip_direction_strings_));

  ofs.merge_vector_map<trip_direction_idx_t, trip_direction_t>(
      lhs.trip_directions_, std::move(rhs.trip_directions_));

  ofs.merge_vecvec<trip_line_idx_t, char>(lhs.trip_lines_,
                                          std::move(rhs.trip_lines_));

  ofs.merge_vecvec<transport_idx_t, attribute_combination_idx_t>(
      lhs.transport_section_attributes_,
      std::move(rhs.transport_section_attributes_));

  ofs.merge_vecvec<transport_idx_t, provider_idx_t>(
      lhs.transport_section_providers_,
      std::move(rhs.transport_section_providers_));

  ofs.merge_vecvec<transport_idx_t, trip_direction_idx_t>(
      lhs.transport_section_directions_,
      std::move(rhs.transport_section_directions_));

  ofs.merge_vecvec<transport_idx_t, trip_line_idx_t>(
      lhs.transport_section_lines_, std::move(rhs.transport_section_lines_));

  ofs.merge_vecvec<transport_idx_t, route_color>(
      lhs.transport_section_route_colors_,
      std::move(rhs.transport_section_route_colors_));

  ofs.merge_vecvec<location_idx_t, footpath>(
      lhs.fwd_search_lb_graph_, std::move(rhs.fwd_search_lb_graph_));

  ofs.merge_vecvec<location_idx_t, footpath>(
      lhs.bwd_search_lb_graph_, std::move(rhs.bwd_search_lb_graph_));

  // There is currently no way to determine the correct profile offset from the
  // tables alone. Since they are currently not stored in the tables anyway,
  // this will need to be handled once they are.
  assert(lhs.profiles_.empty() && rhs.profiles_.empty());
  // ofs.merge_hashmap<string, profile_idx_t>(lhs.profiles_,
  // std::move(rhs.profiles_));

  ofs.merge_vector_map<source_idx_t, fares>(lhs.fares_, std::move(rhs.fares_));

  ofs.merge_vector_map<area_idx_t, area>(lhs.areas_, std::move(rhs.areas_));

  ofs.merge_vecvec<location_idx_t, area_idx_t>(lhs.location_areas_,
                                               std::move(rhs.location_areas_));

  ofs.merge_vector_map<gtfs::gtfs_trip_idx_t, trip>(
      lhs_trip_data.data_, std::move(rhs_trip_data.data_));
}

}  // namespace nigiri::loader