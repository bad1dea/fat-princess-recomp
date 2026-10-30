#pragma once

// Changes apply to locally controlled AI on its next movement update.
extern "C" void fp_ai_route_set_guard(int enabled);
extern "C" int fp_ai_route_guard_enabled();
