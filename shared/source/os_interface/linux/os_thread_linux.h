/*
 * Copyright (C) 2018-2021 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 *
 */

#include "shared/source/os_interface/os_thread.h"

#include <pthread.h>

namespace NEO {
class ThreadLinux : public Thread {
  public:
    ThreadLinux(pthread_t threadId);
    void join() override;
    void yield() override;
    ~ThreadLinux() override = default;

  protected:
    pthread_t threadId;
};
} // namespace NEO
