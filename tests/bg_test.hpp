// Bandwidth Governor - minimal deterministic test harness.
// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdio>
#include <exception>
#include <functional>
#include <string>
#include <vector>

namespace bgtest {

inline int& failures() {
  static int f = 0;
  return f;
}

struct Entry {
  std::string name;
  std::function<void()> fn;
};

inline std::vector<Entry>& entries() {
  static std::vector<Entry> e;
  return e;
}

struct Registrar {
  Registrar(const char* name, std::function<void()> fn) {
    entries().push_back({name, std::move(fn)});
  }
};

inline void report_failure(const char* file, int line, const char* expr) {
  ++failures();
  std::printf("    FAIL %s:%d: %s\n", file, line, expr);
}

inline int run_all() {
  for (const Entry& e : entries()) {
    std::printf("[ RUN  ] %s\n", e.name.c_str());
    int before = failures();
    try {
      e.fn();
    } catch (const std::exception& ex) {
      ++failures();
      std::printf("    UNCAUGHT exception: %s\n", ex.what());
    } catch (...) {
      ++failures();
      std::printf("    UNCAUGHT unknown exception\n");
    }
    if (failures() == before) std::printf("[  OK  ] %s\n", e.name.c_str());
    else std::printf("[ FAIL ] %s\n", e.name.c_str());
  }
  if (failures() != 0) {
    std::printf("\n%d test failure(s) reported\n", failures());
    return 1;
  }
  std::printf("\nall tests passed\n");
  return 0;
}

}  // namespace bgtest

#define BG_TEST_CONCAT_(a, b) a##b
#define BG_TEST_CONCAT(a, b) BG_TEST_CONCAT_(a, b)

#define BG_TEST(name)                                                      \
  static void BG_TEST_CONCAT(bg_test_fn_, __LINE__)();                     \
  static ::bgtest::Registrar BG_TEST_CONCAT(bg_test_reg_, __LINE__)(       \
      name, &BG_TEST_CONCAT(bg_test_fn_, __LINE__));                       \
  static void BG_TEST_CONCAT(bg_test_fn_, __LINE__)()

#define CHECK(cond)                                                        \
  do { if (!(cond)) ::bgtest::report_failure(__FILE__, __LINE__, #cond); } \
  while (0)

#define REQUIRE(cond)                                                      \
  do { if (!(cond)) { ::bgtest::report_failure(__FILE__, __LINE__, #cond); return; } } \
  while (0)

#define REQUIRE_THROWS_AS(expr, ExcType)                                   \
  do {                                                                     \
    bool bg_caught = false;                                                \
    try { (void)(expr); }                                                  \
    catch (const ExcType&) { bg_caught = true; }                           \
    catch (...) {}                                                         \
    if (!bg_caught) ::bgtest::report_failure(__FILE__, __LINE__, "expected " #ExcType); \
  } while (0)

#define REQUIRE_NOTHROW(expr)                                              \
  do {                                                                     \
    try { (void)(expr); }                                                  \
    catch (...) { ::bgtest::report_failure(__FILE__, __LINE__, "unexpected throw in " #expr); } \
  } while (0)
