#include "gtest/gtest.h"

#include "nigiri/loader/load.h"
#include "nigiri/loader/loader_interface.h"
#include "nigiri/string_store.h"
#include "date/date.h"
#include "test_data.h"

using namespace nigiri;
using namespace nigiri::loader;
using namespace nigiri::loader::gtfs;
using namespace date;
using namespace std::string_view_literals;

namespace {

namespace testdata {
static loader_config constexpr default_cfg{.default_tz_ = "Europe/Berlin"};

static std::pair<std::string, loader_config> example =
    std::make_pair(example_files().get_content(), default_cfg);

static std::pair<std::string, loader_config> berlin =
    std::make_pair(berlin_files().get_content(), default_cfg);

}  // namespace testdata

void verify_timetable_sizes(timetable const& tt, timetable const& ref) {

#define verify_size(field) ASSERT_EQ(tt.field.size(), ref.field.size())

  verify_size(trip_id_to_idx_);
  verify_size(trip_ids_);
  verify_size(trip_id_strings_);
  verify_size(trip_id_src_);
  verify_size(trip_train_nr_);
  verify_size(trip_route_id_);
  verify_size(trip_transport_ranges_);
  verify_size(trip_stop_seq_numbers_);
  verify_size(trip_debug_);
  verify_size(source_file_names_);
  verify_size(trip_display_names_);
  verify_size(route_transport_ranges_);
  verify_size(route_location_seq_);
  verify_size(route_clasz_);
  verify_size(route_section_clasz_);
  verify_size(route_bikes_allowed_);
  verify_size(route_bikes_allowed_per_section_);
  verify_size(location_routes_);
  verify_size(route_stop_time_ranges_);
  verify_size(route_stop_times_);
  verify_size(transport_first_dep_offset_);
  verify_size(initial_day_offset_);
  verify_size(transport_traffic_days_);
  verify_size(bitfields_);
  verify_size(transport_route_);
  verify_size(transport_to_trip_section_);
  verify_size(merged_trips_);
  verify_size(attributes_);
  verify_size(attribute_combinations_);
  verify_size(providers_);
  verify_size(trip_direction_strings_);
  verify_size(trip_directions_);
  verify_size(trip_lines_);
  verify_size(transport_section_attributes_);
  verify_size(transport_section_providers_);
  verify_size(transport_section_directions_);
  verify_size(transport_section_lines_);
  verify_size(transport_section_route_colors_);
  verify_size(fwd_search_lb_graph_);
  verify_size(bwd_search_lb_graph_);
  verify_size(profiles_);
  verify_size(fares_);
  verify_size(areas_);
  verify_size(location_areas_);

  for (size_t i = 0; i < tt.fares_.size(); ++i) {
    ASSERT_EQ(tt.fares_[source_idx_t(i)].fare_media_.size(),
              ref.fares_[source_idx_t(i)].fare_media_.size());
  }

#undef verify_size
}

static timetable load_serial(
    std::vector<std::pair<std::string, loader_config>> paths) {

  interval<date::sys_days> date_range{date::sys_days{2024_y / March / 1},
                                      date::sys_days{2025_y / March / 2}};

  return serial_load(paths, {}, date_range, nullptr, nullptr, false);
}

static timetable load_parallel(
    std::vector<std::pair<std::string, loader_config>> paths) {

  interval<date::sys_days> date_range{date::sys_days{2024_y / March / 1},
                                      date::sys_days{2025_y / March / 2}};

  return serial_load(paths, {}, date_range, nullptr, nullptr, false);
}

static void compare_serial_parallel(
    std::vector<std::pair<std::string, loader_config>> paths) {

  auto tt_serial = load_serial(paths);
  auto tt_parallel = load_parallel(paths);

  verify_timetable_sizes(tt_serial, tt_parallel);
}

TEST(gtfs, merge_single_timetable) {
  compare_serial_parallel({testdata::example});
  compare_serial_parallel({testdata::berlin});
}

TEST(gtfs, merge_multiple_timetables) {
  compare_serial_parallel({testdata::example, testdata::berlin});
}

}  // namespace