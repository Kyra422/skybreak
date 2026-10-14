#pragma once
// GXF12: host-side frame pipeline pieces with no GL and no runtime state, so
// they can be unit tested (outputs/pc-tests/gxf12).
//
//  - PlanCut: which completed guest frame (EFB->XFB copy) ProcessGXFifo cuts
//    next, and which older draws it drops. PerFrame cuts the OLDEST completed
//    frame and lets up to `max_backlog` further completed frames wait; beyond
//    that (the host is behind) the oldest are merged into one cut (their draws
//    dropped, state/copies kept). With max_backlog 0 (gx.cpp default, see the
//    comment there) it cuts the newest, like Merge. Merge is the pre-GXF12
//    cutter (cut at the LAST frame end, drop every older frame's draws);
//    gxreplay uses it for captures recorded before GXF12.
//  - PresentRing: slot bookkeeping of the present thread (main.cpp). The host
//    thread renders and copies each frame into a free slot and queues it; the
//    present thread shows queued slots in order, one per swap. Slots in use by
//    either side are never handed out.
//  - ParallelFor: a small persistent worker pool (vertex decode of a frame).
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iterator>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace nwii::runtime::gx {

enum class CutPolicy { PerFrame, Merge };

struct CutPlan {
  bool cut = false;        // a completed frame is cut
  size_t end_slot = 0;     // index into `ends` of the cut frame's XFB copy
  long long drop_upto = -1; // draws at carry index <= drop_upto are dropped
  size_t merged = 0;       // guest frames whose draws are dropped
};

// `ends`: carry indices of the completed frames' XFB copies, ascending.
inline CutPlan PlanCut(const std::vector<size_t> &ends, CutPolicy policy, int max_backlog) {
  CutPlan p;
  if (ends.empty())
    return p;
  p.cut = true;
  if (policy == CutPolicy::Merge) {
    p.end_slot = ends.size() - 1;
  } else {
    const size_t keep = (size_t)std::max(0, max_backlog);
    p.end_slot = ends.size() - 1 > keep ? ends.size() - 1 - keep : 0;
  }
  if (p.end_slot > 0) {
    p.drop_upto = (long long)ends[p.end_slot - 1];
    p.merged = p.end_slot;
  }
  return p;
}

// Takes the planned cut out of `carry`: returns commands [0, ends[end_slot]]
// minus the draws at index <= drop_upto (counted in *dropped), keeps the tail
// (later frames) in `carry`, and rebases `ends` / `scan_pos` to it. The vectors
// are swapped so only the tail is moved (the old front erase moved the whole
// remaining carry).
template <class Cmd, class IsDraw>
std::vector<Cmd> TakeCut(std::vector<Cmd> &carry, std::vector<size_t> &ends, size_t &scan_pos,
                         const CutPlan &plan, IsDraw is_draw, size_t *dropped) {
  std::vector<Cmd> frame;
  if (dropped)
    *dropped = 0;
  if (!plan.cut || plan.end_slot >= ends.size())
    return frame;
  const size_t cut_end = ends[plan.end_slot];
  frame.swap(carry);
  if (frame.size() > cut_end + 1) {
    carry.reserve(frame.size() - (cut_end + 1) + 4096);
    carry.assign(std::make_move_iterator(frame.begin() + (cut_end + 1)),
                 std::make_move_iterator(frame.end()));
    frame.resize(cut_end + 1);
  }
  if (plan.drop_upto >= 0) {
    size_t w = 0, n = 0;
    for (size_t i = 0; i < frame.size(); ++i) {
      if ((long long)i <= plan.drop_upto && is_draw(frame[i])) {
        ++n;
        continue;
      }
      if (w != i)
        frame[w] = std::move(frame[i]);
      ++w;
    }
    frame.resize(w);
    if (dropped)
      *dropped = n;
  }
  const size_t consumed = cut_end + 1;
  size_t k = 0;
  for (size_t j = plan.end_slot + 1; j < ends.size(); ++j)
    ends[k++] = ends[j] - consumed;
  ends.resize(k);
  scan_pos = scan_pos >= consumed ? scan_pos - consumed : 0;
  return frame;
}

class PresentRing {
public:
  static constexpr int kMaxSlots = 8;
  enum class St : uint8_t { Free, Writing, Queued, InUse };

  explicit PresentRing(int slots = 4, int max_queued = 2)
      : n_(std::clamp(slots, 2, kMaxSlots)), max_q_(std::max(1, max_queued)) {}

  int slots() const { return n_; }
  St state(int s) const { return st_[s]; }
  uint64_t frame(int s) const { return fr_[s]; }
  int queued() const { return (int)q_.size(); }
  // INTERP17: frames may wait for their present time while interpolating (gxf12_presenter.h).
  void set_max_queued(int q) { max_q_ = std::max(1, q); }

  // Producer: a slot to write the next frame into, or -1 if every slot is in
  // use. With no free slot the oldest queued frame is dropped: its slot is
  // returned and *stolen set (its frame in *dropped).
  int Acquire(bool *stolen, uint64_t *dropped) {
    if (stolen)
      *stolen = false;
    for (int s = 0; s < n_; ++s)
      if (st_[s] == St::Free) {
        st_[s] = St::Writing;
        return s;
      }
    if (q_.empty())
      return -1;
    const int s = q_.front();
    q_.erase(q_.begin());
    if (stolen)
      *stolen = true;
    if (dropped)
      *dropped = fr_[s];
    st_[s] = St::Writing;
    return s;
  }
  // Producer: slot `s` now holds `frame`. Queues it; beyond max_queued the
  // oldest queued frame is dropped and its slot freed. Returns that slot (its
  // frame in *dropped), or -1.
  int Queue(int s, uint64_t frame, uint64_t *dropped) {
    fr_[s] = frame;
    st_[s] = St::Queued;
    q_.push_back(s);
    int freed = -1;
    while ((int)q_.size() > max_q_) {
      const int o = q_.front();
      q_.erase(q_.begin());
      if (dropped)
        *dropped = fr_[o];
      st_[o] = St::Free;
      freed = o;
    }
    return freed;
  }
  // Producer: give back an acquired slot without queueing it.
  void Abandon(int s) { st_[s] = St::Free; }
  // Consumer: the oldest queued slot (in frame order), or -1.
  int Pop() {
    if (q_.empty())
      return -1;
    const int s = q_.front();
    q_.erase(q_.begin());
    st_[s] = St::InUse;
    return s;
  }
  // Consumer: done reading slot `s`.
  void Release(int s) { st_[s] = St::Free; }

private:
  int n_, max_q_;
  St st_[kMaxSlots] = {};
  uint64_t fr_[kMaxSlots] = {};
  std::vector<int> q_;
};

// INTERP17: present times on the frame interpolation schedule (gxf12_presenter.h; unit test
// outputs/pc-tests/interp17). Guest frame N is cut at t_cut; its in-between frame I is shown at
// t_cut + D and N at t_cut + D + P/2, so I and N are P/2 apart and the cadence follows the guest's cut
// times (no drift). D = p90 of how long after the cut the frames were ready (last 32 samples) + 1 ms:
// steady presents although the host's time per frame jitters. Times in microseconds.
// Only a synchronized swap/compositor wait needs an early submission.
// An immediate swap must reach its scheduled deadline, otherwise a ready
// in-between image and its real image cluster instead of being P/2 apart.
inline int64_t PresentLeadUs(int64_t composition_period, int swap_interval, bool dwm_wait) {
  return (dwm_wait || swap_interval != 0) ? std::max<int64_t>(0, composition_period / 2) : 0;
}

inline bool DropLatePresent(int64_t now, int64_t target, int64_t composition_period, int queued) {
  // A synchronized presenter cannot catch up by swapping an old queue at the
  // same rate it arrives. Skip expired images only when a newer one is waiting.
  return queued > 0 && now > target + std::max<int64_t>(1000, composition_period);
}

// Optional interpolation work must not gain a larger allowance when overload
// slows the 30-Hz game. Presentation timing still follows the measured cadence.
inline int64_t InterpWorkPeriodUs(int64_t measured_period) {
  return std::clamp<int64_t>(measured_period, 1000, 33367);
}
inline int64_t InterpWorkBudgetUs(int64_t measured_period, int percent) {
  return InterpWorkPeriodUs(measured_period) * std::clamp(percent, 75, 95) / 100;
}
inline int64_t InterpRetryDelayUs(int64_t overload, int64_t minimum) {
  minimum = std::clamp<int64_t>(minimum, 100000, 500000);
  return minimum + std::min<int64_t>(500000 - minimum, std::clamp<int64_t>(overload, 0, 500000) * 8);
}
inline bool InterpPreparationCoolingDown(int64_t now, int64_t until, bool rerender, bool forced) {
  return rerender && !forced && now < until;
}

class PacePlanner {
public:
  // kind 1 = in-between frame, 2 = real frame; ready = when its image was complete.
  int64_t Plan(int kind, int64_t t_cut, int64_t period, int64_t ready) {
    const int64_t half = period / 2;
    Sample(ready - t_cut - (kind == 2 ? half : 0));
    return t_cut + d_ + (kind == 2 ? half : 0);
  }
  int64_t D() const { return d_; }

private:
  void Sample(int64_t v) {
    s_[i_] = v;
    i_ = (i_ + 1) % 64;
    n_ = std::min(n_ + 1, 64);
    int64_t t[64];
    std::copy(s_, s_ + n_, t);
    const int k = std::min((n_ * 9) / 10, n_ - 1);
    std::nth_element(t, t + k, t + n_);
    // D follows the p90 in small steps (up <= 0.4 ms, down 0.05 ms per sample), so consecutive presents
    // stay P/2 apart; the first samples set it directly.
    const int64_t want = std::clamp<int64_t>(t[k] + 1000, 2000, 80000);
    d_ = n_ < 8 ? want : want > d_ ? std::min(want, d_ + 400) : std::max(want, d_ - 50);
  }
  int64_t s_[64] = {};
  int n_ = 0, i_ = 0;
  int64_t d_ = 20000;
};

class ParallelFor {
public:
  explicit ParallelFor(int workers) {
    for (int i = 0; i < workers; ++i)
      threads_.emplace_back([this] { Loop(); });
  }
  ~ParallelFor() {
    {
      std::lock_guard<std::mutex> lk(m_);
      quit_ = true;
    }
    cv_work_.notify_all();
    for (auto &t : threads_)
      t.join();
  }
  ParallelFor(const ParallelFor &) = delete;
  ParallelFor &operator=(const ParallelFor &) = delete;

  int workers() const { return (int)threads_.size(); }

  // fn(begin, end) over [0, n) in chunks of `grain`; the calling thread
  // takes part. Returns when every chunk is done.
  void Run(size_t n, size_t grain, const std::function<void(size_t, size_t)> &fn) {
    grain = std::max<size_t>(1, grain);
    if (threads_.empty() || n <= grain) {
      for (size_t b = 0; b < n; b += grain)
        fn(b, std::min(n, b + grain));
      return;
    }
    {
      std::lock_guard<std::mutex> lk(m_);
      fn_ = &fn;
      n_ = n;
      grain_ = grain;
      next_.store(0, std::memory_order_relaxed);
      busy_ = (int)threads_.size();
      ++gen_;
    }
    cv_work_.notify_all();
    Work();
    std::unique_lock<std::mutex> lk(m_);
    cv_done_.wait(lk, [&] { return busy_ == 0; });
    fn_ = nullptr;
  }

private:
  void Work() {
    for (;;) {
      const size_t b = next_.fetch_add(grain_, std::memory_order_relaxed);
      if (b >= n_)
        break;
      (*fn_)(b, std::min(n_, b + grain_));
    }
  }
  void Loop() {
    uint64_t seen = 0;
    for (;;) {
      {
        std::unique_lock<std::mutex> lk(m_);
        cv_work_.wait(lk, [&] { return quit_ || gen_ != seen; });
        if (quit_)
          return;
        seen = gen_;
      }
      Work();
      {
        std::lock_guard<std::mutex> lk(m_);
        if (--busy_ == 0)
          cv_done_.notify_one();
      }
    }
  }

  std::vector<std::thread> threads_;
  std::mutex m_;
  std::condition_variable cv_work_, cv_done_;
  uint64_t gen_ = 0;
  bool quit_ = false;
  const std::function<void(size_t, size_t)> *fn_ = nullptr;
  size_t n_ = 0, grain_ = 1;
  std::atomic<size_t> next_{0};
  int busy_ = 0;
};

// PERF13: hand-off of the FIFO parse between the thread that writes the FIFO
// (the guest CPU thread, which also syncs when the guest idles) and a parse
// worker. gx.cpp uses it with its s_parse_mtx and DrainAndParseLocked; the
// unit test (outputs/pc-tests/perf13) runs the same code under stress.
//  - `pending`: bytes written and not drained yet. The writer sets it (under
//    its FIFO lock); the drain clears it with release ordering.
//  - `busy`: the worker holds the parse mutex and has started a drain+parse.
//  - `gen`: counts drain+parse rounds that produced commands.
// Invariant: when IdleSync returns, every byte written before the call has
// been drained AND parsed (by the worker or by IdleSync itself), so the PE
// interrupts it carries were raised before the guest's idle skip advances the
// clock -- the pre-PERF13 guarantee of the CPU thread's own idle parse.
// `drain_parse` runs under the parse mutex, drains (clearing `pending`) and
// parses; it returns true when commands were added.
struct ParseHandoff {
  std::atomic<bool> pending{false};
  std::atomic<bool> busy{false};
  std::atomic<uint64_t> gen{0};

  // Worker: one round. `stop` (optional) is checked under the mutex.
  template <class M, class F>
  bool WorkerRound(M &m, F &&drain_parse, const std::atomic<bool> *stop = nullptr) {
    if (!pending.load(std::memory_order_acquire))
      return false;
    std::unique_lock<M> lk(m);
    if (stop && stop->load(std::memory_order_acquire))
      return false;
    busy.store(true, std::memory_order_seq_cst); // before the drain clears `pending`
    const bool grew = drain_parse();
    if (grew)
      gen.fetch_add(1, std::memory_order_release);
    busy.store(false, std::memory_order_release);
    return grew;
  }

  // UIT14: stops the worker for good. `stop` is set under the parse mutex, so
  // a round in progress has finished when this returns and no later
  // WorkerRound(..., &stop) parses again. Bounded (`tries` x 1 ms): an exit
  // never hangs on a thread that stopped holding the mutex (then the stop is
  // set without it; returns false). gx.cpp GX_StopParseWorker (main() calls it
  // before the guest RAM is freed; unit test outputs/pc-tests/uit14 T1/T2).
  template <class M>
  static bool StopWorker(M &m, std::atomic<bool> &stop, int tries = 200) {
    for (int i = 0; i < tries; ++i) {
      if (m.try_lock()) {
        stop.store(true, std::memory_order_release);
        m.unlock();
        return true;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    stop.store(true, std::memory_order_release);
    return false;
  }

  // Writer thread, at an idle point. Returns true when commands were parsed
  // (by anyone) since the previous call (`seen`, owned by the caller), i.e.
  // new PE interrupts may be pending. `*waited` = this call took the mutex.
  // kWaitInFlight=false is the unsafe variant the unit test's negative
  // control uses (it ignores a chunk the worker has drained but not parsed).
  template <bool kWaitInFlight = true, class M, class F>
  bool IdleSync(M &m, F &&drain_parse, uint64_t &seen, bool *waited = nullptr) {
    // `pending` (acquire) first: if it reads the worker's clear, the worker's
    // earlier busy=true is visible too, so a chunk in flight is waited for.
    const bool p = pending.load(std::memory_order_acquire);
    const bool b = kWaitInFlight && busy.load(std::memory_order_acquire);
    if (waited)
      *waited = false;
    if (p || b) {
      std::lock_guard<M> lk(m);
      if (drain_parse())
        gen.fetch_add(1, std::memory_order_release);
      if (waited)
        *waited = true;
    }
    const uint64_t g = gen.load(std::memory_order_acquire);
    const bool parsed = g != seen;
    seen = g;
    return parsed;
  }
};

} // namespace nwii::runtime::gx
