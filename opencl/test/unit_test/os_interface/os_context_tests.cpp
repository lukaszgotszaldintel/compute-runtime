/*
 * Copyright (C) 2019-2021 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 *
 */

#include "shared/source/os_interface/device_factory.h"
#include "shared/source/os_interface/os_context.h"
#include "shared/test/common/helpers/debug_manager_state_restore.h"
#include "shared/test/common/helpers/default_hw_info.h"
#include "shared/test/common/mocks/mock_device.h"

#include "gtest/gtest.h"

using namespace NEO;

TEST(OSContext, whenCreatingDefaultOsContextThenExpectInitializedAlways) {
    OsContext *osContext = OsContext::create(nullptr, 0, 0, EngineTypeUsage{aub_stream::ENGINE_RCS, EngineUsage::Regular}, PreemptionMode::Disabled, false);
    EXPECT_FALSE(osContext->isLowPriority());
    EXPECT_FALSE(osContext->isInternalEngine());
    EXPECT_FALSE(osContext->isRootDevice());
    delete osContext;
}

TEST(OSContext, givenInternalAndRootDeviceAreTrueWhenCreatingDefaultOsContextThenExpectGettersTrue) {
    OsContext *osContext = OsContext::create(nullptr, 0, 0, EngineTypeUsage{aub_stream::ENGINE_RCS, EngineUsage::Internal}, PreemptionMode::Disabled, true);
    EXPECT_FALSE(osContext->isLowPriority());
    EXPECT_TRUE(osContext->isInternalEngine());
    EXPECT_TRUE(osContext->isRootDevice());
    delete osContext;
}

TEST(OSContext, givenLowPriorityAndRootDeviceAreTrueWhenCreatingDefaultOsContextThenExpectGettersTrue) {
    OsContext *osContext = OsContext::create(nullptr, 0, 0, EngineTypeUsage{aub_stream::ENGINE_RCS, EngineUsage::LowPriority}, PreemptionMode::Disabled, true);
    EXPECT_TRUE(osContext->isLowPriority());
    EXPECT_FALSE(osContext->isInternalEngine());
    EXPECT_TRUE(osContext->isRootDevice());
    delete osContext;
}

TEST(OSContext, givenOsContextCreatedDefaultIsFalseWhenSettingTrueThenFlagTrueReturned) {
    OsContext *osContext = OsContext::create(nullptr, 0, 0, EngineTypeUsage{aub_stream::ENGINE_RCS, EngineUsage::Regular}, PreemptionMode::Disabled, false);
    EXPECT_FALSE(osContext->isDefaultContext());
    osContext->setDefaultContext(true);
    EXPECT_TRUE(osContext->isDefaultContext());
    delete osContext;
}

struct DeferredOsContextCreationTests : ::testing::Test {
    void SetUp() override {
        device = std::unique_ptr<MockDevice>{MockDevice::createWithNewExecutionEnvironment<MockDevice>(defaultHwInfo.get())};
        DeviceFactory::prepareDeviceEnvironments(*device->getExecutionEnvironment());
    }

    void expectContextCreation(EngineTypeUsage engineTypeUsage, bool defaultEngine, bool expectedImmediate) {
        OSInterface *osInterface = device->getRootDeviceEnvironment().osInterface.get();
        std::unique_ptr<OsContext> osContext{OsContext::create(osInterface, 0, 0, engineTypeUsage, PreemptionMode::Disabled, false)};
        EXPECT_FALSE(osContext->isInitialized());

        const bool immediate = osContext->isImmediateContextInitializationEnabled(defaultEngine);
        EXPECT_EQ(expectedImmediate, immediate);
        if (immediate) {
            osContext->ensureContextInitialized();
            EXPECT_TRUE(osContext->isInitialized());
        }
    }

    void expectDeferredContextCreation(EngineTypeUsage engineTypeUsage, bool defaultEngine) {
        expectContextCreation(engineTypeUsage, defaultEngine, false);
    }

    void expectImmediateContextCreation(EngineTypeUsage engineTypeUsage, bool defaultEngine) {
        expectContextCreation(engineTypeUsage, defaultEngine, true);
    }

    std::unique_ptr<MockDevice> device;
    static inline const EngineTypeUsage engineTypeUsageRegular{aub_stream::ENGINE_RCS, EngineUsage::Regular};
    static inline const EngineTypeUsage engineTypeUsageInternal{aub_stream::ENGINE_RCS, EngineUsage::Internal};
};

TEST_F(DeferredOsContextCreationTests, givenRegularEngineWhenCreatingOsContextThenOsContextIsInitializedDeferred) {
    DebugManagerStateRestore restore{};

    expectImmediateContextCreation(engineTypeUsageRegular, false);

    DebugManager.flags.DeferOsContextInitialization.set(1);
    expectDeferredContextCreation(engineTypeUsageRegular, false);

    DebugManager.flags.DeferOsContextInitialization.set(0);
    expectImmediateContextCreation(engineTypeUsageRegular, false);
}

TEST_F(DeferredOsContextCreationTests, givenDefaultEngineWhenCreatingOsContextThenOsContextIsInitializedDeferred) {
    DebugManagerStateRestore restore{};

    expectImmediateContextCreation(engineTypeUsageRegular, true);

    DebugManager.flags.DeferOsContextInitialization.set(1);
    expectImmediateContextCreation(engineTypeUsageRegular, true);

    DebugManager.flags.DeferOsContextInitialization.set(0);
    expectImmediateContextCreation(engineTypeUsageRegular, true);
}

TEST_F(DeferredOsContextCreationTests, givenRegularEngineWhenCreatingOsContextThenOsContextIsInitializedImmediately) {
    DebugManagerStateRestore restore{};

    expectImmediateContextCreation(engineTypeUsageInternal, false);

    DebugManager.flags.DeferOsContextInitialization.set(1);
    expectImmediateContextCreation(engineTypeUsageInternal, false);

    DebugManager.flags.DeferOsContextInitialization.set(0);
    expectImmediateContextCreation(engineTypeUsageInternal, false);
}

TEST_F(DeferredOsContextCreationTests, givenEnsureContextInitializeCalledMultipleTimesWhenOsContextIsCreatedThenInitializeOnlyOnce) {
    struct MyOsContext : OsContext {
        MyOsContext(uint32_t contextId,
                    DeviceBitfield deviceBitfield,
                    EngineTypeUsage typeUsage,
                    PreemptionMode preemptionMode,
                    bool rootDevice) : OsContext(contextId, deviceBitfield, typeUsage, preemptionMode, rootDevice) {}

        void initializeContext() override {
            initializeContextCalled++;
        }

        size_t initializeContextCalled = 0u;
    };

    MyOsContext osContext{0, 0, engineTypeUsageRegular, PreemptionMode::Disabled, false};
    EXPECT_FALSE(osContext.isInitialized());

    osContext.ensureContextInitialized();
    EXPECT_TRUE(osContext.isInitialized());
    EXPECT_EQ(1u, osContext.initializeContextCalled);

    osContext.ensureContextInitialized();
    EXPECT_TRUE(osContext.isInitialized());
    EXPECT_EQ(1u, osContext.initializeContextCalled);
}
