#pragma once
#include <QPoint>
#include <cstdint>
class QWidget;
namespace hdrshot::test {
struct NativeFocus { std::int64_t process{}; std::uintptr_t key_window{}; bool operator==(const NativeFocus &) const = default; };
// Restores the foreground app on all exit paths; never launches an app.
class NativeBackgroundScope {
public:
  NativeBackgroundScope();
  ~NativeBackgroundScope();
  NativeBackgroundScope(const NativeBackgroundScope &) = delete;
  NativeBackgroundScope &operator=(const NativeBackgroundScope &) = delete;
  bool activate();
  std::int64_t process() const { return external_; }
private:
  std::int64_t original_{}, external_{};
};
struct NativeWindowState { bool visible{}, on_screen{}, hides_on_deactivate{}; std::int64_t number{}; };
NativeWindowState native_window_state(QWidget &window);
struct NativeWindowContract {
  std::uintptr_t view{}, window{};
  bool hides_on_deactivate{}, can_become_key{};
  bool animation_none{};
};
// Reads an already-created native host; no activation, ordering or mutation.
NativeWindowContract native_window_contract(QWidget &window);
bool native_desktop_ready();
NativeFocus native_focus();
bool native_make_key(QWidget &window);
void native_mouse(QWidget &target, QPoint local, bool down, bool right = false);
void native_space(QWidget &target, bool down);
}
