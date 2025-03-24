#pragma once

#include <deque>
#include <exception>
#include <functional>
#include <iostream>
#include <unordered_map>
#include <variant>

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
    ///////////////// LOCATIONS /////////////////
    {
      bool const success = map_leaves<location_idx_t>(
          lhs_.locations_.names_.size(), rhs_.locations_.names_.size(),
          [&](location_idx_t const l, location_idx_t const r) {
            return (lhs_.locations_.names_[l].view() ==
                    rhs_.locations_.names_[r].view())  //
                   &&  //
                   (lhs_.locations_.ids_[l].view() ==
                    rhs_.locations_.ids_[r].view());
          },
          [&](location_idx_t const l, location_idx_t const r) {
            location_map_[l] = r;
          },
          [&](location_idx_t const l) {
            std::cout << "Location " << lhs_.locations_.names_[l].view() << " ("
                      << lhs_.locations_.ids_[l].view()
                      << ") is missing in right hand table\n";
          });

      if (!success) {
        std::cout << "Table mismatch: Locations\n";
        return false;
      }
    }

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

    ///////////////// ROUTE IDS /////////////////
    for (auto const& [l, r] : trip_map_) {
      route_id_map_[lhs_.trip_route_id_[l]] = rhs_.trip_route_id_[r];
    }

    ///////////////// SOURCES /////////////////
    for (auto const& [l, r] : trip_id_map_) {
      source_map_[lhs_.trip_id_src_[l]] = rhs_.trip_id_src_[r];
    }

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
    // This mapping is build according to references from other index types and
    // will be verified later.
    for (auto const& [l, r] : trip_map_) {
      // NOTE: This only works under the assumption that the transport indices
      // are always in the same order, which is the case as long as there is no
      // concurrency within individual files' loading routines. If this fails
      // due to order mismatches, that might be the reason.
      for (auto const& [l_transport, r_transport] :
           utl::zip(lhs_.trip_transport_ranges_[l],
                    rhs_.trip_transport_ranges_[r])) {
        transport_map_[l_transport.first] = r_transport.first;
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
    for (auto const& [l, r] : location_map_) {
      auto const& l_routes = lhs_.location_routes_[l];
      auto const& r_routes = rhs_.location_routes_[r];
      if (l_routes.size() != r_routes.size()) {
        std::cout << "Tables have different number of routes for location "
                  << lhs_.locations_.names_[l].view() << ": " << l_routes.size()
                  << " != " << r_routes.size() << "\n";
        return false;
      }

      for (auto const& [l_route, r_route] : utl::zip(l_routes, r_routes)) {
        if (route_map_.at(l_route) != r_route) {
          std::cout << "Mismatching routes for location "
                    << lhs_.locations_.names_[l].view() << "\n";
          return false;
        }
      }
    }

    for (size_t i = 0; i < lhs_.trip_stop_seq_numbers_.size(); ++i) {
      trip_idx_t idx(i);
      auto const& l_stops = lhs_.trip_stop_seq_numbers_[idx];
      auto const& r_stops = rhs_.trip_stop_seq_numbers_[trip_map_.at(idx)];

      for (auto const& [l, r] : utl::zip(l_stops, r_stops)) {
        if (l != r) {
          std::cout << "Mismatching stop sequences.\n ";
          return false;
        }
      }
    }

    for (auto const& [l, r] : trip_map_) {
      if (lhs_.trip_ids_[l].size() != rhs_.trip_ids_[r].size()) {
        std::cout << "Mismatching number of external trip IDs.\n";
        return false;
      }
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