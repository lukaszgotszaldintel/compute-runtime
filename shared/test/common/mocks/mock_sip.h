/*
 * Copyright (C) 2018-2021 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 *
 */

#pragma once

#include "shared/source/built_ins/sip.h"

#include "opencl/test/unit_test/mocks/mock_execution_environment.h"

#include <memory>
#include <vector>

namespace NEO {
class MemoryAllocation;

class MockSipKernel : public SipKernel {
  public:
    using SipKernel::type;

    MockSipKernel(SipKernelType type, GraphicsAllocation *sipAlloc);
    MockSipKernel();
    ~MockSipKernel() override;

    static const char *dummyBinaryForSip;
    static std::vector<char> getDummyGenBinary();

    GraphicsAllocation *getSipAllocation() const override;
    const std::vector<char> &getStateSaveAreaHeader() const override;

    void createMockSipAllocation();

    std::unique_ptr<MemoryAllocation> mockSipMemoryAllocation;
    const std::vector<char> mockStateSaveAreaHeader = {'s', 's', 'a', 'h'};
    MockExecutionEnvironment executionEnvironment;
};

namespace MockSipData {
extern std::unique_ptr<MockSipKernel> mockSipKernel;
extern SipKernelType calledType;
extern bool called;
extern bool returned;
extern bool useMockSip;

void clearUseFlags();
} // namespace MockSipData
} // namespace NEO
