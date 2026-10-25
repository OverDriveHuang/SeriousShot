#pragma once
#include <QApplication>
#include <QDialog>
#include <QObject>
#include <QPointer>
#include <QThread>
#include <QTimer>
#include <algorithm>
#include <functional>
#include <utility>
#include <vector>

namespace hdrshot {
// UI-thread lifecycle gate for the Windows shell's synchronous export protocol.
// It deliberately does not own windows, dialogs, exports, or their runtimes.
class WindowsExitCoordinator final : public QObject {
 public:
  using Confirm = std::function<bool()>;
  using Finalize = std::function<void(int)>;
  WindowsExitCoordinator(Confirm confirm, Finalize finalize)
      : confirm_(std::move(confirm)), finalize_(std::move(finalize)) {}

  class Call final {
   public:
    Call(const Call &) = delete;
    Call &operator=(const Call &) = delete;
    Call(Call &&other) noexcept
        : owner_(std::exchange(other.owner_,{})), dialog_(other.dialog_),
          is_dialog_(other.is_dialog_) {}
    ~Call() {
      if(owner_) owner_->leave(dialog_,is_dialog_);
    }
   private:
    friend class WindowsExitCoordinator;
    Call(WindowsExitCoordinator &owner,QDialog *dialog)
        : owner_(&owner),dialog_(dialog),is_dialog_(dialog!=nullptr) {
      owner.enter(dialog);
    }
    QPointer<WindowsExitCoordinator> owner_;
    QPointer<QDialog> dialog_;
    bool is_dialog_{};
  };
  [[nodiscard]] Call enter_call() { return Call(*this,nullptr); }
  [[nodiscard]] Call enter_dialog(QDialog &dialog) { return Call(*this,&dialog); }
  [[nodiscard]] bool accepting_work() const { return state_==State::accepting; }
  [[nodiscard]] bool finished() const { return state_==State::finished; }
  [[nodiscard]] bool synchronous_call_active() const { return calls_!=0 || dialogs_!=0; }

  void request_exit(int code=0) {
    if(!accepting_work()) return; // Including nested requests during confirmation.
    state_=State::requested;
    exit_code_=code;
    unwinding_=true;
    // reject() only ends exec(); its C++ call and cancellation callback still
    // have to return. The RAII depths remain held until those returns occur.
    const auto active=active_dialogs_;
    for(const auto &dialog:active) if(dialog) dialog->reject();
    queue_return_barrier();
  }

  // The updater requires a synchronous completion receipt. A pending user exit
  // or synchronous save cannot be reported as an already-completed shutdown.
  bool shutdown_synchronously(int code=0) {
    if(finished()) return true;
    if(!accepting_work() || synchronous_call_active() || unwinding_ ||
       QApplication::activeModalWidget() || QThread::currentThread()->loopLevel()>1) return false;
    exit_code_=code;
    return confirm_and_finalize();
  }
  // The composition root is stack-owned outside QApplication::exec(). All
  // nested dialog/submit frames have returned before that scope can destruct.
  // Do not ask interactive close questions after the event loop has ended.
  void finish_after_event_loop() {
    Q_ASSERT(!synchronous_call_active());
    if(finished()) return;
    state_=State::finalizing;
    if(finalize_) finalize_(exit_code_);
    state_=State::finished;
  }

 private:
  enum class State { accepting, requested, confirming, finalizing, finished };
  void enter(QDialog *dialog) {
    unwinding_=true;
    if(dialog) {++dialogs_;active_dialogs_.push_back(dialog);}
    else ++calls_;
  }
  void leave(const QPointer<QDialog> &dialog,bool is_dialog) {
    if(is_dialog) {
      --dialogs_;
      const auto found=std::find(active_dialogs_.begin(),active_dialogs_.end(),dialog);
      if(found!=active_dialogs_.end()) active_dialogs_.erase(found);
    } else --calls_;
    queue_return_barrier();
  }
  void queue_return_barrier() {
    if(synchronous_call_active() || barrier_pending_ || !unwinding_) return;
    barrier_pending_=true;
    // DeferredDelete observes Qt's dispatch/loop nesting. It is only a return
    // barrier: readiness is checked again when delivered, and a deeper modal
    // loop cannot authorize destruction after the guarded scope has returned.
    auto *barrier=new QObject(this);
    QObject::connect(barrier,&QObject::destroyed,this,[this] {
      barrier_pending_=false;
      if(synchronous_call_active()) return;
      // The shell lives outside QApplication::exec() (level 1). Also wait for
      // an independently opened modal confirmation: it can own a stack dialog
      // even when no export Call exists and native exec does not add a Qt loop.
      if(QThread::currentThread()->loopLevel()>1 || QApplication::activeModalWidget()) {
        QTimer::singleShot(10,this,[this]{queue_return_barrier();});
        return;
      }
      unwinding_=false;
      if(state_==State::requested) (void)confirm_and_finalize();
    });
    barrier->deleteLater();
  }
  bool confirm_and_finalize() {
    state_=State::confirming;
    if(confirm_ && !confirm_()) {
      state_=State::accepting;
      return false;
    }
    state_=State::finalizing;
    if(finalize_) finalize_(exit_code_);
    state_=State::finished;
    return true;
  }
  Confirm confirm_;
  Finalize finalize_;
  State state_{State::accepting};
  int exit_code_{},calls_{},dialogs_{};
  bool unwinding_{},barrier_pending_{};
  std::vector<QPointer<QDialog>> active_dialogs_;
};
} // namespace hdrshot
