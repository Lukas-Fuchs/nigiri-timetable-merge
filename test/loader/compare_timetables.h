#pragma once

#include <deque>
#include <exception>
#include <functional>
#include <iostream>
#include <unordered_map>

#include "nigiri/timetable.h"
#include "utl/enumerate.h"

using namespace nigiri;

struct compare_state {
  compare_state(timetable const& lhs, timetable const& rhs)
      : lhs_(lhs), rhs_(rhs) {}

private:
  template <typename IDX_T>
  bool map_leaves(size_t const l_size,
                  size_t const r_size,
                  std::function<bool(IDX_T const, IDX_T const)> const& match,
                  std::function<void(IDX_T const, IDX_T const)> const& success,
                  std::function<void(IDX_T const)> const& failure) {
    for (size_t l_raw = 0; l_raw < l_size; ++l_raw) {
      IDX_T const l(l_raw);
      bool found = false;
      for (size_t r_raw = 0; r_raw < r_size; ++r_raw) {
        IDX_T const r(r_raw);
        if (match(l, r)) {
          success(l, r);
          found = true;
          break;
        }
      }
      if (!found) {
        failure(l);
        return false;
      }
    }
    return true;
  }

  bool create_mappings() {
    ///////////////// TRIP IDS /////////////////
    {
      bool const success = map_leaves<trip_id_idx_t>(
          lhs_.trip_id_strings_.size(), rhs_.trip_id_strings_.size(),
          [&](trip_id_idx_t const l, trip_id_idx_t const r) {
            return lhs_.trip_id_strings_[l].view() ==
                       rhs_.trip_id_strings_[r].view()  //
                   &&  //
                   lhs_.trip_train_nr_[l] == rhs_.trip_train_nr_[r];
          },
          [&](trip_id_idx_t const l, trip_id_idx_t const r) {
            trip_id_map_[l] = r;
          },
          [&](trip_id_idx_t const l) {
            std::cout << "Trip ID " << lhs_.trip_id_strings_[l].view()
                      << " (Train No.: " << lhs_.trip_train_nr_[l]
                      << ") is missing in right hand table\n";
          });

      if (!success) {
        std::cout << "Table mismatch: Trip IDs\n";
        return false;
      }
    }

    ///////////////// TRIPS /////////////////
    {
      bool const success = map_leaves<trip_idx_t>(
          lhs_.trip_ids_.size(), rhs_.trip_ids_.size(),
          [&](trip_idx_t const l, trip_idx_t const r) -> bool {
            return !lhs_.trip_ids_[l].empty() && !rhs_.trip_ids_[r].empty() &&
                   trip_id_map_.at(lhs_.trip_ids_[l].front()) ==
                       rhs_.trip_ids_[r].front();
          },
          [&](trip_idx_t const l, trip_idx_t const r) { trip_map_[l] = r; },
          [&](trip_idx_t const l) {
            std::cout << "Trip " << lhs_.trip_display_names_[l].view()
                      << " is missing in right hand table\n";
          });

      if (!success) {
        std::cout << "Table mismatch: Trips\n";
        return false;
      }
    }

    ///////////////// SOURCES /////////////////
    for (auto const& [l, r] : trip_id_map_) {
      source_map_[lhs_.trip_id_src_[l]] = rhs_.trip_id_src_[r];
    }
    source_map_[source_idx_t::invalid()] = source_idx_t::invalid();

    ///////////////// SOURCE FILES /////////////////
    {
      bool const success = map_leaves<source_file_idx_t>(
          lhs_.source_file_names_.size(), rhs_.source_file_names_.size(),
          [&](source_file_idx_t const l, source_file_idx_t const r) {
            return lhs_.source_file_names_[l].view() ==
                   rhs_.source_file_names_[r].view();
          },
          [&](source_file_idx_t const l, source_file_idx_t const r) {
            source_file_map_[l] = r;
          },
          [&](source_file_idx_t const l) {
            std::cout << "Source file " << lhs_.source_file_names_[l].view()
                      << " is missing in right hand table\n";
          });

      if (!success) {
        std::cout << "Table mismatch: Source files\n";
        return false;
      }
    }

    ///////////////// LOCATIONS /////////////////
    {
      bool success = true;
      for (auto const& [l_id, l] : lhs_.locations_.location_id_to_idx_) {

        location_id r_id{.id_ = l_id.id_, .src_ = source_map_.at(l_id.src_)};
        auto const it_r = rhs_.locations_.location_id_to_idx_.find(r_id);
        if (it_r == rhs_.locations_.location_id_to_idx_.end()) {
          std::cout << "Location " << lhs_.locations_.names_[l].view()
                    << " (source index " << l_id.src_
                    << ") is missing in right hand table\n";
          success = false;
          break;
        }
        location_map_[l] = it_r->second;
      }

      if (!success) {
        std::cout << "Table mismatch: Locations\n";
        return false;
      }
    }

    ///////////////// ROUTE IDS /////////////////
    for (auto const& [l, r] : trip_map_) {
      route_id_map_[lhs_.trip_route_id_[l]] = rhs_.trip_route_id_[r];
    }

    ///////////////// PROVIDERS /////////////////
    {
      auto cmp_providers = [](provider l, provider r) -> bool {
        l.tz_ = r.tz_;  // The timezone index is table-dependent and may differ.
        return l == r;
      };

      bool const success = map_leaves<provider_idx_t>(
          lhs_.providers_.size(), rhs_.providers_.size(),
          [&](provider_idx_t const l, provider_idx_t const r) {
            return cmp_providers(lhs_.providers_[l], rhs_.providers_[r]);
          },
          [&](provider_idx_t const l, provider_idx_t const r) {
            provider_map_[l] = r;
          },
          [&](provider_idx_t const l) {
            std::cout << "Provider " << lhs_.providers_[l]
                      << " is missing in right hand table\n";
          });

      if (!success) {
        std::cout << "Table mismatch: Providers\n";
        return false;
      }
    }

    ///////////////// TRANSPORTS /////////////////
    // This mapping is built according to references from other index types and
    // will be verified later.
    {
      bool const success = map_leaves<transport_idx_t>(
          lhs_.transport_traffic_days_.size(),
          rhs_.transport_traffic_days_.size(),
          [&](transport_idx_t l, transport_idx_t r) {
            auto const l_dbg = lhs_.dbg(l);
            auto const r_dbg = rhs_.dbg(r);
            bool const match = l_dbg.path_ == r_dbg.path_ &&
                               l_dbg.line_from_ == r_dbg.line_from_ &&
                               l_dbg.line_to_ == r_dbg.line_to_ &&
                               lhs_.transport_first_dep_offset_[l] ==
                                   rhs_.transport_first_dep_offset_[r] &&
                               lhs_.transport_traffic_days_[l] ==
                                   rhs_.transport_traffic_days_[r];

            if (match && transport_map_.contains(l)) {
              std::cout << "Ambiguous transport mapping. This is probably a "
                           "limitation "
                           "of the test, not a bug.\n";
              std::cout << "Transport " << l << " would be mapped to "
                        << transport_map_[l] << " and " << r << ".\n";
              return false;
            }
            return match;
          },
          [&](transport_idx_t l, transport_idx_t r) { transport_map_[l] = r; },
          [&](transport_idx_t const l) {
            auto const l_dbg = lhs_.dbg(l);
            std::cout << "Transport at " << l_dbg.path_ << ", L."
                      << l_dbg.line_from_ << "-" << l_dbg.line_to_
                      << " is missing in right hand table\n";
          });

      if (!success) {
        return false;
      }
    }

    ///////////////// ROUTES /////////////////
    for (auto const& [l, r] : transport_map_) {
      auto const l_route = lhs_.transport_route_[l];
      // This is a one-to-many mapping, which is fine but it must be verified
      // that transports referencing the same route in one table also map to the
      // same route in the other table.
      if (auto it = route_map_.find(l_route); it != route_map_.end()) {
        if (it->second != rhs_.transport_route_[r]) {
          std::cout << "Conflicting route mappings from transports.\n";
          return false;
        }
        continue;
      }
      route_map_[l_route] = rhs_.transport_route_[r];
    }

    return true;
  }

  bool check_relations() const {
    ///////////////// Trip Features /////////////////

    for (auto const& [l, r] : trip_map_) {
      if (lhs_.trip_ids_[l].size() != rhs_.trip_ids_[r].size()) {
        std::cout << "Mismatching number of external trip IDs.\n";
        return false;
      }

      for (auto const& [l_stop, r_stop] :
           utl::zip(lhs_.trip_stop_seq_numbers_[l],
                    rhs_.trip_stop_seq_numbers_[r])) {
        if (l_stop != r_stop) {
          std::cout << "Mismatching trip stop sequences.\n ";
          return false;
        }
      }

      for (auto const& [l_debug, r_debug] :
           utl::zip(lhs_.trip_debug_[l], rhs_.trip_debug_[r])) {
        if (source_file_map_.at(l_debug.source_file_idx_) !=
            r_debug.source_file_idx_) {
          std::cout << "Mismatching source file index: "
                    << source_file_map_.at(l_debug.source_file_idx_)
                    << " != " << r_debug.source_file_idx_ << "\n";
        }

        if ((r_debug.source_file_idx_ !=
             source_file_map_.at(l_debug.source_file_idx_)) ||
            (r_debug.line_number_from_ != l_debug.line_number_from_) ||
            (r_debug.line_number_to_ != l_debug.line_number_to_)) {
          std::cout << "Mismatching debugging information for trip "
                    << lhs_.trip_display_names_[l].view() << ".\n";
          std::cout << " - File: "
                    << lhs_.source_file_names_[l_debug.source_file_idx_].view()
                    << "(" << l_debug.source_file_idx_ << ")" << "\t|\t"
                    << rhs_.source_file_names_[r_debug.source_file_idx_].view()
                    << "(" << r_debug.source_file_idx_ << "/"
                    << source_file_map_.at(l_debug.source_file_idx_) << ")"
                    << "\n";
          std::cout << " - Lines: " << l_debug.line_number_from_ << "-"
                    << l_debug.line_number_to_ << "\t|\t"
                    << r_debug.line_number_from_ << "-"
                    << r_debug.line_number_to_ << std::endl;
          return false;
        }
      }

      if (lhs_.trip_display_names_[l].view() !=
          rhs_.trip_display_names_[r].view()) {
        std::cout << "Mismatching trip display names.\n";
        return false;
      }
    }

    ///////////////// Route Features /////////////////
    for (auto const& [l, r] : route_map_) {

      {
        auto const& l_transport = lhs_.route_transport_ranges_[l];
        auto const& r_transport = rhs_.route_transport_ranges_[r];
        if ((r_transport.from_ != transport_map_.at(l_transport.from_)) ||
            (r_transport.to_ - 1 != transport_map_.at(l_transport.to_ - 1))) {
          std::cout << "Mismatching route transport ranges.\n";
          return false;
        }
      }

      for (auto const& [l_stop, r_stop] :
           utl::zip(lhs_.route_location_seq_[l], rhs_.route_location_seq_[r])) {
        if (l_stop != r_stop) {
          std::cout << "Mismatching route stop sequences.\n";
          return false;
        }
      }

      if (lhs_.route_clasz_[l] != rhs_.route_clasz_[r]) {
        std::cout << "Mismatching route clasz.\n";
        return false;
      }

      for (auto const& [l_clasz, r_clasz] : utl::zip(
               lhs_.route_section_clasz_[l], rhs_.route_section_clasz_[r])) {
        if (l_clasz != r_clasz) {
          std::cout << "Mismatching route section clasz.\n";
          return false;
        }
      }

      {
        uint32_t const l_bike_idx(l * 2);
        uint32_t const r_bike_idx(r * 2);
        if (lhs_.route_bikes_allowed_.test(l_bike_idx) !=
            rhs_.route_bikes_allowed_.test(r_bike_idx)) {
          std::cout << "Mismatching route bikes allowed.\n";
          return false;
        }

        if (lhs_.route_bikes_allowed_.test(l_bike_idx + 1) !=
            rhs_.route_bikes_allowed_.test(r_bike_idx + 1)) {
          std::cout
              << "Mismatching route bikes allowed route/section policy.\n";
          return false;
        }

        if (lhs_.route_bikes_allowed_.test(l_bike_idx + 1)) {
          for (auto const& [l_bike, r_bike] :
               utl::zip(lhs_.route_bikes_allowed_per_section_[l],
                        rhs_.route_bikes_allowed_per_section_[r])) {
            if (l_bike != r_bike) {
              std::cout << "Mismatching bike section in route.\n";
              return false;
            }
          }
        }
      }

      if (lhs_.route_stop_time_ranges_[l] != rhs_.route_stop_time_ranges_[r]) {
        std::cout << "Mismatching route stop time ranges.\n";
        return false;
      }
    }

    ///////////////// Location Features /////////////////

    for (auto const& [l, r] : location_map_) {

      if (lhs_.locations_.names_[l].view() !=
          rhs_.locations_.names_[r].view()) {
        std::cout << "Mismatching location names: "
                  << lhs_.locations_.names_[l].view()
                  << " != " << rhs_.locations_.names_[r].view() << "\n";
        return false;
      }

      if (lhs_.locations_.ids_[l].view() != rhs_.locations_.ids_[r].view()) {
        std::cout << "Mismatching location IDs: "
                  << lhs_.locations_.ids_[l].view()
                  << " != " << rhs_.locations_.ids_[r].view() << "\n";
        return false;
      }

      if (lhs_.locations_.coordinates_[l] != rhs_.locations_.coordinates_[r]) {
        std::cout << "Mismatching location coordinates";
        return false;
      }

      if (source_map_.at(lhs_.locations_.src_[l]) != rhs_.locations_.src_[r]) {
        std::cout << "Mismatching location sources";
        return false;
      }

      if (lhs_.locations_.transfer_time_[l] !=
          rhs_.locations_.transfer_time_[r]) {
        std::cout << "Mismatching location transfer times";
        return false;
      }

      if (lhs_.locations_.types_[l] != rhs_.locations_.types_[r]) {
        std::cout << "Mismatching location types";
        return false;
      }

      auto const tz_name = [](timetable const& tt,
                              location_idx_t loc) -> string {
        auto const tz_idx = tt.locations_.location_timezones_[loc];
        if (tz_idx == timezone_idx_t::invalid()) {
          // These are permitted so this causing the test to pass is fine.
          return "<INVALID>";
        }
        auto const& tz = tt.locations_.timezones_[tz_idx];
        if (!holds_alternative<pair<string, void const*>>(tz)) {
          throw std::runtime_error("Unexpected timezone type");
        }
        return tz.as<pair<string, void const*>>().first;
      };

      if (tz_name(lhs_, l) != tz_name(rhs_, r)) {
        std::cout << "Mismatching location time zones";
        return false;
      }

      for (auto const& l_eq : lhs_.locations_.equivalences_[l]) {
        auto const expected_r_eq = location_map_.at(l_eq);
        bool found = false;
        for (auto const& r_eq : rhs_.locations_.equivalences_[r]) {
          if (r_eq == expected_r_eq) {
            found = true;
            break;
          }
        }
        if (!found) {
          std::cout << "Mismatching location equivalences: " << expected_r_eq
                    << " not in\n\t[";
          for (auto const& r_eq : rhs_.locations_.equivalences_[r]) {
            std::cout << r_eq << " ";
          }
          std::cout << "]\n";
          return false;
        }
      }

      for (auto const& l_child : lhs_.locations_.children_[l]) {
        auto const expected_r_child = location_map_.at(l_child);
        bool found = false;
        for (auto const& r_child : rhs_.locations_.children_[r]) {
          if (r_child == expected_r_child) {
            found = true;
            break;
          }
        }
        if (!found) {
          std::cout << "Child mismatch for location "
                    << lhs_.locations_.names_[l].view() << ":\n\tL: ";
          for (auto const l_child : lhs_.locations_.children_[l]) {
            std::cout << l_child << " ";
          }
          std::cout << "\n\tR: ";
          for (auto const r_child : rhs_.locations_.children_[r]) {
            std::cout << r_child << " ";
          }

          return false;
        }

        auto check_parent_child_relation = [&](timetable const& tt,
                                               location_idx_t parent,
                                               location_idx_t child) -> bool {
          if (tt.locations_.parents_[child] != parent) {
            std::cout << "Asymmetric location parent->child relation: "
                      << tt.locations_.names_[parent].view() << " -> "
                      << lhs_.locations_.names_[l].view() << "\n";
            return false;
          }
          return true;
        };

        auto const r_child = location_map_.at(l_child);
        if (!check_parent_child_relation(lhs_, l, l_child) ||
            !check_parent_child_relation(rhs_, r, r_child)) {
          return false;
        }
      }

      for (auto const& [l_route, r_route] :
           utl::zip(lhs_.location_routes_[l], rhs_.location_routes_[r])) {
        if (route_map_.at(l_route) != r_route) {
          std::cout << "Mismatching routes for location "
                    << lhs_.locations_.names_[l].view() << "\n";
          return false;
        }
      }

      // Anything related to footpaths is either created or deleted during
      // finalization and may not be a perfect combination of the constituents.
      // Footpaths are therefore ignored for now.
      // TODO: Maybe perform the Dijkstra test on merged tables to verify the
      // correctness of search graphs.
    }

    return true;
  }

public:
  bool operator()() {
    if (lhs_.date_range_ != rhs_.date_range_) {
      std::cout << "Timetable mismatch: Date range\n";
      return false;
    }
    return create_mappings() && check_relations();
  }

private:
  timetable const& lhs_;
  timetable const& rhs_;

  // Below mappings translate from lhs_ indices to rhs_ indices.
  std::unordered_map<location_idx_t, location_idx_t> location_map_;
  std::unordered_map<trip_idx_t, trip_idx_t> trip_map_;
  std::unordered_map<trip_id_idx_t, trip_id_idx_t> trip_id_map_;
  std::unordered_map<route_idx_t, route_idx_t> route_map_;
  std::unordered_map<route_id_idx_t, route_id_idx_t> route_id_map_;
  std::unordered_map<transport_idx_t, transport_idx_t> transport_map_;
  std::unordered_map<source_idx_t, source_idx_t> source_map_;
  std::unordered_map<source_file_idx_t, source_file_idx_t> source_file_map_;
  std::unordered_map<provider_idx_t, provider_idx_t> provider_map_;
};

bool compare_timetables(timetable const& lhs, timetable const& rhs) {
  return compare_state(lhs, rhs)();
}