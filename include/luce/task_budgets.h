#pragma once

#include "luce/task_runtime.h"

namespace luce {

namespace task_budget {

inline constexpr TaskBudget kTaskBlink{"blink", 2048, 1, tskNO_AFFINITY};
inline constexpr TaskBudget kTaskLed{"led", 2048, 1, tskNO_AFFINITY};
inline constexpr TaskBudget kTaskIoDiagnostics{"io", 8192, 1, tskNO_AFFINITY};
inline constexpr TaskBudget kTaskWifi{"wifi", 4096, 2, tskNO_AFFINITY};
inline constexpr TaskBudget kTaskNtp{"ntp", 4096, 2, tskNO_AFFINITY};
inline constexpr TaskBudget kTaskMdns{"mdns", 4096, 2, tskNO_AFFINITY};
inline constexpr TaskBudget kTaskCliNet{"cli_net", 3072, 2, tskNO_AFFINITY};
inline constexpr TaskBudget kTaskMqtt{"mqtt", 4096, 2, tskNO_AFFINITY};
inline constexpr TaskBudget kTaskHttp{"http_server", 4096, 2, tskNO_AFFINITY};
inline constexpr TaskBudget kTaskOta{"ota", 6144, 2, tskNO_AFFINITY};
inline constexpr TaskBudget kTaskCli{"cli", 6144, 2, tskNO_AFFINITY};

}  // namespace task_budget

}  // namespace luce
