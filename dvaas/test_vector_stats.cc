// Copyright 2024 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "dvaas/test_vector_stats.h"

#include <string>

#include "absl/algorithm/container.h"
#include "absl/strings/str_format.h"
#include "dvaas/test_vector.pb.h"
#include "gtest/gtest.h"

namespace dvaas {

namespace {

void AddTestVectorStats(const PacketTestOutcome& outcome,
                        TestVectorStats& stats) {
  stats.num_vectors += 1;

  if (outcome.test_result().has_failure()) {
    if (outcome.test_result().failure().has_minimization_analysis()) {
      stats.num_vectors_with_reproducibility_rate += 1;
      if (outcome.test_result()
              .failure()
              .minimization_analysis()
              .reproducibility_rate() == 1.0) {
        stats.num_deterministic_failures += 1;
      }
    }
  } else {
    stats.num_vectors_passed += 1;
  }

  const SwitchOutput& actual_output = outcome.test_run().actual_output();
  const int num_forwarded = actual_output.packets_size();
  const int num_punted = actual_output.packet_ins_size();

  bool has_correct_num_outputs = absl::c_any_of(
      outcome.test_run().test_vector().acceptable_outputs(),
      [&](const SwitchOutput& acceptable_output) {
        return num_forwarded == acceptable_output.packets_size() &&
               num_punted == acceptable_output.packet_ins_size();
      });
  if (has_correct_num_outputs) {
    stats.num_vectors_where_sut_produced_correct_number_of_outputs += 1;
  }

  stats.num_vectors_where_sut_forwarded_at_least_one_packet +=
      num_forwarded > 0 ? 1 : 0;
  stats.num_vectors_where_sut_punted_at_least_one_packet +=
      num_punted > 0 ? 1 : 0;
  stats.num_vectors_where_sut_produced_no_output +=
      num_forwarded == 0 && num_punted == 0;
  stats.num_packets_forwarded += num_forwarded;
  stats.num_packets_punted += num_punted;

  // Compute expectation stats.
  bool forwarding_allowed =
      absl::c_any_of(outcome.test_run().test_vector().acceptable_outputs(),
                     [](const SwitchOutput& acceptable_output) {
                       return acceptable_output.packets_size() > 0;
                     });
  bool forwarding_required =
      absl::c_all_of(outcome.test_run().test_vector().acceptable_outputs(),
                     [](const SwitchOutput& acceptable_output) {
                       return acceptable_output.packets_size() > 0;
                     });
  bool punting_allowed =
      absl::c_any_of(outcome.test_run().test_vector().acceptable_outputs(),
                     [](const SwitchOutput& acceptable_output) {
                       return acceptable_output.packet_ins_size() > 0;
                     });
  bool punting_required =
      absl::c_all_of(outcome.test_run().test_vector().acceptable_outputs(),
                     [](const SwitchOutput& acceptable_output) {
                       return acceptable_output.packet_ins_size() > 0;
                     });
  bool no_output_allowed =
      absl::c_any_of(outcome.test_run().test_vector().acceptable_outputs(),
                     [](const SwitchOutput& acceptable_output) {
                       return acceptable_output.packets_size() == 0 &&
                              acceptable_output.packet_ins_size() == 0;
                     });
  bool no_output_required =
      absl::c_all_of(outcome.test_run().test_vector().acceptable_outputs(),
                     [](const SwitchOutput& acceptable_output) {
                       return acceptable_output.packets_size() == 0 &&
                              acceptable_output.packet_ins_size() == 0;
                     });
  stats.max_vectors_with_forwarding_expected += forwarding_allowed ? 1 : 0;
  stats.min_vectors_with_forwarding_expected += forwarding_required ? 1 : 0;
  stats.max_vectors_with_punting_expected += punting_allowed ? 1 : 0;
  stats.min_vectors_with_punting_expected += punting_required ? 1 : 0;
  stats.max_vectors_with_no_output_expected += no_output_allowed ? 1 : 0;
  stats.min_vectors_with_no_output_expected += no_output_required ? 1 : 0;
}

}  // namespace

TestVectorStats ComputeTestVectorStats(
    const PacketTestOutcomes& test_outcomes) {
  TestVectorStats stats;
  for (const auto& outcome : test_outcomes.outcomes()) {
    AddTestVectorStats(outcome, stats);
  }
  return stats;
}

namespace {

std::string ExplainPercent(double fraction) {
  return absl::StrFormat("%.2f%%", fraction * 100);
}
std::string ExplainPercent(int numerator, int denominator) {
  if (denominator == 0) return "100%";
  return ExplainPercent(static_cast<double>(numerator) / denominator);
}

std::string ExplainFraction(int numerator, int denominator) {
  return absl::StrFormat("%d (%s) of %d", numerator,
                         ExplainPercent(numerator, denominator), denominator);
}

std::string ExplainRange(int min, int max) {
  if (min == max) return absl::StrFormat("%d", min);
  return absl::StrFormat("%d - %d", min, max);
}

}  // namespace

std::string ExplainTestVectorStats(const TestVectorStats& stats) {
  std::string result;

  absl::StrAppendFormat(
      &result, "%s test vectors passed\nThe System Under Test (SUT):\n",
      ExplainFraction(stats.num_vectors_passed, stats.num_vectors));

  absl::StrAppendFormat(
      &result,
      "- forwarded:                          for %d test vectors "
      "(expected: %s)\n",
      stats.num_vectors_where_sut_forwarded_at_least_one_packet,
      ExplainRange(stats.min_vectors_with_forwarding_expected,
                   stats.max_vectors_with_forwarding_expected));
  absl::StrAppendFormat(
      &result,
      "- punted:                             for %d test vectors "
      "(expected: %s)\n",
      stats.num_vectors_where_sut_punted_at_least_one_packet,
      ExplainRange(stats.min_vectors_with_punting_expected,
                   stats.max_vectors_with_punting_expected));
  absl::StrAppendFormat(
      &result,
      "- dropped:                            for %d test vectors "
      "(expected: %s)\n",
      stats.num_vectors_where_sut_produced_no_output,
      ExplainRange(stats.min_vectors_with_no_output_expected,
                   stats.max_vectors_with_no_output_expected));

  // If some test vectors didn't pass, but came close, explain how many.
  const int num_almost_passed =
      stats.num_vectors_where_sut_produced_correct_number_of_outputs -
      stats.num_vectors_passed;
  if (num_almost_passed > 0) {
    absl::StrAppendFormat(
        &result,
        "- came close* to expected behavior:   for %d failing test vectors\n"
        "  (*produced expected number of forwarded/punted packets, but with "
        "unexpected header/payload values)\n",
        num_almost_passed);
  }

  return result;
}

void RecordStatsAsGoogleTestProperties(const TestVectorStats& stats) {
  using ::testing::Test;
  Test::RecordProperty("tag_num_vectors", stats.num_vectors);
  Test::RecordProperty("tag_num_vectors_passed", stats.num_vectors_passed);
  Test::RecordProperty(
      "tag_pass_percentage",
      ExplainPercent(stats.num_vectors_passed, stats.num_vectors));
  Test::RecordProperty(
      "tag_num_vectors_where_sut_produced_correct_number_of_outputs",
      stats.num_vectors_where_sut_produced_correct_number_of_outputs);
  Test::RecordProperty(
      "tag_num_vectors_where_sut_forwarded_at_least_one_packet",
      stats.num_vectors_where_sut_forwarded_at_least_one_packet);
  Test::RecordProperty("tag_num_vectors_where_sut_punted_at_least_one_packet",
                       stats.num_vectors_where_sut_punted_at_least_one_packet);
  Test::RecordProperty("tag_num_vectors_where_sut_produced_no_output",
                       stats.num_vectors_where_sut_produced_no_output);
  Test::RecordProperty("tag_num_packets_forwarded",
                       stats.num_packets_forwarded);
  Test::RecordProperty("tag_num_packets_punted", stats.num_packets_punted);
  Test::RecordProperty("tag_num_deterministic_failures",
                       stats.num_deterministic_failures);
  Test::RecordProperty("tag_num_vectors_with_reproducibility_rate",
                       stats.num_vectors_with_reproducibility_rate);
}

}  // namespace dvaas
