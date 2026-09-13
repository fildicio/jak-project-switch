#include "Assert.h"

#ifndef NO_ASSERT

#include <cstdio>
#include <cstdlib>
#include <string_view>

#include "common/log/log.h"

#if defined(__SWITCH__)
// FIX 7n: lg::die is buffered and has never survived a death on hardware. An assert is
// exactly the kind of event we have been unable to see (Eden hit 'size > 0' in
// alloc_from_heap at this very phase), so write it through the raw fsync'd channel too.
void switch_run_logf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
#endif

void private_assert_failed(const char* expr,
                           const char* file,
                           int line,
                           const char* function,
                           const char* msg) {
#if defined(__SWITCH__)
  switch_run_logf("[ASSERT] '%s' at %s:%d in %s msg=%s lr=%p", expr ? expr : "?",
                  file ? file : "?", line, function ? function : "?", (msg && msg[0]) ? msg : "-",
                  __builtin_return_address(0));
#endif
  if (!msg || msg[0] == '\0') {
    std::string log = fmt::format("Assertion failed: '{}'\n\tSource: {}:{}\n\tFunction: {}\n", expr,
                                  file, line, function);
    lg::die("{}", log);
  } else {
    std::string log =
        fmt::format("Assertion failed: '{}'\n\tMessage: {}\n\tSource: {}:{}\n\tFunction: {}\n",
                    expr, msg, file, line, function);
    lg::die("{}", log);
  }
#if defined(__SWITCH__)
  // -O3 inlines Ptr<T>::operator*, so return_address(0) is the real offending call site.
  // addr2line these against build-switch/game/gk to name it.
  lg::error("assert call site: {}", __builtin_return_address(0));
#endif
  abort();
}

void private_assert_failed(const char* expr,
                           const char* file,
                           int line,
                           const char* function,
                           const std::string_view& msg) {
  if (msg.empty()) {
    private_assert_failed(expr, file, line, function);
  } else {
    private_assert_failed(expr, file, line, function, msg.data());
  }
}

#endif
