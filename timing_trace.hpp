#pragma once

#include <cstdint>
#include <cstring>
#include <vulkan/vulkan.h>

// Timing recorder for vrcompositor. See timing_trace.cpp.
bool TraceEnabled();
void Trace(const char* name, uint64_t a, uint64_t b);

// Logs a line when vrcompositor misses vsyncs. See timing_trace.cpp.
void NoteVsyncForHitches(int64_t waitBeginNs, int64_t waitEndNs);

// Returns a tracing hook for pName, or nullptr when tracing is off or the
// function is not traced.
PFN_vkVoidFunction TraceGetDeviceProcAddr(const char* pName);
