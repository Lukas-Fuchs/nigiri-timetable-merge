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

  bool map_locations() {
    return map_leaves<location_idx_t>(
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
  }

  bool map_trips() {
    return map_leaves<trip_id_idx_t>(
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
               })  //
           &&  //
           map_leaves<trip_idx_t>(
               lhs_.trip_display_names_.size(), rhs_.trip_display_names_.size(),
               [&](trip_idx_t const l, trip_idx_t const r) {
                 return lhs_.trip_display_names_[l].view() ==
                        rhs_.trip_display_names_[r].view();
               },
               [&](trip_idx_t const l, trip_idx_t const r) {
                 trip_map_[l] = r;
               },
               [&](trip_idx_t const l) {
                 std::cout << "Trip " << lhs_.trip_display_names_[l].view()
                           << " is missing in right hand table\n";
               });
  }

  bool map_source_files() {
    return map_leaves<source_file_idx_t>(
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
  }

  bool map_providers() {
    auto cmp_providers = [](provider l, provider r) -> bool {
      l.tz_ = r.tz_;  // The timezone index is table-dependent and may differ.
      return l == r;
    };

    return map_leaves<provider_idx_t>(
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
  }

public:
  bool map_and_check_leaves() {
    if (!map_locations()) {
      std::cout << "Timetable mismatch: Locations\n";
      return false;
    }
    if (!map_trips()) {
      std::cout << "Timetable mistmatch: Trips\n";
      return false;
    }
    if (!map_source_files()) {
      std::cout << "Timetable mismatch: Source files\n";
      return false;
    }
    if (!map_providers()) {
      std::cout << "Timetable mismatch: Providers\n";
      return false;
    }
    return true;
  }

private:
  timetable const& lhs_;
  timetable const& rhs_;

  // Below mappings translate from lhs_ indices to rhs_ indices.
  std::unordered_map<location_idx_t, location_idx_t> location_map_;
  std::unordered_map<trip_idx_t, trip_idx_t> trip_map_;
  std::unordered_map<trip_id_idx_t, trip_id_idx_t> trip_id_map_;
  std::unordered_map<source_file_idx_t, source_file_idx_t> source_file_map_;
  std::unordered_map<provider_idx_t, provider_idx_t> provider_map_;
};

bool compare_timetables(timetable const& lhs, timetable const& rhs) {
  compare_state cs(lhs, rhs);
  if (lhs.date_range_ != rhs.date_range_) {
    std::cout << "Timetable mismatch: Date range\n";
    return false;
  }

  if (!cs.map_and_check_leaves()) {
    std::cout << "Timetable mismatch: Basic elements are different\n";
    return false;
  }

  return true;
}