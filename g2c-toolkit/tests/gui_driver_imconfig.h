// Only for tests/gui_driver.cpp: catch ImGui assertions instead of aborting.
//
// The driver clicks its way through every window. A failed assertion should
// end up in the log as an error, with file and line, instead of ending the
// run without any notice.
#pragma once

void g2DriverAssert(const char* expr, const char* file, int line);

#define IM_ASSERT(_EXPR) ((_EXPR) ? (void)0 : g2DriverAssert(#_EXPR, __FILE__, __LINE__))
