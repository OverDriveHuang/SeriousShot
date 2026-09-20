#include "platform/qt/qt_export_task_executor.hpp"
#include "test_support.hpp"
#include <QCoreApplication>
#include <atomic>
#include <chrono>
#include <future>
#include <vector>

using namespace hdrshot;
void serial_background_and_main_publication_order() {
  QObject context;
  QtExportTaskExecutor executor(context);
  std::promise<void> started, release;
  auto ready = started.get_future();
  auto gate = release.get_future();
  std::atomic<bool> second_started{};
  std::vector<int> published;
  executor.background([&] {
    started.set_value();
    // Bounded even when this test fails; drain must never hang a test runner.
    gate.wait_for(std::chrono::seconds(2));
    executor.main_thread([&] { published.push_back(1); });
  });
  executor.background([&] {
    second_started = true;
    executor.main_thread([&] { published.push_back(2); });
  });
  const bool began = ready.wait_for(std::chrono::seconds(1)) == std::future_status::ready;
  const bool serialized = !second_started.load();
  release.set_value();
  executor.drain();
  HDRSHOT_CHECK(began && serialized);
  HDRSHOT_CHECK(published == std::vector<int>({1, 2}));
}
int main(int argc, char** argv) {
  QCoreApplication application(argc, argv);
  return test::run({{"real Qt executor preserves copy order", serial_background_and_main_publication_order}});
}
