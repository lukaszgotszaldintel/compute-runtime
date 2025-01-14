/*
 * Copyright (C) 2021 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 *
 */

#pragma once
#include <level_zero/zet_api.h>

struct _zet_debug_session_handle_t {};

namespace L0 {

struct Device;

struct DebugSession : _zet_debug_session_handle_t {
    virtual ~DebugSession() = default;
    DebugSession() = delete;

    static DebugSession *create(const zet_debug_config_t &config, Device *device, ze_result_t &result);

    static DebugSession *fromHandle(zet_debug_session_handle_t handle) { return static_cast<DebugSession *>(handle); }
    inline zet_debug_session_handle_t toHandle() { return this; }

    virtual bool closeConnection() = 0;
    virtual ze_result_t initialize() = 0;

    virtual ze_result_t readEvent(uint64_t timeout, zet_debug_event_t *event) = 0;
    virtual ze_result_t interrupt(ze_device_thread_t thread) = 0;
    virtual ze_result_t resume(ze_device_thread_t thread) = 0;
    virtual ze_result_t readMemory(ze_device_thread_t thread, const zet_debug_memory_space_desc_t *desc, size_t size, void *buffer) = 0;
    virtual ze_result_t writeMemory(ze_device_thread_t thread, const zet_debug_memory_space_desc_t *desc, size_t size, const void *buffer) = 0;
    virtual ze_result_t acknowledgeEvent(const zet_debug_event_t *event) = 0;
    virtual ze_result_t readRegisters(ze_device_thread_t thread, zet_debug_regset_type_t type, uint32_t start, uint32_t count, void *pRegisterValues) = 0;
    virtual ze_result_t writeRegisters(ze_device_thread_t thread, zet_debug_regset_type_t type, uint32_t start, uint32_t count, void *pRegisterValues) = 0;

    Device *getConnectedDevice() { return connectedDevice; }
    virtual void startAsyncThread() = 0;

  protected:
    DebugSession(const zet_debug_config_t &config, Device *device) : connectedDevice(device){};
    Device *connectedDevice = nullptr;
};

} // namespace L0