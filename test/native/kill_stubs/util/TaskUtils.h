#pragma once

#include "host_fakes.h"

namespace OpenShock::TaskUtils {
  BaseType_t TaskCreateUniversal(TaskFunction_t, const char*, uint32_t, void*, UBaseType_t, TaskHandle_t*, BaseType_t);
  BaseType_t TaskCreateExpensive(TaskFunction_t, const char*, uint32_t, void*, UBaseType_t, TaskHandle_t*);
}  // namespace OpenShock::TaskUtils
