/* Copyright 2024 The OpenXLA Authors.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#include "xla/service/memory_space_assignment/simulator.h"

#include <cstdint>
#include <list>
#include <map>
#include <memory>
#include <string_view>

#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include "absl/log/log.h"
#include "absl/status/status.h"
#include "absl/strings/string_view.h"
#include "xla/hlo/ir/hlo_instruction.h"
#include "xla/hlo/ir/hlo_module.h"
#include "xla/hlo/utils/hlo_live_range.h"
#include "xla/service/hlo_alias_analysis.h"
#include "xla/service/hlo_cost_analysis.h"
#include "xla/service/memory_space_assignment/allocation.h"
#include "xla/service/memory_space_assignment/cost_analysis.h"
#include "xla/shape.h"
#include "xla/shape_util.h"
#include "xla/tests/hlo_test_base.h"
#include "tsl/lib/core/status_test_util.h"
#include "tsl/platform/errors.h"
#include "tsl/platform/statusor.h"

namespace xla {
namespace {

using memory_space_assignment::CostAnalysis;
using memory_space_assignment::CostAnalysisOptions;
using memory_space_assignment::RuntimeSimulator;

using ::testing::ElementsAreArray;
using ::testing::IsEmpty;

constexpr int64_t kPointerSize = 8;
constexpr int64_t kAlternateMemorySpace = 1;

int64_t ShapeSize(const Shape& shape) {
  return ShapeUtil::ByteSizeOf(shape, kPointerSize);
}

class MemorySpaceAssignmentSimulatorTest : public HloTestBase {
 protected:
  absl::Status Initialize(absl::string_view hlo_string) {
    TF_ASSIGN_OR_RETURN(module_, ParseAndReturnVerifiedModule(hlo_string));
    HloCostAnalysis::Options tpu_device_options;
    tpu_device_options.shape_size = ShapeSize;
    // Assume 1 FLOP per second for testing.
    tpu_device_options.set_flops_per_second(1);
    // Assume 1 byte per second for testing.
    tpu_device_options.set_bytes_per_second(1);
    hlo_cost_analysis_ = std::make_unique<HloCostAnalysis>(tpu_device_options);
    TF_RETURN_IF_ERROR(
        module_->entry_computation()->Accept(hlo_cost_analysis_.get()));
    hlo_cost_analysis_costs_ =
        std::make_unique<memory_space_assignment::HloCostAnalysisCosts>(
            *hlo_cost_analysis_);
    CostAnalysisOptions _options;
    TF_ASSIGN_OR_RETURN(
        cost_analysis_,
        CostAnalysis::Create(*hlo_cost_analysis_costs_, _options, *module_));
    runtime_simulator_ = std::make_unique<RuntimeSimulator>(
        cost_analysis_.get(), kAlternateMemorySpace);
    return absl::OkStatus();
  }
  std::unique_ptr<HloCostAnalysis> hlo_cost_analysis_;
  std::unique_ptr<memory_space_assignment::HloCostAnalysisCosts>
      hlo_cost_analysis_costs_;
  std::unique_ptr<CostAnalysis> cost_analysis_;
  std::unique_ptr<RuntimeSimulator> runtime_simulator_;
  std::unique_ptr<HloModule> module_;
};

TEST_F(MemorySpaceAssignmentSimulatorTest, SingleLayerNestedLoop) {
  absl::string_view hlo_string =
      R"(HloModule module, is_scheduled=true

      %body {
        %constant.1 = s32[] constant(1)
        %param = (s32[]) parameter(0)
        %count = s32[] get-tuple-element(%param), index=0
        %increment = s32[] add(s32[] %count, s32[] %constant.1)
        ROOT %loop_result = (s32[]) tuple(%increment)
      }

      %condition {
        %param = (s32[]) parameter(0)
        %constant.42 = s32[] constant(42)
        %condition_input = s32[] get-tuple-element(%param), index=0
        ROOT %greater = pred[] compare(s32[] %constant.42, s32[] %condition_input), direction=GT
      }

      ENTRY Entry {
        %dummy_input = s32[] parameter(0)
        %constant.0 = s32[] constant(0)
        ROOT %while = (s32[]) while(tuple(%constant.0)), condition=%condition, body=%body
      }

    )";
  TF_ASSERT_OK(Initialize(hlo_string));
  TF_ASSERT_OK_AND_ASSIGN(auto alias_analysis,
                          HloAliasAnalysis::Run(module_.get()));
  TF_ASSERT_OK_AND_ASSIGN(
      auto hlo_live_range,
      HloLiveRange::Run(module_->schedule(), *alias_analysis,
                        module_->entry_computation()));

  // Since the HLO does not contain memory access, pass an empty allocation
  // sequence for test.
  memory_space_assignment::AllocationSequence allocations;
  // The while loop has 42 iterations, and each iteration has 2 FLOP (for
  // %increment and %greater). Thus, the total FLOPs are 84 FLOPs.
  float expected_elapsed_time = 84;
  EXPECT_EQ(runtime_simulator_->ComputeEstimatedElapsedTime(*hlo_live_range,
                                                            allocations),
            expected_elapsed_time);
}

class SimulateAsyncCopyDoneTest : public MemorySpaceAssignmentSimulatorTest {
 protected:
  absl::Status Initialize(absl::string_view hlo_string) {
    TF_RETURN_IF_ERROR(
        MemorySpaceAssignmentSimulatorTest::Initialize(hlo_string));
    for (const HloInstruction* inst :
         module_->entry_computation()->instructions()) {
      instruction_map_[inst->name()] = inst;
      if (inst->name() == "copy-start.1") {
        outstanding_read_default_queue_.push_back(
            memory_space_assignment::OutstandingAsyncCopy{inst, 512});
      } else if (inst->name() == "copy-start.2") {
        outstanding_write_default_queue_.push_back(
            memory_space_assignment::OutstandingAsyncCopy{inst, 128});
      }
    }
    runtime_simulator_ = std::make_unique<RuntimeSimulator>(
        cost_analysis_.get(), kAlternateMemorySpace,
        outstanding_read_default_queue_, outstanding_write_default_queue_);
    return absl::OkStatus();
  }
  std::map<std::string_view, const HloInstruction*> instruction_map_;
  std::list<memory_space_assignment::OutstandingAsyncCopy>
      outstanding_read_default_queue_;
  std::list<memory_space_assignment::OutstandingAsyncCopy>
      outstanding_write_default_queue_;
};

TEST_F(SimulateAsyncCopyDoneTest, AsyncCopyAlreadyCompleted) {
  absl::string_view hlo_string =
      R"(HloModule module, is_scheduled=true
      ENTRY Entry {
        param_0 = f32[128] parameter(0)
        copy-start.1 = (f32[128]{0:S(1)}, f32[128], u32[]) copy-start(param_0)
        ROOT copy-done.1 = f32[128]{0:S(1)} copy-done(copy-start.1)
      }
    )";

  TF_ASSERT_OK(Initialize(hlo_string));

  const HloInstruction* copy_done_inst = instruction_map_["copy-done.1"];
  // Process the copy-start.1
  runtime_simulator_->SimulateAsyncCopyDone(copy_done_inst);

  // There should be no request in the read/write queues.
  EXPECT_THAT(runtime_simulator_->GetOutstandingReadDefaultQueue(), IsEmpty());
  EXPECT_THAT(runtime_simulator_->GetOutstandingWriteDefaultQueue(), IsEmpty());
  // The function should return 0 for requests that are already completed.
  float elapsed_time_for_completed_copy =
      runtime_simulator_->SimulateAsyncCopyDone(copy_done_inst);
  EXPECT_EQ(elapsed_time_for_completed_copy, 0);
  // There should be no request in the read/write queues.
  EXPECT_THAT(runtime_simulator_->GetOutstandingReadDefaultQueue(), IsEmpty());
  EXPECT_THAT(runtime_simulator_->GetOutstandingWriteDefaultQueue(), IsEmpty());
}

TEST_F(SimulateAsyncCopyDoneTest, AsyncCopyFullBandwidth) {
  absl::string_view hlo_string =
      R"(HloModule module, is_scheduled=true
      ENTRY Entry {
        param_0 = f32[128] parameter(0)
        copy-start.1 = (f32[128]{0:S(1)}, f32[128], u32[]) copy-start(param_0)
        ROOT copy-done.1 = f32[128]{0:S(1)} copy-done(copy-start.1)
      }
    )";

  TF_ASSERT_OK(Initialize(hlo_string));
  const HloInstruction* copy_done_inst = instruction_map_["copy-done.1"];

  // The elapsed time for copy-done.1 is 128 * 4 / 1 = 512.
  float copy_done_elapsed_time =
      runtime_simulator_->SimulateAsyncCopyDone(copy_done_inst);
  EXPECT_EQ(copy_done_elapsed_time, 512);

  // There should be no request in the read/write queues.
  EXPECT_THAT(runtime_simulator_->GetOutstandingReadDefaultQueue(), IsEmpty());
  EXPECT_THAT(runtime_simulator_->GetOutstandingWriteDefaultQueue(), IsEmpty());
}

TEST_F(SimulateAsyncCopyDoneTest, AsyncCopySharedBandwidth) {
  absl::string_view hlo_string =
      R"(HloModule module, is_scheduled=true
      ENTRY Entry {
        param_0 = f32[128] parameter(0)
        param_1 = f32[32]{0:S(1)} parameter(1)
        copy-start.1 = (f32[128]{0:S(1)}, f32[128], u32[]) copy-start(param_0)
        copy-start.2 = (f32[32], f32[32]{0:S(1)}, u32[]) copy-start(param_1)
        copy-done.2 = f32[32] copy-done(copy-start.2)
        ROOT copy-done.1 = f32[128]{0:S(1)} copy-done(copy-start.1)
      }
    )";

  TF_ASSERT_OK(Initialize(hlo_string));

  const HloInstruction* copy_start_1_inst = instruction_map_["copy-start.1"];
  const HloInstruction* copy_done_2_inst = instruction_map_["copy-done.2"];

  // The copy-start.2 needs to share bandwidth with copy-start.1. Thus, it can
  // only use half bandwidth to access default memory. Thus, the elapsed time is
  // 32 * 4 / 0.5 = 256
  float copy_done_2_elapsed_time =
      runtime_simulator_->SimulateAsyncCopyDone(copy_done_2_inst);
  EXPECT_EQ(copy_done_2_elapsed_time, 256);

  // The only write request (copy-start.2) should be completed.
  EXPECT_THAT(runtime_simulator_->GetOutstandingWriteDefaultQueue(), IsEmpty());

  // The read request has (128-32)*4 bytes left to process.
  EXPECT_THAT(runtime_simulator_->GetOutstandingReadDefaultQueue(),
              ElementsAreArray({memory_space_assignment::OutstandingAsyncCopy{
                  copy_start_1_inst, 384}}));
}

TEST_F(SimulateAsyncCopyDoneTest, AsyncCopyTransferPartialProcess) {
  absl::string_view hlo_string =
      R"(HloModule module, is_scheduled=true
      ENTRY Entry {
        param_0 = f32[128] parameter(0)
        param_1 = f32[32]{0:S(1)} parameter(1)
        copy-start.1 = (f32[128]{0:S(1)}, f32[128], u32[]) copy-start(param_0)
        copy-start.2 = (f32[32], f32[32]{0:S(1)}, u32[]) copy-start(param_1)
        copy-done.2 = f32[32] copy-done(copy-start.2)
        ROOT copy-done.1 = f32[128]{0:S(1)} copy-done(copy-start.1)
      }
    )";

  TF_ASSERT_OK(Initialize(hlo_string));

  const HloInstruction* copy_start_1_inst = instruction_map_["copy-start.1"];
  const HloInstruction* copy_done_1_inst = instruction_map_["copy-done.1"];
  const HloInstruction* copy_done_2_inst = instruction_map_["copy-done.2"];

  // Execute copy-done.2.
  float copy_done_2_elapsed_time =
      runtime_simulator_->SimulateAsyncCopyDone(copy_done_2_inst);
  // For copy-done.2, it requires to transfer 32*4 bytes
  // default-write request. At the same time, there is a 128*4 bytes
  // default-read request in the queue for copy-start.1. So the
  // elapsed time for copy-done.2 is 32*4 / (0.5*1) = 256.
  EXPECT_EQ(copy_done_2_elapsed_time, 256);
  // In parallel with copy-done.2, copy-start.1 is also being processed.
  // So the remaining bytes should be 128*4 - 32*4 = 384.
  EXPECT_THAT(runtime_simulator_->GetOutstandingReadDefaultQueue(),
              ElementsAreArray({memory_space_assignment::OutstandingAsyncCopy{
                  copy_start_1_inst, 384}}));
  EXPECT_THAT(runtime_simulator_->GetOutstandingWriteDefaultQueue(), IsEmpty());

  // Execute copy-done.1.
  float copy_done_1_elapsed_time =
      runtime_simulator_->SimulateAsyncCopyDone(copy_done_1_inst);
  // The copy-done.1 is the only request in the read-queue, and there is no
  // request in the write-queue. Thus, it can use the full bandwidth. The
  // elapsed time is 384 / 1 = 384.
  EXPECT_EQ(copy_done_1_elapsed_time, 384);
  // No request should be in the queue.
  EXPECT_THAT(runtime_simulator_->GetOutstandingReadDefaultQueue(), IsEmpty());
  EXPECT_THAT(runtime_simulator_->GetOutstandingWriteDefaultQueue(), IsEmpty());
}

}  // namespace
}  // namespace xla
