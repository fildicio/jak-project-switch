#include "Loader.h"

#include <algorithm>
#include <ranges>
#include <cstring>

#include "common/global_profiler/GlobalProfiler.h"
#include "common/util/FileUtil.h"
#include "common/util/Timer.h"
#include "common/util/compress.h"

#include "game/graphics/opengl_renderer/loader/LoaderStages.h"

// FIX 55 (AI-assisted): for switch_diag_enabled(), which gates the gpu probe. Off-Switch this
// header supplies an always-true stub, so the gate is inert on host builds.
#include "game/switch/run_log.h"
// FIX 70 / FIX 55 pattern (AI-assisted): Switch-only core pinning helpers,
// empty off-Switch.
#include "game/switch/platform.h"

#if defined(__SWITCH__)
#include "game/switch/imgui_stub.h"
#include <unistd.h>
#else
#include "third-party/imgui/imgui.h"
#endif

#include "common/versions/versions.h"

namespace {
// FIX 76 (AI-assisted): the jak1 overworld - which fr3 level can follow which.
// Order is priority. This is only a seed: transitions the game itself shows
// us at runtime (see record_transitions_locked) take priority over the table,
// and every candidate is checked against the disk before it is handed to the
// loader thread (read_binary_file on a missing name would kill the process).
const std::unordered_map<std::string, std::vector<std::string>> kJak1LevelAdjacency = {
    {"intro", {"training"}},
    {"training", {"village1"}},
    // FIX 76f: training/misty are warp/boat-only (blackout loads) - never prefetch targets.
    {"village1", {"beach", "jungle", "firecanyon"}},
    {"beach", {"village1"}},
    {"jungle", {"village1"}},
    {"firecanyon", {"village1", "village2"}},
    {"village2", {"firecanyon", "rolling", "sunken", "swamp", "ogre"}},
    {"ogre", {"village2", "village3"}},
    {"rolling", {"village2"}},
    {"sunken", {"village2"}},
    {"swamp", {"village2"}},
    {"village3", {"snow", "maincave", "lavatube", "ogre"}},
    {"snow", {"village3", "darkcave"}},
    {"maincave", {"village3"}},
    {"darkcave", {"snow"}},
    {"lavatube", {"village3", "citadel"}},
    {"citadel", {"lavatube"}},
};

// FIX 86 (AI-assisted): the jak2 seed. Haven City is hub-and-spoke: the
// persistent city shell (ctywide) touches every district, and every outdoor
// area is entered from the city. The l* sub-levels are deliberately absent -
// they load alongside their parent, so guessing one is a wasted cancel - and
// so are destinations reached only through blackout loads (elevator / train),
// which the game loads itself. Cold-start seed only: transitions the game
// shows us at runtime (record_transitions_locked) outrank this table.
const std::unordered_map<std::string, std::vector<std::string>> kJak2LevelAdjacency = {
    {"ctywide",
     {"ctymarka", "ctymarkb", "ctysluma", "ctyslumb", "ctyport", "ctyinda", "ctyindb",
      "ctypal", "ctyfarmb", "ctyasha", "forest", "drill", "tomba", "mountain"}},
    {"ctymarka", {"ctywide", "ctymarkb"}},
    {"ctymarkb", {"ctywide", "ctymarka"}},
    {"ctysluma", {"ctywide", "ctyslumb"}},
    {"ctyslumb", {"ctywide", "ctysluma"}},
    {"ctyinda", {"ctywide", "ctyindb"}},
    {"ctyindb", {"ctywide", "ctyinda"}},
    {"ctyport", {"ctywide"}},
    {"ctypal", {"ctywide"}},
    {"ctyfarmb", {"ctywide"}},
    {"ctyasha", {"ctywide"}},
    {"forest", {"ctywide"}},
    {"drill", {"ctywide"}},
    {"tomba", {"ctywide"}},
    {"mountain", {"ctywide"}},
};

// FIX 96 (AI-assisted): the jak3 seed, built from the player's own observed
// transitions (the "GAMEPLAY: enter" lines across the five card sessions,
// 2026-10-06..09) rather than guesswork. Jak 3 has three hubs: the Wasteland
// (desert) reaching its sub-areas and outposts, Spargus (wascitya), and the
// Haven City district web. l* sub-levels load alongside their parent, and
// title/intro/wasintro are blackout destinations the game loads itself - all
// deliberately absent, same policy as kJak2LevelAdjacency. Only each area's
// top entries ever matter (pick_prefetch_target_locked reads rank 1-2); the
// learned graph still outranks this once the session has real transitions.
const std::unordered_map<std::string, std::vector<std::string>> kJak3LevelAdjacency = {
    // Wasteland hub. desertb/f/g are jak3's l*-style sub-areas: the game itself
    // streams them in and out while you drive (the f96 log showed `PC unloading
    // desert / desinter` churn and `GAMEPLAY: enter desertb` mid-drive with no
    // door involved), so FIX 97 removed them from the seed - prefetching them
    // raced the game's own streaming (a 119 ms hitch right after
    // `[pf] start: background-caching desertb`). The desertb/f/g entries below
    // stay: FROM a sub-area, prefetching the parent desert back is a real guess.
    {"desert", {"wasdoors", "foresta", "factorya", "templex"}},
    {"desertb", {"desert", "wasdoors"}},
    {"desertf", {"desert"}},
    {"desertg", {"desert", "wasdoors"}},
    {"wasdoors", {"desert", "wascitya"}},
    // Spargus hub
    {"wascitya", {"wasdoors", "waspala", "wascityb"}},
    {"wascityb", {"wascitya"}},
    {"waspala", {"wascitya"}},
    // Haven City web
    {"ctyinda", {"ctyport", "ctyindb", "sewe"}},
    {"ctyindb", {"ctyinda", "ctysluma"}},
    {"ctyport", {"ctyinda", "desert"}},
    {"ctysluma", {"ctyslumb", "ctyindb"}},
    {"ctyslumb", {"ctysluma", "ctyslumc", "sewa"}},
    {"ctyslumc", {"ctyslumb", "freehq"}},
    {"freehq", {"ctyslumc"}},
    {"hiphog", {"ctyport"}},
    {"mhcitya", {"ctyport"}},
    // sewer chain (entered from ctyinda / ctyslumb)
    {"sewa", {"sewb", "sewe"}},
    {"sewb", {"sewa", "sewc"}},
    {"sewc", {"sewd"}},
    {"sewd", {"sewe"}},
    {"sewe", {"sewa", "ctyinda"}},
    // factory / forest cluster (from the Wasteland)
    {"foresta", {"factorya", "desert"}},
    {"factorya", {"factoryb", "foresta", "factoryd"}},
    {"factoryb", {"factorya"}},
    {"factoryd", {"factorya"}},
    // temple cluster
    {"templex", {"templea", "desert"}},
    {"templea", {"templed", "templex"}},
    {"templeb", {"templed"}},
    {"templed", {"templea", "templeb"}},
    // Wasteland outposts
    {"nsta", {"nstb", "desertg"}},
    {"nstb", {"nsta"}},
};

// FIX 86 (AI-assisted): prefetch may only volunteer work while the recycling
// pool holds at least this many free bytes - twice the budget logic's 16 MB
// pressure line, because a prefetch is work nobody asked for. The f85 BCn
// hardware session measured 67.8 MB free with 8/9 levels live in Haven City.
constexpr size_t kPfPressureFreeBytes = 32 * 1024 * 1024;
// FIX 87 (AI-assisted): the frame-gap EMA above which we never START a
// prefetch. 25.0 ms is the budget logic's idle-healthy/idle-lean line (see
// update()); at the gate nothing is in flight, so this is exactly "only
// volunteer work on idle-healthy frames". Bytes-free is not health - see the
// gate comment below for the f86 hardware session that proved it.
constexpr double kPfMaxFrameEmaMs = 25.0;
// FIX 96 (AI-assisted): jak3's own line. Jak 3 pins to 30 fps, so the frame-gap
// EMA floor is ~33.3 ms regardless of headroom - the 25.0 line above was
// calibrated on jak2 (where sub-25 windows exist) and meant the gate could NEVER
// open here: the Oct 9 f95 hardware session logged `pf 0 cached` for 28 minutes
// straight, every idle reason "frames are too slow to volunteer work", while
// FIX 95's own free-time telemetry measured 2.8-10.5 ms genuinely free on those
// same frames. 34.0 sits just under the 34.5 mid-flight pause line (see the
// skip in update()), so a started prefetch can keep streaming, and the earlier
// gates (load in flight / still staging / blackout / pool bytes) still run first.
constexpr double kPfMaxFrameEmaMsJak3 = 34.0;
// FIX 97 (AI-assisted): a prefetch may only START after the game's want-set
// (__pc-set-levels) has been unchanged for this many seconds - see the churn
// gate in set_want_levels. The f96 Wasteland session changed wants every few
// seconds for the whole drive (desert sub-areas streaming in and out), so 8 s
// is longer than any stream-in/out pair while still short enough that a player
// standing in a hub (where prefetch is the win) gets the neighbor cached.
constexpr int kPfWantStableSec = 8;
}  // namespace

Loader::Loader(const fs::path& base_path, int max_levels, GameVersion version)
    : m_base_path(base_path), m_max_levels(max_levels), m_game_version(version) {
  m_loader_thread = std::thread(&Loader::loader_thread, this);
  m_loader_stages = make_loader_stages();
}

Loader::~Loader() {
  {
    std::lock_guard<std::mutex> lk(m_loader_mutex);
    m_want_shutdown = true;
    m_loader_cv.notify_all();
  }
  m_loader_thread.join();
}

/*!
 * Try to get a loaded level by name. It may fail and return nullptr.
 * Getting a level will reset the counter for the level and prevent it from being kicked out
 * for a little while.
 *
 * This is safe to call from the graphics thread
 */
const LevelData* Loader::get_tfrag3_level(const std::string& level_name) {
  std::unique_lock<std::mutex> lk(m_loader_mutex);
  const auto& existing = m_loaded_tfrag3_levels.find(level_name);
  if (existing == m_loaded_tfrag3_levels.end()) {
    return nullptr;
  } else {
    existing->second->frames_since_last_used = 0;
    return existing->second.get();
  }
}

void Loader::debug_print_loaded_levels() {
  std::unique_lock<std::mutex> lk(m_loader_mutex);
  for (const auto& [name, _] : m_loaded_tfrag3_levels) {
    fmt::print("{}\n", name);
  }
}

/*!
 * The game calls this to give the loader a hint on which levels we want.
 * If the loader is not busy, it will begin loading the level.
 * This should be called on every frame.
 */
void Loader::set_want_levels(const std::vector<std::string>& levels) {
  std::unique_lock<std::mutex> lk(m_loader_mutex);
  m_desired_levels = levels;

#ifdef __SWITCH__
  // ---- FIX 76 (AI-assisted): area prefetch bookkeeping ----
  // FIX 97 (AI-assisted): remember when the want-set last changed. Every level
  // swap / sub-area stream-in / discard rewrites __pc-set-levels, so this
  // timestamp IS the "the game is streaming right now" signal the churn gate
  // below consults. (m_prev_desired_levels still holds last frame's set until
  // record_transitions_locked updates it.)
  if (m_prev_desired_levels != levels) {
    m_wants_stable_since = std::chrono::steady_clock::now();
  }
  record_transitions_locked(levels);

  // a level the game itself now holds is no longer prefetch bookkeeping
  for (auto& lev : levels) {
    m_prefetch_resident.erase(lev);
    m_prefetch_retired.erase(lev);
  }

  // a stale discard flag (the level finished or was already dropped) must not
  // block new prefetches forever.
  if (!m_prefetch_discard.empty() &&
      m_initializing_tfrag3_levels.find(m_prefetch_discard) ==
          m_initializing_tfrag3_levels.end() &&
      m_level_to_load != m_prefetch_discard) {
    m_prefetch_discard.clear();
  }

  const std::string* missing = nullptr;
  for (auto& lev : levels) {
    if (m_loaded_tfrag3_levels.find(lev) == m_loaded_tfrag3_levels.end()) {
      missing = &lev;
      break;
    }
  }

  if (!missing) {
    // Nothing is missing. If the loader is completely idle, quietly start
    // caching the most likely next area - this is the whole point of FIX 76:
    // by the time the player crosses, the level is already here and the
    // transition costs the frame nothing.
    // FIX 85 (AI-assisted): hardware kill switch. Create
    // sdmc:/switch/jakN/gk_no_pf.txt to turn the area prefetch off for that
    // game without redeploying - the 10-second A/B if a frame cost ever shows
    // up on hardware. Read once per session, like gk_pin.txt / gk_no_vag.txt.
#ifdef __SWITCH__
    static const bool s_pf_disabled = [&]() {
      const bool off = access(fmt::format("sdmc:/switch/{}/gk_no_pf.txt",
                                          game_version_names[m_game_version])
                                  .c_str(),
                              F_OK) == 0;
      if (off) {
        switch_run_logf("[pf] DISABLED (sdmc:/switch/%s/gk_no_pf.txt present)",
                        game_version_names[m_game_version]);
      }
      return off;
    }();
    if (s_pf_disabled) {
      return;
    }
#endif
    // FIX 86 (AI-assisted): the old count gate
    // `loaded + 1 >= min(max_live_levels(), 5)` was a jak1-shaped proxy for
    // "no memory for a volunteer level". jak2's Haven City legitimately holds
    // 8-9 live levels at all times - the f85 hardware session showed live 8/9
    // with `pf 0 cached` in every telemetry line, i.e. the prefetch could
    // never start in exactly the place the player crosses districts. Replace
    // it with the loader's own honest signals: the live-level hard cap
    // (FIX 46), pool allocations that actually failed (FIX 76c), and the
    // pool's free bytes at a margin above the budget logic's pressure line.
    const char* pf_blocked_by = nullptr;
    // FIX 97 (AI-assisted): DON'T VOLUNTEER WORK WHILE THE GAME IS STREAMING.
    // The f96 hardware session (2026-10-09, Wasteland drive) proved the EMA
    // gate alone can't tell "healthy 30 fps" from "borderline 30 fps with the
    // game's own streaming in progress": jak3 pins 30 fps, so the EMA sat just
    // under the 34.0 line while the game itself swapped desert sub-areas every
    // few seconds. Prefetching desertb on top of that produced a 119 ms hitch
    // at the start and pinched the staging budget for the game's own loads for
    // the rest of the drive - 9142 cam hitches in one 5-minute session, the
    // "struggled a lot to load new areas" report. A changed want-set is the
    // honest streaming signal; require kPfWantStableSec of quiet first.
    if (std::chrono::steady_clock::now() - m_wants_stable_since <
        std::chrono::seconds(kPfWantStableSec)) {
      pf_blocked_by = "the game is streaming levels (want-set churn)";
    } else if (!m_prefetch_discard.empty()) {
      pf_blocked_by = "a cancel is still draining";
    } else if (m_blackout) {
      pf_blocked_by = "blackout load in progress";
    } else if (!m_level_to_load.empty()) {
      pf_blocked_by = "a level load is in flight";
    } else if (!m_initializing_tfrag3_levels.empty()) {
      pf_blocked_by = "a level is still staging";
    } else if ((int)m_loaded_tfrag3_levels.size() + 1 > max_live_levels()) {
      pf_blocked_by = "at the live-level cap";
    } else if (m_prefetch_resident.size() >= 2) {
      pf_blocked_by = "two prefetched levels already resident";
    } else if (m_buffer_pool.failed_allocations() > 0) {
      pf_blocked_by = "the buffer pool had a failed allocation";
    } else if (m_frame_gap_ema_ms >
               (m_game_version == GameVersion::Jak3 ? kPfMaxFrameEmaMsJak3 : kPfMaxFrameEmaMs)) {
      // FIX 87 (AI-assisted): bytes-free is not health. The f86 hardware log
      // (2026-10-09) opened this gate exactly once, at a 32-40 ms frame-gap
      // EMA with the loader in catchup the whole session: the volunteer then
      // staged a district nobody asked for into a loader that was
      // staging-budget-bound (not memory-bound), the wanted atollext stream
      // stretched to 33.15 s (the same load class took 9.97 s in the f85
      // log), and a `gc 746 tex` reclaim evicted the volunteer before it was
      // ever used - `pf 0 cached` in all 33 telemetry lines and 238 cam
      // hitches in the 140 s session. Free pool bytes say nothing about
      // spare frame budget, so the gate must ask both questions.
      pf_blocked_by = "frames are too slow to volunteer work";
    } else if (m_buffer_pool.pooled_bytes() < kPfPressureFreeBytes) {
      pf_blocked_by = "the buffer pool is low on free bytes";
    }
    if (pf_blocked_by) {
      // FIX 86: one throttled line per reason change, so the next hardware
      // log states the prefetch's situation instead of us inferring it from
      // `pf 0 cached` again.
      static std::string s_pf_diag_last;
      static auto s_pf_diag_at =
          std::chrono::steady_clock::now() - std::chrono::hours(1);
      const auto pf_now = std::chrono::steady_clock::now();
      if (s_pf_diag_last != pf_blocked_by || pf_now - s_pf_diag_at > std::chrono::seconds(30)) {
        s_pf_diag_last = pf_blocked_by;
        s_pf_diag_at = pf_now;
        // FIX 87: switch_run_logf is printf-style (format(printf,1,2)), NOT
        // fmt - the f86 build logged a literal "{}" here (and passed a
        // std::string through varargs, which is UB). %s it is.
        switch_run_logf("[pf] idle, not caching: %s", pf_blocked_by);
      }
      return;
    }
    // FIX 76f (AI-assisted): no prefetch on the jak1 islands. Geyser Rock (training) and
    // Misty Island are only reached by warp gate / boat, which are blackout loads, so a
    // background guess is never used there - and the f76e log showed the beach prefetch
    // on Geyser Rock costing 6-11 ms per staged texture with frames already at 39 ms.
    if (m_game_version == GameVersion::Jak1) {
      for (const auto& d : m_desired_levels) {
        if (d == "training" || d == "misty") {
          return;
        }
      }
    }
    auto target = pick_prefetch_target_locked();
    if (!target) {
      return;
    }
    m_level_to_load = *target;
    m_prefetch_target = *target;
    // FIX 86: make the start visible. `pf N cached` only moves when the level
    // finishes staging, so without this line a mid-staging cancel looks
    // exactly like a gate problem.
    // FIX 86: make the start visible. `pf N cached` only moves when the level
    // finishes staging, so without this line a mid-staging cancel looks
    // exactly like a gate problem. (FIX 87: %s, see the idle-line note.)
    switch_run_logf("[pf] start: background-caching %s", target->c_str());
    lk.unlock();
    m_loader_cv.notify_all();
    return;
  }

  if (!m_prefetch_target.empty()) {
    // Is the in-flight prefetch something the game now wants? (Check every
    // missing level, not just the first - cities request several at once.)
    bool target_wanted = false;
    for (auto& lev : levels) {
      if (lev == m_prefetch_target &&
          m_loaded_tfrag3_levels.find(lev) == m_loaded_tfrag3_levels.end()) {
        target_wanted = true;
        break;
      }
    }
    if (target_wanted) {
      // PREFETCH HIT: the level we are quietly caching is exactly what the
      // game wants. Let it finish - from now on it is a normal load, just a
      // nearly-complete one. (LoadBoost may engage as usual for real loads.)
      m_load_start[m_prefetch_target] = std::chrono::steady_clock::now();
      m_prefetch_target.clear();
      return;
    }
    // PREFETCH MISS: we guessed wrong and the game wants something else.
    // Cancel the prefetch: if it is still being read from disk, free the slot
    // now (loader_thread also checks the discard flag when the read ends so
    // it won't publish or clobber); if it is mid-staging, update() drops it
    // next frame through the FIX 63 unload path.
    // FIX 76d (AI-assisted): a cancelled guess is also cooled down for a
    // minute - the f76c log showed `dropped training mid-staging` twice in a
    // row in village1 (the rank-3 static guess, re-picked immediately after
    // each cancel). Every retry costs real staging frames for nothing.
    m_prefetch_cooldown[m_prefetch_target] =
        std::chrono::steady_clock::now() + std::chrono::seconds(60);
    m_prefetch_discard = m_prefetch_target;
    if (m_level_to_load == m_prefetch_target) {
      m_level_to_load.clear();
    }
    m_prefetch_target.clear();
  }
#endif

  if (!m_level_to_load.empty()) {
    // can't do anything, we're loading a level right now
    return;
  }

  if (!m_initializing_tfrag3_levels.empty()) {
    // can't do anything, we're initializing a level right now
    return;
  }

  // loader isn't busy, try to load one of the requested levels.
  for (auto& lev : levels) {
    auto it = m_loaded_tfrag3_levels.find(lev);
    if (it == m_loaded_tfrag3_levels.end()) {
      // we haven't loaded it yet. Request this level to load and wake up the thread.
      m_level_to_load = lev;
#ifdef __SWITCH__
      // FIX 36 Task 3 (AI-assisted): start the "ready in" clock at request
      // time - it is read and logged when the level finishes staging, inside
      // update()'s finish-stages block, under this same mutex.
      m_load_start[lev] = std::chrono::steady_clock::now();
#endif
      lk.unlock();
      m_loader_cv.notify_all();
      return;
    }
  }
}

#ifdef __SWITCH__
/*!
 * FIX 76 (AI-assisted): learn the level graph by watching the game.
 *
 * set_want_levels() is called every frame with the levels GOAL currently
 * holds. Whenever a new name appears that wasn't there before, record an edge
 * from every previously-held level to it. Counts accumulate, so the most
 * traveled crossings win when we pick a prefetch target. Works for every
 * game, needs no tables, and heals wrong guesses in the static jak1 table
 * after a single crossing. Call with m_loader_mutex held.
 */
void Loader::record_transitions_locked(const std::vector<std::string>& levels) {
  if (!m_prev_desired_levels.empty()) {
    for (auto& neu : levels) {
      if (std::find(m_prev_desired_levels.begin(), m_prev_desired_levels.end(), neu) !=
          m_prev_desired_levels.end()) {
        continue;
      }
      for (auto& alt : m_prev_desired_levels) {
        if (alt != neu) {
          m_learned_transitions[alt][neu]++;
        }
      }
    }
  }
  m_prev_desired_levels = levels;
}

/*!
 * FIX 76 (AI-assisted): pick the next level to prefetch, or nullopt.
 * Learned transitions (strongest first) beat the static jak1 table.
 * Call with m_loader_mutex held.
 */
std::optional<std::string> Loader::pick_prefetch_target_locked() {
  auto skipped = [&](const std::string& name) {
    if (m_game_version == GameVersion::Jak1 && (name == "training" || name == "misty")) {
      return true;  // FIX 76f: islands are blackout-only, never prefetch them
    }
    // FIX 76d: recently-cancelled guesses stay skipped until their cooldown
    // expires; the entry is erased lazily so the map can't grow forever.
    if (auto cd = m_prefetch_cooldown.find(name); cd != m_prefetch_cooldown.end()) {
      if (std::chrono::steady_clock::now() < cd->second) {
        return true;
      }
      m_prefetch_cooldown.erase(cd);
    }
    return m_loaded_tfrag3_levels.count(name) > 0 || m_prefetch_resident.count(name) > 0 ||
           m_prefetch_retired.count(name) > 0 || m_prefetch_target == name ||
           m_initializing_tfrag3_levels.count(name) > 0 || m_level_to_load == name;
  };

  std::string best;
  int best_count = 0;
  for (const auto& from : m_desired_levels) {
    auto lit = m_learned_transitions.find(from);
    if (lit == m_learned_transitions.end()) {
      continue;
    }
    for (const auto& [to, count] : lit->second) {
      if (count > best_count && !skipped(to)) {
        best = to;
        best_count = count;
      }
    }
  }

  // FIX 86 (AI-assisted): jak2 gets a seed table too (hub-and-spoke, see
  // kJak2LevelAdjacency). FIX 96 (AI-assisted): jak3 finally gets one as well,
  // built from the player's own observed transitions (kJak3LevelAdjacency).
  const std::unordered_map<std::string, std::vector<std::string>>* seed_table =
      m_game_version == GameVersion::Jak1   ? &kJak1LevelAdjacency
      : m_game_version == GameVersion::Jak2 ? &kJak2LevelAdjacency
      : m_game_version == GameVersion::Jak3 ? &kJak3LevelAdjacency
                                            : nullptr;
  if (best.empty() && seed_table) {
    // FIX 76d (AI-assisted): the static table is a priority list, not a menu.
    // The old scan fell through to rank-2/3/4 guesses as soon as the top
    // entry was already cached - which is exactly when the guess quality is
    // worst. The f76c log prefetched (and then cancelled) `training`, the
    // LAST village1 entry, twice in a row while beach/jungle sat cached:
    // pure wasted staging. Now only each area's #1 candidate counts, and if
    // it is already resident the learned graph (or nothing) decides.
    for (const auto& from : m_desired_levels) {
      auto sit = seed_table->find(from);
      // FIX 96: compare against the SELECTED table's end. This was hardcoded to
      // jak1's map and only worked by accident (iterators from different maps
      // never compare equal, so the guard never fired).
      if (sit == seed_table->end() || sit->second.empty()) {
        continue;
      }
      // FIX 76e (AI-assisted): the top TWO entries are trustworthy - in jak1
      // village1's list that is beach then jungle, and the f76c noise was all
      // rank 3+ (training). 76d cut this to #1 only, which overcorrected: a
      // fresh f76d session never prefetched the jungle (its #2), and the
      // player's first crossing paid a full visible 16.87 s load.
      for (size_t k = 0; k < sit->second.size() && k < 2; k++) {
        if (!skipped(sit->second[k])) {
          best = sit->second[k];
          break;
        }
      }
      if (!best.empty()) {
        break;
      }
    }
  }

  if (best.empty()) {
    return std::nullopt;
  }
  // Never hand the loader thread a name without a file: read_binary_file
  // throws on a missing file and the whole process dies. Retire bad names
  // for the session so we don't stat the SD card every frame.
  if (!fs::exists(m_base_path / fmt::format("{}.fr3", best))) {
    m_prefetch_retired.insert(best);
    return std::nullopt;
  }
  return best;
}
#endif

#ifdef __SWITCH__
/*!
 * FIX 36 Task 3 (AI-assisted): adaptive loader budget.
 *
 * The flat "2 ms / 256 KB per frame" Switch budget from FIX 33 was tuned for
 * steady gameplay, but it also applied during blackout loads where there is
 * nothing to protect - the game is already stalled behind update_blocking().
 * A city re-entry then paid tens of seconds of 256 KB/frame uploads
 * (SWITCH_FIX36_AGENT_BRIEF.md §1C).
 *
 * Now the budget follows what the frame can actually afford, decided from a
 * frame-gap EMA (~8-frame average, single gaps clamped at 200 ms so one hitch
 * can't pin it high):
 *   blackout - the game is stalled waiting for us: go big (12 ms / 4 MB).
 *   healthy  - EMA under 25 ms: stream faster (4 ms / 1 MB).
 *   lean     - around 30 fps: the old flat budget (2 ms / 256 KB).
 *   struggle - badly missing 30 fps: don't make it worse (1 ms / 128 KB).
 * Render thread only, called once per update() before the stages run.
 */
#ifdef __SWITCH__
// FIX 95 (AI-assisted): FRAME-FREE-TIME BUDGET FOR GAMEPLAY STREAMING (no clock changes).
//
// f94b city log: while ctyport-sized levels (848 textures) streamed during play, every
// frame paid `slow setup` 10-15 ms (stage texture ~9 ms on a 7-8 ms tier + 2-4 mip
// chains), EMA 37-38 ms = the city slow motion. The f93 [phase] lines show the render
// thread itself is ~20 ms of buckets and ~11 ms of pcrtc, which is mostly the swapchain
// acquire WAIT: real idle time that the fixed 7-8 ms tiers overshoot. OpenGLRenderer
// publishes loader+pcrtc per frame; the gameplay loader now spends at most that minus a
// margin (min over 4 frames). The texture stage still dispatches >= 1 texture per frame,
// so streaming can never stall - it just no longer outgrows the frame.
extern double g_switch_frame_free_ms;
namespace {
double s_free_hist[4] = {-1, -1, -1, -1};
int s_free_i = 0;
u32 s_free_frame = 0;
constexpr double kFreeMarginMs = 4.0;  // pcrtc's own blit/FSR submit (~1-2 ms) + safety
double switch_free_ms_min() {
  double m = -1;
  for (double v : s_free_hist) {
    if (v >= 0 && (m < 0 || v < m)) {
      m = v;
    }
  }
  return m;
}
}  // namespace
#endif

void Loader::update_frame_budget() {
  const auto now = std::chrono::steady_clock::now();
  if (m_last_update_tp.time_since_epoch().count() > 0) {
    double gap = std::chrono::duration<double, std::milli>(now - m_last_update_tp).count();
    gap = std::min(gap, 200.0);
    m_frame_gap_ema_ms += (gap - m_frame_gap_ema_ms) * 0.125;
    // FIX 76e (AI-assisted): the raw single-frame gap, for the prefetch
    // backoff - the EMA alone cannot tell "one missed frame just now" (it
    // only moves by 1/8 of the outlier) from "recovered and locked".
    m_last_frame_gap_ms = gap;
  }
  m_last_update_tp = now;
#ifdef __SWITCH__
  if (!m_in_update_blocking) {
    s_free_hist[s_free_i] = g_switch_frame_free_ms;
    s_free_i = (s_free_i + 1) & 3;
    ++s_free_frame;
  }
#endif

  // FIX 38 (AI-assisted): BACKLOG BEATS FRAME TIME.
  //
  // Driving the budget purely from the frame gap created a death spiral: the
  // city runs at ~13 fps, so the EMA sat at 35-85 ms permanently, so the
  // loader sat in "struggle" at 1 ms / 128 KB per frame = ~1.6 MB/s. That is
  // why the city took forever to repopulate and why NPCs and the zoomer were
  // missing for tens of seconds after re-entry (hardware log, 2026-09-25:
  // "budget ms=1.0 tex_kb=128 mode=struggle" alternating with "lean", with
  // live=7 want=5 the whole time). The logic was exactly backwards - it
  // throttled hardest precisely when there was most to load.
  //
  // Now: if there is anything queued, we are in catch-up and get a real
  // budget. Frame time may only modulate WITHIN catch-up, never below the
  // floor. Dropping a few frames while the world populates is what the player
  // wants; a 30-second wait is not.
  size_t pending = 0;
  size_t game_pending = 0;
#ifdef __SWITCH__
  // FIX 76d (AI-assisted): hoisted outside the lock below, so the dispatch-cap
  // decision at the end of this function knows whether what we are currently
  // staging is our own prefetch.
  size_t prefetch_in_flight = 0;
#endif
  {
    std::unique_lock<std::mutex> lk(m_loader_mutex);
    pending = m_initializing_tfrag3_levels.size() + (m_level_to_load.empty() ? 0 : 1);
    if (m_desired_levels.size() > m_loaded_tfrag3_levels.size()) {
      pending += m_desired_levels.size() - m_loaded_tfrag3_levels.size();
    }
    game_pending = pending;
#ifdef __SWITCH__
    // FIX 76 (AI-assisted): prefetch work must not lower the resolution.
    // LoadBoost streaming is a visible tell ("the game is loading"), and the
    // whole point of prefetching is that the player can't tell. Only work the
    // game actually asked for may engage it. The tier selection below still
    // sees `pending` (prefetch stages at the proven f74c catchup rate); only
    // LoadBoost is keyed off `game_pending`.
    if (!m_prefetch_target.empty()) {
      if (m_initializing_tfrag3_levels.count(m_prefetch_target)) {
        prefetch_in_flight++;
      }
      if (m_level_to_load == m_prefetch_target) {
        prefetch_in_flight++;
      }
      game_pending -= std::min(game_pending, prefetch_in_flight);
    }
#endif
  }

  // FIX 52 (AI-assisted): THE ONE TUNING KNOB THAT COULD NOT BE TUNED BEFORE.
  //
  // The tiers below pick a budget from the frame-gap EMA, which times the whole frame and
  // is therefore dominated by the renderer. Two consecutive sessions of loader work
  // (FIX 47-49) were spent adjusting those numbers while the logs showed
  // `[phase] loader 0.01 | pcrtc 22.33`, i.e. the loader's own attributed cost was
  // one hundredth of a millisecond. The signal was never about the loader.
  //
  // The texture budget governs identical work however it is chosen, so it is safe -- and
  // correct -- to shrink it when the loader's *own measured* GPU cost is high. This does
  // not undo FIX 38 ("backlog beats frame time"): the floor is untouched, and the tier is
  // only ever lowered by evidence that the loader itself is what the GPU is stuck on.
  //
  // Note the asymmetry with FIX 49, which is the whole reason this is not a repeat of it:
  // FIX 49 clamped the MIP RATE against a wall-clock budget the upload had already
  // exceeded, so the clamp was permanently true and the queue grew to 768. This clamps
  // the TEXTURE BYTE CAP against a measured GPU cost, and only while the GPU is
  // demonstrably saturated by this loader -- if the cost is low, nothing changes at all.
  double gpu_scale = 1.0;
  if (m_loader_gpu_ema_ms > 12.0) {
    gpu_scale = 0.25;  // the loader alone is eating >12 ms of GPU per frame
  } else if (m_loader_gpu_ema_ms > 6.0) {
    gpu_scale = 0.5;   // significant; slow down before it becomes the stall it looks like
  }

  // FIX 39 (AI-assisted): a blackout is a loading screen and a backlog is a stream-in;
  // both are windows where resolution is worth trading for load speed.
  // (FIX 76: `game_pending` excludes pure-prefetch work on Switch - a hidden
  // prefetch must never show itself as a resolution dip.)
  loadboost_set_streaming(m_blackout || game_pending > 0);

  // FIX 71 (AI-assisted): fast-load CPU clocks while the screen is black (see
  // switch/platform.cpp). Scoped to blackouts on purpose -- the boost
  // configuration also clamps the GPU to its minimum clock, so unlike
  // loadboost it must NOT extend to the in-gameplay streaming backlog.
  switch_platform::switch_set_cpu_boost(m_blackout);
  switch_platform::switch_clock_tick();  // FIX 92


  LoaderFrameBudget want;
  const char* mode;
#ifdef __SWITCH__
  // FIX 94 (AI-assisted): FROZEN LOADS GET THE WHOLE FRAME.
  //
  // update_blocking() calls update() back to back with no frame presented (the game
  // is frozen behind the save-load/warp sweep), but m_blackout is already false there,
  // so every call fell into the GAMEPLAY tiers. The f93 log shows the save-load sweep
  // at `catchup-floor (ema 48.6)` -> 8 ms per call, and the >45 ms EMA also tripped
  // the 2-dispatch crawl cap below. Each call also repays the fixed per-update() costs
  // (mip drain, garbage, GPU probe fence), so small slices are pure overhead here.
  // A black-screen frame (m_blackout) is likewise invisible: 24 ms instead of 12.
  if (m_in_update_blocking) {
    want = {40.f, 32 * 1024 * 1024, 32768};
    mode = "blocking";
  } else if (m_blackout) {
    // FIX 94b: back to 12 ms. The f94 log showed 24 ms (+ the blackout mip burst) made
    // the fade-out frames 150-250 ms long, and the blackout window was no shorter.
    want = {12.f, 4 * 1024 * 1024, 4096};
    mode = "blackout";
  } else if (pending > 0) {
#else
  if (m_blackout) {
    want = {12.f, 4 * 1024 * 1024, 4096};
    mode = "blackout";
  } else if (pending > 0) {
#endif
    // FIX 46b (AI-assisted): CATCH-UP MUST NOT DEEPEN A DROPPED FRAME.
    //
    // FIX 38 fixed the death spiral where a bad frame rate throttled the loader
    // to nothing. But it overcorrected in the other direction: `catchup` (8 ms /
    // 2 MB) was handed out whenever the EMA was <= 45 ms, i.e. *including* the
    // case where the frame is already at 30 fps and owes nothing. On the Switch
    // the frame is ~32 ms of a 33.3 ms budget with zero slack (FIX 43), so 8 ms
    // of loader work does not fit in the slack -- it is simply added to the
    // frame. That is the "slow motion while an area loads", and it is why
    // lowering the resolution (LoadBoost) never helped: the frame is not
    // pixel-bound.
    //
    // The floor stays (loading must never be throttled to a crawl again - that
    // was FIX 38's whole point), but above the floor we now only take more when
    // the frame demonstrably has room. A frame at 30 fps keeps the *floor*
    // instead of the larger budget, so the extra work is spread over more frames
    // rather than injected into one.
    //
    // Threshold is 30 fps (33.3 ms) with a little tolerance, not the old 45 ms:
    // 45 ms is already a dropped frame in a 30 fps target, which is far too late
    // to start being careful.
    constexpr double kFrameHasRoomMs = 30.0;
    constexpr double kFrameIsDroppedMs = 38.0;
    // FIX 50 (AI-assisted): ON SWITCH, STOP KEYING THE LOADER OFF FRAME TIME ALONE.
    //
    // The 2026-09-27 hardware log finally separated the two costs:
    //
    //   [phase] setup 0.01 | loader 0.01 | buckets 32.31 | blit 0.00 | bucket-sum 30.94
    //   [loader] level lwidea ready in 19.88s (budget catchup-floor)
    //   [loader] tex stage: 1222 textures, upload 2203.1ms
    //
    // The renderer was spending ~32 ms/frame (mostly the blit stall FIX 50 removes),
    // which pushed the frame-gap EMA over 38 ms, which pinned the loader to
    // `catchup-floor` (4 ms/frame) for the entire city. So a 19.9 s load was spent
    // uploading textures at 4 ms/frame while the loader's own measured cost was
    // 0.01 ms - the frame time was almost entirely the renderer's, and throttling
    // the loader could never have helped it. FIX 38's rule ("backlog beats frame
    // time") was being defeated by a renderer problem the loader cannot see.
    //
    // The floor still exists and the EMA still matters - loading must never go back
    // to being throttled to a crawl (FIX 38), but "the frame is slow" is no longer
    // sufficient reason to starve the loader when there is a backlog. Raise the
    // floor so a real load progresses at a useful rate, and keep the larger tiers
    // for frames that genuinely have room.
    if (m_frame_gap_ema_ms > kFrameIsDroppedMs) {
      want = {8.f, 2 * 1024 * 1024, 2048};
      mode = "catchup-floor";
    } else if (m_frame_gap_ema_ms > kFrameHasRoomMs) {
      // FIX 68 (AI-assisted): pace gets the floor's byte cap and a 7 ms line.
      //
      // The F66/F67 hardware logs (2026-10-03, jak2 atoll/city + jak1
      // beach/jungle) showed this tier IS the remaining asset delay. Every
      // stream-in load lands here and stays here: the renderer holds the
      // frame-gap EMA at 30-38 ms for the whole stream, so the mode never
      // upgrades to `catchup`. At the measured ~1.6 ms per texture dispatch
      // the old 5 ms line admitted only ~3 textures per frame - jak2's atoll
      // (848 textures, 1167 ms of upload) took 9.97 s at that rate, while the
      // same engine serving a load from `catchup` (8 ms / 2 MB) finishes
      // 0.4-1.8 s. jak1's beach/jungle (7-9 s) sat in the same tier.
      //
      // The fear that kept this tier small (FIX 46b: don't deepen a 30 fps
      // frame) is now covered better elsewhere: FIX 52's gpu_scale halves the
      // byte cap whenever the loader's OWN measured GPU cost exceeds 6 ms
      // (measured: never, in 1868 probes), and the per-frame timer remains the
      // hard stop - the worst case is ~8.6 ms of submit on a frame that was
      // already ~32 ms, in exchange for finishing ~40% sooner. The dip window
      // itself then shrinks too: a shorter dip at 28 fps beats a long one at
      // 30. 7 ms (not 8) keeps a visible delta from catchup/floor in the logs
      // so the next session can still tell which tier served a load.
      want = {7.f, 2 * 1024 * 1024, 2048};
      mode = "catchup-pace";
    } else {
      want = {8.f, 2 * 1024 * 1024, 2048};
      mode = "catchup";
    }
    // FIX 52 (AI-assisted): apply the measured-GPU scaling to the texture byte cap of the
    // tier just selected. Applied to tex_bytes and stage_kb together, because they govern
    // the same uploads -- scaling only one would just move the ceiling.
    //
    // The floor is a floor: `catchup-floor` exists because FIX 38 established that a slow
    // frame must not starve a real backlog, so scaling it down would re-introduce the exact
    // death spiral FIX 38 removed. Slow the *faster* tiers instead; the floor already is the
    // conservative case.
    if (gpu_scale < 1.0 && std::strcmp(mode, "catchup-floor") != 0) {
      want.tex_bytes = (u32)((double)want.tex_bytes * gpu_scale);
      want.stage_kb = (u32)std::max(256.0, (double)want.stage_kb * gpu_scale);
    }
  } else if (m_frame_gap_ema_ms > 45.0) {
    want = {1.f, 128 * 1024, 256};
    mode = "idle-struggle";
  } else if (m_frame_gap_ema_ms > 25.0) {
    want = {2.f, 256 * 1024, 512};
    mode = "idle-lean";
  } else {
    want = {4.f, 1024 * 1024, 1024};
    mode = "idle-healthy";
  }
#ifdef __SWITCH__
  // FIX 76e (AI-assisted): A PURE-PREFETCH STREAM GETS THE LEAN BUDGET.
  //
  // The f76d hardware log (2026-10-03, Geyser Rock) showed the prefetch
  // staging at catchup-pace: 7-8 ms of submit budget on a frame that has
  // ~0-1 ms of slack, so EVERY staging frame ran 36-41 ms for the whole
  // multi-minute dwell (budget lines oscillating catchup-pace 33 -> floor 40
  // -> pace 35 ...). The user reported it as "fps was terrible in Geyser
  // Rock". The game's own loads may take catchup budgets (FIX 38: a visible
  // stream-in may spend frames to finish sooner) - but a prefetch is by
  // definition not urgent. If the ONLY thing in flight is our prefetch, drop
  // to the lean numbers: 2 ms / 256 KB / 1 dispatch, and the update() skip
  // path plus the texture-stage deferral below keep even that from costing
  // the player a frame. Worst case the cache finishes a bit later; there is
  // no user-visible failure mode for a slow prefetch, only for a hitching one.
  if (!m_blackout && !m_in_update_blocking && prefetch_in_flight > 0 &&
      pending == prefetch_in_flight) {
    want = {2.f, 256 * 1024, 512};
    mode = "pf-lean";
  }
#endif
  // FIX 52 (AI-assisted): the `|| want.tex_bytes != ...` clause is load-bearing. This
  // block only publishes `want` to the stages when the mode NAME changes, and the FIX 52
  // GPU scaling changes the byte cap *within* a mode. Without the extra clause a
  // GPU-driven rescale would be computed every frame and then silently discarded -- the
  // exact class of bug where the instrumentation looks healthy and nothing happens.
  if (std::strcmp(mode, m_budget_mode) != 0 || want.tex_bytes != g_loader_budget.tex_bytes) {
    m_budget_mode = mode;
    g_loader_budget = want;
    // FIX 46 (AI-assisted): also report the live-level cap and how many levels
    // are resident/held/wanted. The FIX 46 theory is that the loader used to be
    // capped below what the game holds (jak3 asks for 11, cap was 8), so it
    // evicted a level the game still wanted every frame and re-uploaded it.
    // "live" pinned at the cap together with "hold" above it would prove the
    // churn; after this fix "live" should be able to reach "hold" and stay.
    fmt::print(
        "[loader] budget ms={:.1f} tex_kb={} mode={} (ema {:.1f}ms, pending {}, live {}/{}, "
        "hold {}, active {})\n",
        (double)g_loader_budget.ms, g_loader_budget.tex_bytes / 1024, mode, m_frame_gap_ema_ms,
        pending, (int)m_loaded_tfrag3_levels.size(), max_live_levels(), (int)m_desired_levels.size(),
        (int)m_active_levels.size());
  }

#ifdef __SWITCH__
  // FIX 76d (AI-assisted): PER-FRAME DISPATCH CAP - the crawl killer.
  //
  // The f76c hardware log (2026-10-03) caught the two costs of the texture
  // stage's "at least 4 dispatches" floor:
  //   - Geyser Rock arrival staged the warp's "additional level" village1
  //     during live play at catchup-floor; every frame paid 4+ dispatches of
  //     1-15 ms each (slow setup lines of 10-30 ms), the frame gap EMA sat at
  //     55-67 ms for ~10 s - the user's "10-15 fps everywhere" report - and
  //     the run log recorded 43 hitches >= 100 ms in that window.
  //   - Every quiet prefetch added a visible hitch per staging frame for the
  //     same reason.
  // One dispatch cannot be split (a texture uploads atomically on this
  // driver), so the fix is to bound the COUNT: 1 while staging our own
  // prefetch (it has minutes of dwell time), 2 while a live game load is
  // pushing the EMA past 45 ms (progress continues - the FIX 38 floor keeps
  // its budget - but each frame recovers instead of compounding into a
  // crawl), 20 otherwise. Blackout stays uncapped: the game is frozen
  // behind update_blocking() and wants the load finished, hitches included.
  u32 dispatch_cap = 20;
  if (m_in_update_blocking) {
    dispatch_cap = 64;  // FIX 94: frozen sweep, no frame to protect
  } else if (m_blackout) {
    // frozen game: finish whatever is in flight as fast as it can go.
  } else if (prefetch_in_flight > 0) {
    dispatch_cap = 1;
  } else if (m_frame_gap_ema_ms > 45.0) {
    dispatch_cap = 2;
  }
  g_loader_budget.dispatch_cap = dispatch_cap;
  // FIX 95: clamp the gameplay streaming line to this frame's measured free time.
  if (!m_blackout && !m_in_update_blocking && pending > 0) {
    const double free_ms = switch_free_ms_min();
    if (free_ms >= 0) {
      g_loader_budget.ms = (float)std::clamp(free_ms - kFreeMarginMs, 1.0, (double)want.ms);
    }
    if ((s_free_frame % 60) == 0) {
      fmt::print("[loader] FIX 95 free {:.1f}ms -> stream budget {:.1f}ms (tier {} {:.1f}ms)\n",
                 free_ms, (double)g_loader_budget.ms, mode, (double)want.ms);
    }
  } else {
    g_loader_budget.ms = want.ms;
  }
  // FIX 76e: published every frame (unlike the tier above, which only
  // republishes on a mode change) - the stages must see the CURRENT frame's
  // prefetch-only status, not the one from when the budget last changed.
  g_loader_budget.prefetch_only = (!m_blackout && !m_in_update_blocking &&
                                   prefetch_in_flight > 0 && pending == prefetch_in_flight);
#endif
}

/*!
 * FIX 52 (AI-assisted): MEASURE THE LOADER'S GPU COST, NOT ITS SUBMIT TIME.
 *
 * Why this exists. Every loader budget in this file is keyed off a signal that cannot
 * see the loader's own cost:
 *
 *   m_frame_gap_ema_ms  times the whole frame, so it is dominated by the renderer.
 *   loader_timer        times the GL *calls*, which only enqueue work. glTexSubImage2D
 *                       and glGenerateMipmap return as soon as the command is in the
 *                       driver's queue; the GPU does the work later, during the
 *                       swapchain acquire that OpenGLRenderer.cpp times as `pcrtc`.
 *
 * So these three lines can all appear in the same second, none of them wrong, and none
 * of them the loader's real cost:
 *
 *   [phase] loader 0.01 | pcrtc 22.33      <- loader attributed 0.01 ms
 *   Loader::update slow setup: 20.1ms      <- submit time, 10x its own budget
 *   [loader] budget ... mode=catchup-pace  <- decided from a number it cannot attribute
 *
 * This is exactly why FIX 49's clamp regressed: it throttled a number that measured
 * nothing, so the mip queue grew to 768 while the frame showed no improvement.
 *
 * The fix is to insert a fence after the loader submits its frame's work and wait for
 * it. That wait is a true "how long did my work take" measurement -- the first the
 * loader has ever had.
 *
 * Cost control, because a fence that waits forever would itself be the stall:
 *  - One fence object is reused across frames (m_gpu_fence), never one per frame.
 *  - The wait is bounded by kProbeTimeoutMs. On timeout the result is reported as a
 *    lower bound (negative) rather than blocking the frame; a timeout is itself the
 *    finding, meaning the GPU is deeper behind than the timeout.
 *  - Results are smoothed with the same 1/8 alpha as the frame-gap EMA, so a single
 *    slow frame cannot swing the budget.
 *
 * Render thread only.
 */
double Loader::gpu_cost_probe() {
  // FIX 55 (AI-assisted): the probe is self-gating. It was added to answer one question --
  // "is the loader's GPU cost the 20ms in the frame gap?" -- and the answer was no: 1675 of
  // 1868 readings were 0, and every real reading was under 3.7ms. Keeping it armed costs a
  // glFenceSync + glClientWaitSync every frame, which is the trade FIX 40 rejected when it
  // made diagnostics opt-in.
  //
  // It also outlived its own baseline: `gpu=` prints through fmt::print and so reports
  // unconditionally, but the `[phase]` split it has to be read against is gated on
  // switch_diag_enabled() and is OFF by default -- which is how two sessions of loader work
  // ran without anyone ever seeing the frame split. Tie the probe to the same switch that
  // gates the number it explains, so "no gpu= lines" and "no [phase] lines" mean the same
  // thing and cannot be misread as a finding.
  //
  // This sits above the submits check on purpose: gating has to hold on every path, or a
  // frame with nothing to measure would leave a fence armed and the probe would quietly
  // resume the moment the loader next submitted anything.
  if (!switch_diag_enabled()) {
    if (m_gpu_fence) {
      if (glad_glDeleteSync) {
        glad_glDeleteSync((GLsync)m_gpu_fence);
      }
      m_gpu_fence = nullptr;
    }
    m_loader_gpu_last_ms = 0.0;
    return 0.0;
  }

  // FIX 52: the texture stage counts its submissions in a global (g_loader_gpu_submits_
  // this_frame) so both upload paths are covered without either stage knowing about the
  // probe. Take the count and clear it: it is "since the last probe", not "since boot".
  //
  // Nothing submitted means nothing to attribute, and an idle loader must not pay for a
  // measurement of zero. Decay rather than snap, so a load that just finished does not
  // keep governing the budget for several seconds afterwards.
  if (g_loader_gpu_submits_this_frame == 0) {
    m_loader_gpu_ema_ms *= 0.875;
    m_loader_gpu_last_ms = 0.0;
    return 0.0;
  }
  g_loader_gpu_submits_this_frame = 0;

  // The entry points live in glad's GL 3.2 block, which the Switch loader skips for the
  // "OpenGL ES 3.1" version string -- they are resolved by name in
  // graphics/pipelines/opengl.cpp (FIX 52 there). If resolution failed on some driver,
  // report the absence once rather than jumping to address 0.
  static bool s_reported_missing = false;
  if (!glad_glFenceSync || !glad_glClientWaitSync) {
    if (!s_reported_missing) {
      s_reported_missing = true;
      fmt::print("[loader] gpu-probe UNAVAILABLE: glFenceSync/glClientWaitSync are null\n");
    }
    return 0.0;
  }

  // Wait for the fence from the PREVIOUS frame. It was inserted after that frame's
  // loader submission, so its completion is that frame's loader GPU work retiring.
  double measured = 0.0;
  bool timed_out = false;
  if (m_gpu_fence) {
    constexpr double kProbeTimeoutMs = 8.0;
    const GLuint64 timeout_ns = (GLuint64)(kProbeTimeoutMs * 1e6);
    Timer wait_timer;
    // GL_SYNC_FLUSH_COMMANDS_BIT guarantees the fence is actually reached, so the wait
    // measures the work rather than sitting on a command that was never flushed.
    const GLenum r = glClientWaitSync((GLsync)m_gpu_fence, GL_SYNC_FLUSH_COMMANDS_BIT, timeout_ns);
    measured = wait_timer.getMs();
    timed_out = (r == GL_TIMEOUT_EXPIRED);
    glDeleteSync((GLsync)m_gpu_fence);
    m_gpu_fence = nullptr;
  }

  // Insert this frame's fence now that the previous one has been consumed.
  m_gpu_fence = (void*)glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
  // FIX 54 (AI-assisted): validate before trusting it. A GLsync is an opaque pointer and
  // glFenceSync returns 0 on failure; some ES drivers also hand back a non-null value that
  // glIsSync rejects. Passing a bad object to glClientWaitSync is undefined, so check both
  // and drop the probe -- and the fence -- if it is not a real sync object. Without this,
  // a driver that cannot do fence sync would have the loader waiting on garbage pointers
  // every frame, which is a plausible way to reach the black screen this build produced.
  if (m_gpu_fence && glad_glIsSync && !glad_glIsSync((GLsync)m_gpu_fence)) {
    m_gpu_fence = nullptr;
  }
  if (!m_gpu_fence) {
    if (!s_reported_missing) {
      s_reported_missing = true;
      fmt::print("[loader] gpu-probe UNAVAILABLE: glFenceSync returned an invalid sync\n");
    }
    m_loader_gpu_last_ms = 0.0;
    return 0.0;
  }

  if (m_gpu_fence) {
    m_loader_gpu_ema_ms += (measured - m_loader_gpu_ema_ms) * 0.125;
  }

  // Negative marks a lower bound, so "8.0" and "8.0 and still counting" are
  // distinguishable in the log -- the difference decides whether the budget is helping
  // or merely guessing. See the slow-setup format string.
  m_loader_gpu_last_ms = timed_out ? -measured : measured;
  return measured;
}

/*!
 * FIX 36 Task 3 (AI-assisted): real GPU-buffer-pool pressure. FIX 33 evicted
 * levels on a frame counter while the pool sat on 208 MB of free buffers.
 * The live-level hard cap itself lives in pick_eviction_victim(); this is
 * the "is the recycling pool actually running dry" signal. Render thread only.
 */
bool Loader::loader_under_pressure() {
  // pooled_bytes() is free recycled-buffer bytes (see the [loader] telemetry);
  // below ~16 MB a fresh stage upload would have to extend the arena.
  constexpr size_t kPressureFreeBytes = 16 * 1024 * 1024;
  return m_buffer_pool.pooled_bytes() < kPressureFreeBytes;
}
#endif

/*!
 * The game calls this to tell the loader that we absolutely want these levels active.
 * This will NOT trigger a load!
 */
void Loader::set_active_levels(const std::vector<std::string>& levels) {
  std::unique_lock<std::mutex> lk(m_loader_mutex);
  m_active_levels = levels;
}

/*!
 * Get all levels that are in memory and used very recently.
 */
std::vector<LevelData*> Loader::get_in_use_levels() {
  std::vector<LevelData*> result;
  std::unique_lock<std::mutex> lk(m_loader_mutex);

  for (auto& [name, lev] : m_loaded_tfrag3_levels) {
    if (lev->frames_since_last_used < 5) {
      result.push_back(lev.get());
    }
  }
  return result;
}

void Loader::draw_debug_window() {
  ImGui::Begin("Loader");

  if (!m_selected_level_for_reload.empty() &&
      m_loaded_tfrag3_levels.find(m_selected_level_for_reload) == m_loaded_tfrag3_levels.end()) {
    m_selected_level_for_reload.clear();
  }

  const char* preview = m_selected_level_for_reload.empty() ? "(select a level)"
                                                            : m_selected_level_for_reload.c_str();
  if (ImGui::BeginCombo("Level Select", preview)) {
    for (const auto& [name, data] : m_loaded_tfrag3_levels) {
      bool selected = (name == m_selected_level_for_reload);
      if (ImGui::Selectable(name.c_str(), selected)) {
        m_selected_level_for_reload = name;
      }
      if (selected) {
        ImGui::SetItemDefaultFocus();
      }
    }
    ImGui::EndCombo();
  }

  bool single_reload_pending = !m_single_level_to_reload.empty();
  bool can_reload_selected =
      !m_selected_level_for_reload.empty() && !single_reload_pending && !m_want_reload;
  if (!can_reload_selected) {
    ImGui::BeginDisabled();
  }
  if (ImGui::Button("Force Reload Selected")) {
    m_single_level_to_reload = m_selected_level_for_reload;
  }
  if (!can_reload_selected) {
    ImGui::EndDisabled();
  }
  ImGui::SameLine();
  if (ImGui::Button("Force Reload Common")) {
    m_want_reload_common = true;
  }
  ImGui::SameLine();
  if (ImGui::Button("Force Reload All")) {
    m_want_reload = true;
  }
  ImGui::SameLine();
  if (m_want_reload) {
    ImGui::TextUnformatted("(waiting for full reload...)");
  } else if (single_reload_pending) {
    ImGui::Text("(waiting to reload %s...)", m_single_level_to_reload.c_str());
  }
  ImGui::Separator();

  std::unique_lock<std::mutex> lk(m_loader_mutex);
  ImVec4 blue(0.3, 0.3, 0.8, 1.0);
  ImVec4 red(0.8, 0.3, 0.3, 1.0);
  ImVec4 green(0.3, 0.8, 0.3, 1.0);

  if (!m_desired_levels.empty()) {
    ImGui::Text("desired levels");
    for (auto& lev : m_desired_levels) {
      auto lev_color = red;
      if (m_initializing_tfrag3_levels.find(lev) != m_initializing_tfrag3_levels.end()) {
        lev_color = blue;
      }
      if (m_loaded_tfrag3_levels.find(lev) != m_loaded_tfrag3_levels.end()) {
        lev_color = green;
      }
      ImGui::TextColored(lev_color, "%s", lev.c_str());
      ImGui::SameLine();
    }
    ImGui::NewLine();
    ImGui::Separator();
  }

  if (!m_initializing_tfrag3_levels.empty()) {
    ImGui::Text("init levels");
    for (auto& lev : m_initializing_tfrag3_levels) {
      ImGui::TextColored(blue, "%s", lev.first.c_str());
      ImGui::SameLine();
    }
    ImGui::NewLine();
    ImGui::Separator();
  }

  if (!m_loaded_tfrag3_levels.empty()) {
    ImGui::Text("loaded levels");
    for (auto& lev : m_loaded_tfrag3_levels) {
      auto lev_color = green;
      if (lev.second->frames_since_last_used > 0) {
        lev_color = blue;
      }
      if (lev.second->frames_since_last_used > 180) {
        lev_color = red;
      }
      ImGui::TextColored(lev_color, "%20s : %3d", lev.first.c_str(),
                         lev.second->frames_since_last_used);
      ImGui::Text("  %d textures", (int)lev.second->textures.size());
      ImGui::Text("  %d merc", (int)lev.second->merc_model_lookup.size());
    }
    ImGui::NewLine();
    ImGui::Separator();
  }

  ImGui::End();
}

/*!
 * Loader function that runs in a completely separate thread.
 * This is used for file I/O and unpacking.
 */
void Loader::loader_thread() {
  try {
#ifdef __SWITCH__
    // FIX 70 (AI-assisted): the loader owns core 2 (PERF_PLAN_NEXT_AGENT.md
    // step 1): disk read + decompress + stage prep, off the GOAL core and off
    // the render core.
    switch_platform::switch_pin_current_thread("loader", 2);
#endif
    while (!m_want_shutdown) {
      prof().root_event();
#ifdef __SWITCH__
      // FIX 70: 10 s throttled [cores] self-report. We sleep in the cv wait
      // below, so this only fires around real load work.
      switch_platform::switch_core_diag_periodic("loader");
#endif
      std::unique_lock<std::mutex> lk(m_loader_mutex);

      // this will keep us asleep until we've got a level to load.
      m_loader_cv.wait(lk, [&] { return !m_level_to_load.empty() || m_want_shutdown; });
      if (m_want_shutdown) {
        return;
      }
      std::string lev = m_level_to_load;
      // don't hold the lock while reading the file.
      lk.unlock();

      // simulate slower hard drive (so that the loader thread can lose to the game loads)
      // std::this_thread::sleep_for(std::chrono::milliseconds(1500));

      // load the fr3 file
      prof().begin_event("read-file");
      Timer disk_timer;
      auto data = file_util::read_binary_file(m_base_path / fmt::format("{}.fr3", lev));
      double disk_load_time = disk_timer.getSeconds();
      prof().end_event();

      // the FR3 files are compressed
      prof().begin_event("decompress-file");
      Timer decomp_timer;
      auto decomp_data = compression::decompress_zstd(data.data(), data.size());
      double decomp_time = decomp_timer.getSeconds();
      prof().end_event();

      // Read back into the tfrag3::Level structure
      prof().begin_event("deserialize");
      Timer import_timer;
      auto result = std::make_unique<tfrag3::Level>();
      Serializer ser(decomp_data.data(), decomp_data.size());
      result->serialize(ser);
      double import_time = import_timer.getSeconds();
      prof().end_event();

      // and finally "unpack", which creates the vertex data we'll upload to the GPU

      Timer unpack_timer;
      {
        auto p = scoped_prof("tie-unpack");
        for (auto& tie_tree : result->tie_trees) {
          for (auto& tree : tie_tree) {
            tree.unpack();
          }
        }
      }

      {
        auto p = scoped_prof("tfrag-unpack");
        for (auto& t_tree : result->tfrag_trees) {
          for (auto& tree : t_tree) {
            tree.unpack();
          }
        }
      }

      {
        auto p = scoped_prof("shrub-unpack");
        for (auto& shrub_tree : result->shrub_trees) {
          shrub_tree.unpack();
        }
      }
      decode_level_bcn_to_rgba(*result);  // FIX 91: off the render thread

      fmt::print(
          "------------> Load from file: {:.3f}s, import {:.3f}s, decomp {:.3f}s unpack {:.3f}s\n",
          disk_load_time, import_time, decomp_time, unpack_timer.getSeconds());

      // FIX 48 (AI-assisted): this call is now a deliberate no-op. FIX 47 primed a
      // byte-swapped copy here; the swap turned out to be both wrong and
      // unnecessary, so there is nothing to stage and this loop is kept only so
      // the call site is obvious if staging is ever needed again.
      {
        auto p = scoped_prof("prime-texture-swap");
        Timer prime_timer;
        for (const auto& tex : result->textures) {
          prime_texture_swap(tex);
        }
        if (prime_timer.getMs() > 1.f) {
          fmt::print("[loader] FIX 48 primed {} texture swaps in {:.1f}ms (no-op)\n",
                     result->textures.size(), prime_timer.getMs());
        }
      }

      // grab the lock again
      lk.lock();
#ifdef __SWITCH__
      // FIX 76 (AI-assisted): this prefetch was cancelled while we were
      // reading the file (the game asked for a different level). Drop the
      // data without publishing and without touching m_level_to_load - the
      // game's real request may already be queued in that slot - then go
      // back to waiting.
      if (!m_prefetch_discard.empty() && m_prefetch_discard == lev) {
        m_prefetch_discard.clear();
        fmt::print("[loader] prefetch: abandoned {} after file read\n", lev);
        continue;
      }
#endif
      // move this level to "initializing" state.
      m_initializing_tfrag3_levels[lev] = std::make_unique<LevelData>();  // reset load state
      m_initializing_tfrag3_levels[lev]->level = std::move(result);
      m_level_to_load = "";
      m_file_load_done_cv.notify_all();
    }
  } catch (std::exception& e) {
    ASSERT_MSG(false, fmt::format("Exception {} encountered in loader_thread", e.what()));
  }
}

/*!
 * Load a "common" FR3 file that has non-level textures.
 * This should be called during initialization, before any threaded loading goes on.
 */
const tfrag3::Level& Loader::load_common(TexturePool& tex_pool, const std::string& name) {
  auto data = file_util::read_binary_file(m_base_path / fmt::format("{}.fr3", name));

  auto decomp_data = compression::decompress_zstd(data.data(), data.size());
  Serializer ser(decomp_data.data(), decomp_data.size());
  m_common_level.level = std::make_unique<tfrag3::Level>();
  m_common_level.level->serialize(ser);
  decode_level_bcn_to_rgba(*m_common_level.level);  // FIX 91
  for (auto& tex : m_common_level.level->textures) {
    m_common_level.textures.push_back(add_texture(tex_pool, tex, true));
  }

  install_buffer_reclaim(tex_pool);
  Timer tim;
  MercLoaderStage mls;
  LoaderInput input;
  input.tex_pool = &tex_pool;
  input.mercs = &m_all_merc_models;
  input.lev_data = &m_common_level;
  input.buffers = &m_buffer_pool;
  bool done = false;
  while (!done) {
    done = mls.run(tim, input);
  }
  return *m_common_level.level;
}

bool Loader::upload_textures(Timer& timer, LevelData& data, TexturePool& texture_pool) {
  // try to move level from initializing to initialized:

  auto evt = scoped_prof("upload-textures");
  // FIX 66 (AI-assisted): this path has no callers (the TextureLoaderStage in
  // LoaderStages.cpp is the live spender), but it still hardcoded a 128 KB
  // per-run byte cap that ignored g_loader_budget entirely - a landmine for
  // anyone who wires it back up. Both caps now mirror the stage exactly:
  // byte cap = g_loader_budget.tex_bytes, dispatch count scaled from it.
  const u32 max_tex_bytes_this_run = g_loader_budget.tex_bytes;

  int bytes_this_run = 0;
  int tex_this_run = 0;
  if (data.textures.size() < data.level->textures.size()) {
    std::unique_lock<std::mutex> tpool_lock(texture_pool.mutex());
    while (data.textures.size() < data.level->textures.size()) {
      auto& tex = data.level->textures[data.textures.size()];
      data.textures.push_back(add_texture(texture_pool, tex, false));
      bytes_this_run += tex.w * tex.h * 4;
      tex_this_run++;
      // FIX 46c (AI-assisted): same per-dispatch cap reasoning as the stage in
      // LoaderStages.cpp -- 20 atomic texture uploads is ~18 ms of the frame, so
      // only take that many when the budget says the frame has room.
      const int max_tex_this_dispatch =
          std::clamp<int>(g_loader_budget.tex_bytes / (64 * 1024), 4, 20);
      if (tex_this_run >= max_tex_this_dispatch) {
        break;
      }
      if ((u32)bytes_this_run > max_tex_bytes_this_run ||
          timer.getMs() > SHARED_TEXTURE_LOAD_BUDGET) {
        break;
      }
    }
  }
  return data.textures.size() == data.level->textures.size();
}

void Loader::update_blocking(TexturePool& tex_pool) {
  fmt::print("NOTE: coming out of blackout on next frame, doing all loads now...\n");
#ifdef __SWITCH__
  // FIX 71 (AI-assisted): this synchronous sweep is the actual multi-second
  // freeze, and it runs on the first NON-black frame -- m_blackout has already
  // flipped false, so update_frame_budget would drop the boost clocks right
  // before the heaviest load. Keep them through the sweep, drop them at the
  // end (the "Blackout loads done" point from PERF_PLAN step 2).
  switch_platform::switch_set_cpu_boost(true);
  // FIX 76e: m_blackout is already false here, but this sweep is still a frozen
  // load - keep the prefetch lean/pause/deferral throttles out of it.
  m_in_update_blocking = true;
#endif
  install_buffer_reclaim(tex_pool);

#ifdef __SWITCH__
  // FIX 33 (AI-assisted): free everything the game no longer holds BEFORE
  // staging the new area. The screen has been black, so recycling now is
  // invisible, and peak GPU memory becomes "new area" instead of
  // "old area + new area" (the combination that killed nouveau_mm during
  // zoomer area transitions).
  purge_retired_levels(tex_pool, true);
#endif

  bool missing_levels = true;
  while (missing_levels) {
    bool needs_run = true;

    while (needs_run) {
      needs_run = false;
      {
        std::unique_lock<std::mutex> lk(m_loader_mutex);
        if (!m_level_to_load.empty()) {
          m_file_load_done_cv.wait(lk, [&]() { return m_level_to_load.empty(); });
        }
      }
    }

    needs_run = true;

    while (needs_run) {
      needs_run = false;
      {
        std::unique_lock<std::mutex> lk(m_loader_mutex);
        if (!m_initializing_tfrag3_levels.empty()) {
          needs_run = true;
        }
      }

      if (needs_run) {
        update(tex_pool);
      }
    }

    {
      std::unique_lock<std::mutex> lk(m_loader_mutex);
      missing_levels = false;
      for (auto& des : m_desired_levels) {
        if (m_loaded_tfrag3_levels.find(des) == m_loaded_tfrag3_levels.end()) {
          fmt::print("blackout loader doing additional level {}...\n", des);
          missing_levels = true;
        }
      }
    }

    if (missing_levels) {
      set_want_levels(m_desired_levels);
    }
  }

  fmt::print("Blackout loads done. Current status:");
  std::unique_lock<std::mutex> lk(m_loader_mutex);
  for (auto& ld : m_loaded_tfrag3_levels) {
    fmt::print("  {} is loaded.\n", ld.first);
  }
#ifdef __SWITCH__
  // FIX 71 (AI-assisted): load finished -- back to the normal clock
  // configuration before the fade-in and gameplay.
  switch_platform::switch_set_cpu_boost(false);
  m_in_update_blocking = false;
#endif
}

/*!
 * Choose a level to evict, or nullptr if none is eligible.
 * FIX 33 (AI-assisted): on Switch the game tells us every frame which levels
 * it holds (__pc-set-levels -> m_desired_levels) and which it is actually
 * displaying (__pc-set-active-levels -> m_active_levels; see
 * goal_src/jak2/engine/level/level.gc). FIX 36: retired levels stay resident
 * for several seconds and are only recycled under real memory pressure, plus
 * a live-level cap keeps area transitions from piling up. Render thread only.
 */
const std::string* Loader::pick_eviction_victim() {
  std::unique_lock<std::mutex> lk(m_loader_mutex);
#ifdef __SWITCH__
  // FIX 36 Task 3 (AI-assisted): stop throwing away levels that are about to
  // be needed again. FIX 33 retired anything 30 frames off the want-list
  // while the buffer pool sat on 208 MB of free buffers - a city re-entry
  // then paid a full re-upload (tens of seconds at the old per-frame caps).
  constexpr int kRetiredAge = 300;  // frames off the game's want-list (~5-10 s)

  // -------------------------------------------------------------------------
  // FIX 46 (AI-assisted): THE CAP WAS BELOW WHAT THE GAME LEGITIMATELY HOLDS.
  //
  // This was a flat 8. But the game can ask for more levels than that at once:
  //   jak1 LEVEL_TOTAL 3, jak2 LEVEL_TOTAL 7, jak3 LEVEL_TOTAL 11, jakx 11
  // (common/goal_constants.h; Loader is constructed with that value as
  // m_max_levels). So in jak3 the loader was structurally forbidden from
  // holding what GOAL kept resident, and `at_cap` alone -- with NO age test --
  // was enough to evict. Every frame the game asked for its 11 levels, the
  // loader held 8, evicted the oldest, the game re-requested it on the next
  // frame, and it was re-read, re-decompressed, re-unpacked and re-uploaded
  // from scratch. That is the permanent "huge slowdown" on city entry and the
  // reason an area could appear never to finish loading: the work was being
  // thrown away and redone, not merely delayed. A city holds the most levels,
  // which is why the cities were worst.
  //
  // The cap now derives from what the game can actually request, plus a little
  // slack for the level being staged. And `at_cap` no longer evicts on its
  // own: a level the game still holds (m_desired_levels) is skipped above, so
  // the only remaining cap victims are ones the game has genuinely dropped --
  // which the age test already gates.
  // -------------------------------------------------------------------------
  const bool at_cap = (int)m_loaded_tfrag3_levels.size() >= max_live_levels();
  const bool low_mem = loader_under_pressure();
  const std::string* best = nullptr;
  int best_age = -1;
  for (auto& [name, lev] : m_loaded_tfrag3_levels) {
    if (std::find(m_active_levels.begin(), m_active_levels.end(), name) !=
        m_active_levels.end()) {
      continue;  // currently displayed - never recycle
    }
    if (std::find(m_desired_levels.begin(), m_desired_levels.end(), name) !=
        m_desired_levels.end()) {
      continue;  // the game still holds this level
    }
#ifdef __SWITCH__
    // FIX 76c (AI-assisted): a prefetched next-area cache is never drawn -
    // that is the whole point - so its frames_since_last_used outruns every
    // real level while staging, and low_mem is effectively always true on
    // Switch (the pool reports 0 MB free in steady state, all buffers are
    // out in use). The f76 hardware log shows the result: "prefetch: beach
    // cached" then "PC unloading beach" two seconds later, and the crossing
    // paid a full 18.14s visible reload. Caches are skipped here and only
    // reclaimed (retired, at the unload site) under the fallback below.
    if (m_prefetch_resident.count(name) > 0) {
      continue;
    }
#endif
    const int age = lev->frames_since_last_used;
    // FIX 46: over the cap we may reclaim sooner than kRetiredAge, but never a
    // level younger than kMinReclaimAge -- otherwise a burst of requests could
    // evict something the game is about to want back (the churn above).
    constexpr int kMinReclaimAge = 60;  // ~1-2 s
    if (((low_mem && age >= kRetiredAge) || (at_cap && age >= kMinReclaimAge)) &&
        age > best_age) {
      best_age = age;
      best = &name;
    }
  }
#ifdef __SWITCH__
  if (best == nullptr && m_buffer_pool.failed_allocations() > 0) {
    // FIX 76c: real pressure (an allocation actually failed) and no game
    // level left to recycle - the hidden caches are the remaining luxury.
    // The unload site retires them so this cannot thrash. Note: gated on
    // failed_allocations(), NOT low_mem - pooled_bytes()<16MB is the normal
    // steady state (nothing recycled recently), and gating on it would eat
    // every cache the moment no stale game level is left to evict.
    // (at_cap alone never eats a cache: the prefetch start guard already
    // keeps live levels, caches included, well under the cap.)
    for (auto& name : m_prefetch_resident) {
      if (m_loaded_tfrag3_levels.find(name) != m_loaded_tfrag3_levels.end() &&
          std::find(m_active_levels.begin(), m_active_levels.end(), name) ==
              m_active_levels.end() &&
          std::find(m_desired_levels.begin(), m_desired_levels.end(), name) ==
              m_desired_levels.end()) {
        return &m_loaded_tfrag3_levels.find(name)->first;
      }
    }
  }
#endif
  return best;
#else
  // Desktop: legacy behavior - only unload once we're over m_max_levels, and
  // only levels unused for 180 frames, preferring ones no longer desired.
  if ((int)m_loaded_tfrag3_levels.size() < m_max_levels) {
    return nullptr;
  }
  for (auto& [name, lev] : m_loaded_tfrag3_levels) {
    if (lev->frames_since_last_used > 180 &&
        std::find(m_desired_levels.begin(), m_desired_levels.end(), name) ==
            m_desired_levels.end()) {
      return &name;
    }
  }

  for (const auto& [name, lev] : m_loaded_tfrag3_levels) {
    if (lev->frames_since_last_used > 180) {
      return &name;
    }
  }
  return nullptr;
#endif
}

/*!
 * Tear down every GPU object owned by a level: removes pool textures from the
 * TexturePool, queues GL textures for paced deletion, and returns all GL
 * buffers to the GpuBufferPool (FIX 33: recycling instead of delete/allocate
 * churn). Also drops the level's merc model references. Render thread only;
 * the caller is responsible for removing the LevelData itself.
 */
void Loader::unload_level_gpu_objects(LevelData& lev, TexturePool& tex_pool) {
  {
    std::unique_lock<std::mutex> lk(tex_pool.mutex());
    for (size_t i = 0; i < lev.textures.size() && i < lev.level->textures.size(); i++) {
      const auto& tex = lev.level->textures[i];
      if (tex.load_to_pool) {
        tex_pool.unload_texture(PcTextureId::from_combo_id(tex.combo_id), lev.textures[i]);
      }
    }
  }

  for (auto tex : lev.textures) {
    if (EXTRA_TEX_DEBUG) {
      for (auto& slot : tex_pool.all_textures()) {
        if (slot.source) {
          ASSERT(slot.gpu_texture != tex);
        } else {
          ASSERT(slot.gpu_texture != tex);
        }
      }
    }
    m_garbage_textures.push_back(tex);
  }
#if GOAL_DEFER_MIPMAPS
  mipq_forget(lev.textures);  // FIX 100
#endif

  // FIX 33: buffers go back to the pool instead of being deleted. This also
  // fixes the old normal-eviction path, which never released shrub buffers
  // or hfrag vertices (and queued hfrag indices twice - a double delete).
  for (auto& tie_geo : lev.tie_data) {
    for (auto& tie_tree : tie_geo) {
      m_buffer_pool.release(tie_tree.vertex_buffer);
      if (tie_tree.has_wind) {
        m_buffer_pool.release(tie_tree.wind_indices);
      }
      m_buffer_pool.release(tie_tree.index_buffer);
    }
  }
  for (auto& tfrag_geo : lev.tfrag_vertex_data) {
    for (auto& buf : tfrag_geo) {
      m_buffer_pool.release(buf);
    }
  }
  for (auto& buf : lev.shrub_vertex_data) {
    m_buffer_pool.release(buf);
  }
  m_buffer_pool.release(lev.hfrag_indices);
  m_buffer_pool.release(lev.hfrag_vertices);
  m_buffer_pool.release(lev.collide_vertices);
  m_buffer_pool.release(lev.merc_vertices);
  m_buffer_pool.release(lev.merc_indices);

  for (auto& model : lev.level->merc_data.models) {
    auto it = m_all_merc_models.find(model.name);
    if (it == m_all_merc_models.end()) {
      continue;
    }
    MercRef ref{&model, lev.load_id};
    auto ref_it = std::ranges::find(it->second, ref);
    if (ref_it != it->second.end()) {
      it->second.erase(ref_it);
    }
  }
}

/*!
 * Delete every queued garbage texture right now. Used by the blackout purge,
 * where we want the memory back before the next area stages (FIX 33).
 */
void Loader::flush_texture_garbage() {
  for (auto tex : m_garbage_textures) {
    glDeleteTextures(1, &tex);
  }
  m_garbage_textures.clear();
}

/*!
 * Give one chunk of GPU memory back to the driver, cheapest source first.
 * Called by GpuBufferPool when a level-loader allocation fails, and retried
 * until this returns false. Returning memory here is what lets an area
 * transition that ran the GPU heap dry finish loading instead of handing the
 * loader a buffer with no storage (which the driver then maps to NULL).
 */
bool Loader::reclaim_gpu_memory(TexturePool& tex_pool) {
  if (!m_garbage_buffers.empty()) {
    for (auto buf : m_garbage_buffers) {
      glDeleteBuffers(1, &buf);
    }
    m_garbage_buffers.clear();
    return true;
  }
  if (!m_garbage_textures.empty()) {
    flush_texture_garbage();
    return true;
  }
  // FIX 63 (AI-assisted): then recycle the oldest level the game no longer holds
  // (not displayed, not on the want-list), regardless of its age. The jak2 crash
  // at ~650 s (ctyindb loading after atoll) failed 2 allocations while retired
  // levels still sat on GPU memory, because reclaim stopped at the garbage queues.
  // Stages run without m_loader_mutex held, so taking it here is safe.
  std::unique_ptr<LevelData> victim;
  std::string victim_name;
  {
    std::unique_lock<std::mutex> lk(m_loader_mutex);
    int best_age = -1;
    for (auto& [name, lev] : m_loaded_tfrag3_levels) {
      if (std::find(m_active_levels.begin(), m_active_levels.end(), name) !=
              m_active_levels.end() ||
          std::find(m_desired_levels.begin(), m_desired_levels.end(), name) !=
              m_desired_levels.end()) {
        continue;
      }
      if (lev->frames_since_last_used > best_age) {
        best_age = lev->frames_since_last_used;
        victim_name = name;
      }
    }
    if (best_age >= 0) {
      auto it = m_loaded_tfrag3_levels.find(victim_name);
      victim = std::move(it->second);
      m_loaded_tfrag3_levels.erase(it);
#ifdef __SWITCH__
      // FIX 76: an evicted prefetch is retired for the session - re-fetching
      // it would just fight whatever pressure evicted it.
      if (m_prefetch_resident.erase(victim_name) > 0) {
        m_prefetch_retired.insert(victim_name);
      }
#endif
    }
  }
  if (victim) {
    fmt::print("[loader] reclaim: evicting retired level {} for GPU memory\n", victim_name);
    unload_level_gpu_objects(*victim, tex_pool);
    // The released buffers are pooled; return them to the driver now so the
    // retried allocation can use the space.
    m_buffer_pool.clear();
    flush_texture_garbage();
    return true;
  }
  return false;
}

void Loader::install_buffer_reclaim(TexturePool& tex_pool) {
  if (m_buffer_reclaim_installed) {
    return;
  }
  m_buffer_reclaim_installed = true;
  m_buffer_pool.set_reclaim_callback([this, &tex_pool]() { return reclaim_gpu_memory(tex_pool); });
}

/*!
 * Recycle every level the game no longer holds, i.e. not in the want-list
 * (__pc-set-levels) and not displayed (__pc-set-active-levels). Called at
 * the end of a blackout (update_blocking) so the new area is staged into
 * freed space instead of on top of the old one. With `immediate`, also
 * flushes the garbage queues and glFinish()es so the driver has actually
 * reclaimed the memory before the new allocations start. (FIX 33)
 */
void Loader::purge_retired_levels(TexturePool& tex_pool, bool immediate) {
  std::vector<std::string> victims;
  {
    std::unique_lock<std::mutex> lk(m_loader_mutex);
    for (auto& [name, lev] : m_loaded_tfrag3_levels) {
      const bool active = std::find(m_active_levels.begin(), m_active_levels.end(), name) !=
                          m_active_levels.end();
      const bool desired = std::find(m_desired_levels.begin(), m_desired_levels.end(), name) !=
                           m_desired_levels.end();
      if (!active && !desired) {
#ifdef __SWITCH__
        // FIX 76d (AI-assisted): NEVER purge a prefetch cache here.
        //
        // The f76c log showed the feature dying at exactly this line: every
        // warp exit ran `blackout purge: recycling 3 retired level(s)`, which
        // ate the beach+jungle caches built up over minutes of dwell (caches
        // are never in m_desired_levels, so they always look "retired") AND
        // session-retired them. Result: beach reloaded from scratch in 22.47 s
        // and the forest was never prefetched again - "later it broke your
        // fix". The caches are 2 levels, bounded; if the blocking load
        // genuinely needs their memory, the FIX 63 reclaim path still frees
        // them on a real allocation failure (and retires them - that one is
        // pressure, this one is routine).
        if (m_prefetch_resident.count(name) > 0) {
          continue;
        }
#endif
        victims.push_back(name);
      }
    }
  }
  if (victims.empty()) {
    return;
  }
  fmt::print("[loader] blackout purge: recycling {} retired level(s)\n", victims.size());
  for (const auto& name : victims) {
    std::unique_ptr<LevelData> lev;
    {
      std::unique_lock<std::mutex> lk(m_loader_mutex);
      auto it = m_loaded_tfrag3_levels.find(name);
      if (it == m_loaded_tfrag3_levels.end()) {
        continue;
      }
      lev = std::move(it->second);
      m_loaded_tfrag3_levels.erase(it);
#ifdef __SWITCH__
      // FIX 76: purged prefetch caches are session-retired (frees their cache
      // slot and stops them being re-fetched against the blackout's needs).
      if (m_prefetch_resident.erase(name) > 0) {
        m_prefetch_retired.insert(name);
      }
#endif
    }
    fmt::print("[loader]   purging {}\n", name);
    unload_level_gpu_objects(*lev, tex_pool);
  }
  if (immediate) {
    flush_texture_garbage();
    for (auto buf : m_garbage_buffers) {
      glDeleteBuffers(1, &buf);
    }
    m_garbage_buffers.clear();
    glFinish();
  }
}

void Loader::update(TexturePool& texture_pool) {
  Timer loader_timer;
  install_buffer_reclaim(texture_pool);

#ifdef __SWITCH__
  // FIX 36 Task 3 (AI-assisted): retune the loader budget from the measured
  // frame gap and the blackout flag (set by OpenGLRenderer). Must run before
  // the stages consume g_loader_budget below.
  update_frame_budget();
  // FIX 33 (AI-assisted): periodic loader pressure telemetry, so we can see
  // live levels / pooled buffer usage from gk_stdout.txt on the console.
  if (++m_stats_frame_count >= 120) {
    m_stats_frame_count = 0;
    size_t live, init, want;
    std::string pf;
    {
      std::unique_lock<std::mutex> lk(m_loader_mutex);
      live = m_loaded_tfrag3_levels.size();
      init = m_initializing_tfrag3_levels.size();
      want = m_desired_levels.size();
      // FIX 76: prefetch state in the telemetry line, so hardware logs show
      // the area cache at work (loading / cached N / discard in progress).
      if (!m_prefetch_target.empty()) {
        pf = "loading " + m_prefetch_target;
      } else if (!m_prefetch_discard.empty()) {
        pf = "discarding " + m_prefetch_discard;
      } else {
        pf = fmt::format("{} cached", m_prefetch_resident.size());
      }
    }
    fmt::print(
        "[loader] live={} init={} want={} | pool={} bufs {:.1f}MB free, {} "
        "out, {} failed | gc {} tex {} buf | budget {} (ema {:.1f}ms) | pf {}\n",
        live, init, want, m_buffer_pool.pooled_buffers(),
        (double)m_buffer_pool.pooled_bytes() / (1024.0 * 1024.0),
        m_buffer_pool.outstanding_buffers(), m_buffer_pool.failed_allocations(),
        m_garbage_textures.size(), m_garbage_buffers.size(), m_budget_mode, m_frame_gap_ema_ms,
        pf);
  }
#endif

  if (m_want_reload) {
    std::unique_lock lk(m_loader_mutex);
    if (m_level_to_load.empty() && m_initializing_tfrag3_levels.empty()) {
      m_want_reload = false;
      lk.unlock();
      do_reload(texture_pool);
      return;
    }
  }

  if (!m_single_level_to_reload.empty()) {
    std::unique_lock lk(m_loader_mutex);
    bool in_init = m_initializing_tfrag3_levels.count(m_single_level_to_reload) > 0;
    if (m_level_to_load.empty() && !in_init) {
      std::string name = std::move(m_single_level_to_reload);
      m_single_level_to_reload.clear();
      lk.unlock();
      do_reload_level(name, texture_pool);
      return;
    }
  }

  if (m_want_reload_common) {
    std::unique_lock lk(m_loader_mutex);
    bool in_init = m_initializing_tfrag3_levels.count(m_single_level_to_reload) > 0;
    if (m_level_to_load.empty() && !in_init) {
      m_want_reload_common = false;
      lk.unlock();
      do_reload_common(texture_pool);
      return;
    }
  }

  {
    // lock because we're accessing m_active_levels
    std::unique_lock<std::mutex> lk(m_loader_mutex);
    // only main thread can touch this.
    for (auto& [name, lev] : m_loaded_tfrag3_levels) {
      // FIX 46 (AI-assisted): reset the age counter for a level the game STILL
      // HOLDS (m_desired_levels, i.e. __pc-set-levels), not only for the one
      // currently displayed.
      //
      // A city keeps its sub-levels resident in GOAL while only some of them
      // are being drawn. Those held-but-undisplayed levels kept aging here and
      // became the preferred eviction victims in pick_eviction_victim(), whose
      // "the game still holds this level" guard is only reached for levels with
      // a *higher* age. Then the game wanted one back immediately. That is half
      // of the evict/re-request/re-upload churn FIX 46 removes.
      const bool held = std::find(m_active_levels.begin(), m_active_levels.end(), name) !=
                            m_active_levels.end() ||
                        std::find(m_desired_levels.begin(), m_desired_levels.end(), name) !=
                            m_desired_levels.end();
      if (!held) {
        lev->frames_since_last_used++;
      } else {
        lev->frames_since_last_used = 0;
      }
    }
  }

  // work on moving initializing to initialized.
  {
    // accessing initializing, should lock
    std::unique_lock<std::mutex> lk(m_loader_mutex);
    // grab the first initializing level:
    const auto& it = m_initializing_tfrag3_levels.begin();
    if (it != m_initializing_tfrag3_levels.end()) {
      std::string name = it->first;
#ifdef __SWITCH__
      // FIX 76 (AI-assisted): a cancelled prefetch (the game asked for a
      // different level while we were staging this one). Drop it now through
      // the same proven unload path FIX 63 uses, so the real request can be
      // queued on the very next set_want_levels call. Skipping the eviction /
      // garbage pass below for one frame is harmless - they run every frame.
      if (name == m_prefetch_discard) {
        std::unique_ptr<LevelData> cancelled = std::move(it->second);
        m_initializing_tfrag3_levels.erase(it);
        m_prefetch_discard.clear();
        lk.unlock();
        fmt::print("[loader] prefetch: dropped {} mid-staging (game wants another level)\n", name);
        unload_level_gpu_objects(*cancelled, texture_pool);
        for (auto& stage : m_loader_stages) {
          stage->reset();
        }
        return;
      }
#endif
      auto& lev = it->second;
      if (it->second->load_id == UINT64_MAX) {
        it->second->load_id = m_id++;
      }
      if (it->second->alloc_failures_at_start < 0) {
        it->second->alloc_failures_at_start = m_buffer_pool.failed_allocations();
      }

      // we're the only place that erases, so it's okay to unlock and hold a reference
      lk.unlock();
      bool done = true;
      LoaderInput loader_input;
      loader_input.lev_data = lev.get();
      loader_input.mercs = &m_all_merc_models;
      loader_input.tex_pool = &texture_pool;
      loader_input.buffers = &m_buffer_pool;

      // FIX 76d (AI-assisted): a pure-prefetch staging must never cost the
      // player a frame. When the frame is already missing 30 fps (ema above
      // ~34.5 ms), don't dispatch anything for the prefetch this frame - the
      // stages are fully resumable and the dwell window is minutes, so simply
      // waiting for a calm frame is always available. Real loads keep the FIX
      // 38 floor (they're capped to 2 dispatches by update_frame_budget when
      // the crawl threshold is passed instead); a prefetch HIT also stops
      // matching m_prefetch_target the moment the game adopts the level.
      //
      // FIX 76e (AI-assisted): ...and the gate alone was not enough. At a
      // locked 30 fps the gap EMA sits at 33.3 even in perfect health, so
      // the 34.5 gate only trips AFTER a frame has already been missed -
      // then decays back under it in ~5 frames and the next dispatch misses
      // again. The f76d log showed exactly that sawtooth for the whole
      // Geyser Rock dwell (ema 33 -> 41 -> 33, budget mode pace/floor
      // alternating). So: an actual missed frame (raw gap > 40 ms) now arms
      // a PAUSE measured in seconds, not frames - 1 s, doubling per repeat
      // miss up to 4 s - and the pause only decays back to 1 s after 2 s of
      // clean staged frames. Worst case a prefetch costs one hitch per pause
      // window instead of one per ~6 frames.
      bool stage_this_frame = true;
#ifdef __SWITCH__
      if (name == m_prefetch_target && !m_blackout && !m_in_update_blocking) {
        if (m_prefetch_pause_frames > 0) {
          m_prefetch_pause_frames--;
          stage_this_frame = false;
        } else if (m_frame_gap_ema_ms > 34.5f) {
          stage_this_frame = false;
          if (m_last_frame_gap_ms > 40.0) {
            m_prefetch_pause_frames = m_prefetch_pause_next;
            m_prefetch_pause_next = std::min(m_prefetch_pause_next * 2, 240);
            m_prefetch_clean_streak = 0;
          }
        } else {
          m_prefetch_clean_streak++;
          if (m_prefetch_clean_streak >= 60) {
            m_prefetch_pause_next = 30;
          }
        }
      }
#endif
      if (stage_this_frame) {
        for (auto& stage : m_loader_stages) {
          auto evt = scoped_prof(fmt::format("stage-{}", stage->name()).c_str());
          Timer stage_timer;
          done = stage->run(loader_timer, loader_input);
          if (stage_timer.getMs() > 5.f) {
            fmt::print("stage {} took {:.2f} ms\n", stage->name(), stage_timer.getMs());
          }
          if (!done) {
            break;
          }
#ifdef __SWITCH__
          // FIX 99 (AI-assisted): don't START the next stage on a gameplay
          // frame whose budget is already spent - every stage does at least
          // one chunk, so chaining 2-3 of them stacked into 10+ ms frames.
          // Stages are resumable (finished ones return immediately), so the
          // remainder simply continues next frame.
          if (!m_blackout && !m_in_update_blocking && stage_timer.getMs() > 0.2f &&
              loader_timer.getMs() > g_loader_budget.ms && &stage != &m_loader_stages.back()) {
            done = false;
            break;
          }
#endif
        }
      } else {
        // not finished - just paused until the game's frames recover.
        done = false;
      }

      if (done && m_buffer_pool.failed_allocations() > lev->alloc_failures_at_start) {
        // FIX 63 (AI-assisted): some acquire() returned 0 while staging this level.
        // Publishing it would let Tie3/Tfrag/Merc draw with buffer 0, which Mesa treats
        // as client-side arrays (vbo_get_minmax_indices -> nouveau_bo_del abort: the
        // jak2 Tie3::draw_matching_draws_for_tree crash). Drop it instead; the game is
        // still asking for it, so set_want_levels() re-requests a fresh load.
        fmt::print("[loader] level {} hit {} GPU alloc failure(s); discarding, will retry\n",
                   name, m_buffer_pool.failed_allocations() - lev->alloc_failures_at_start);
        std::unique_ptr<LevelData> failed;
        lk.lock();
        failed = std::move(it->second);
        m_initializing_tfrag3_levels.erase(it);
#ifdef __SWITCH__
        // FIX 76: a prefetch that ran out of memory is done for this session.
        if (name == m_prefetch_target) {
          m_prefetch_target.clear();
          m_prefetch_retired.insert(name);
        }
#endif
        lk.unlock();
        unload_level_gpu_objects(*failed, texture_pool);
        for (auto& stage : m_loader_stages) {
          stage->reset();
        }
      } else if (done) {
        auto evt = scoped_prof("finish-stages");
        lk.lock();
        m_loaded_tfrag3_levels[name] = std::move(lev);
        m_initializing_tfrag3_levels.erase(it);
#ifdef __SWITCH__
        // FIX 76 (AI-assisted): prefetch completion. The level now sits in
        // the cache where the game will find it already-resident the moment
        // the player walks there - an instant, free transition.
        if (name == m_prefetch_target) {
          m_prefetch_target.clear();
          m_prefetch_resident.insert(name);
          fmt::print("[loader] prefetch: {} cached (budget {})\n", name, m_budget_mode);
        }
        if (name == m_prefetch_discard) {
          // cancelled, but it managed to finish anyway - keep it, it's free.
          m_prefetch_discard.clear();
        }
#endif
#ifdef __SWITCH__
        // FIX 36 Task 3 (AI-assisted): load-completion timing, so cold entry
        // vs re-entry can be compared from the log (brief §4.4). The clock
        // starts in set_want_levels(), under this same mutex.
        if (auto st = m_load_start.find(name); st != m_load_start.end()) {
          const double secs =
              std::chrono::duration<double>(std::chrono::steady_clock::now() - st->second).count();
          m_load_start.erase(st);
          fmt::print("[loader] level {} ready in {:.2f}s (budget {})\n", name, secs, m_budget_mode);
        }
#endif

        for (auto& stage : m_loader_stages) {
          stage->reset();
        }
      }
    }
  }

  // ---- FIX 33 (AI-assisted): level recycling + garbage management ----
  // The old code only unloaded levels when the loader was otherwise idle and
  // only after 180 unused frames. During area transitions (blackout loads)
  // the loader is never idle, so every level ever visited stayed resident:
  // peak GPU memory became "old area + new area" and the Tegra suballocator
  // (nouveau_mm) aborted during a large glBufferData. Now eviction runs
  // every frame, at most one level at a time, following the game's own
  // hints - see pick_eviction_victim().
  {
    auto evt = scoped_prof("gpu-unload");
    Timer unload_timer;
    const std::string* to_unload = pick_eviction_victim();
    if (to_unload) {
      std::string victim_name = *to_unload;
      std::unique_ptr<LevelData> lev;
      {
        std::unique_lock<std::mutex> lk(m_loader_mutex);
        auto it = m_loaded_tfrag3_levels.find(victim_name);
        if (it != m_loaded_tfrag3_levels.end()) {
          lev = std::move(it->second);
          m_loaded_tfrag3_levels.erase(it);
#ifdef __SWITCH__
          // FIX 76: an evicted prefetch is retired for the session.
          if (m_prefetch_resident.erase(victim_name) > 0) {
            m_prefetch_retired.insert(victim_name);
          }
#endif
        }
      }
      if (lev) {
        fmt::print("------------------------- PC unloading {}\n", victim_name);
#ifdef __SWITCH__
        // FIX 100 (AI-assisted): f99 jak3 crash (Wasteland, 422 s) came seconds
        // after `PC unloading warpcast` was followed at once by `[pf] start:
        // background-caching warpcast` - the prefetcher re-streaming the level
        // the evictor had just thrown away (380+ textures of upload + garbage
        // churn, for nothing). Keep a just-evicted level off the prefetch list
        // for 90 s; the game can still request it normally at any time.
        {
          std::unique_lock<std::mutex> lk(m_loader_mutex);
          m_prefetch_cooldown[victim_name] =
              std::chrono::steady_clock::now() + std::chrono::seconds(90);
        }
#endif
        unload_level_gpu_objects(*lev, texture_pool);
      }
    }
    if (unload_timer.getMs() > 5.f) {
      fmt::print("Unload took {:.2f}ms\n", unload_timer.getMs());
    }
  }

#if GOAL_DEFER_MIPMAPS
  // FIX 42 (AI-assisted): pay off the deferred mip chains out of slack. While anything is
  // still streaming the frame already belongs to the loader, so only a token amount is done
  // then; once the backlog clears we catch up quickly. That is the whole point of deferring
  // them - the work happens when the player is not waiting for an area to appear.
  {
    auto evt = scoped_prof("mipmaps");
    const bool busy = loadboost_active();  // true while a blackout or a backlog is in progress
    // FIX 42a: a city load defers 600+ chains at once. Draining 2/frame would leave
    // textures unfiltered (shimmering in the distance) for ten seconds, so a large
    // backlog overrides the streaming rate -- it is still far cheaper than the 80+ ms
    // that generating them inline used to cost during the load itself.
    //
    // FIX 46b (AI-assisted): this is *in-frame* GPU work on the render thread, so it
    // has to respect the same "does the frame have room?" question as the upload
    // budget. While busy it was draining 8 chains/frame unconditionally, i.e. adding
    // roughly 8 x 0.43 ms ~= 3.4 ms to a frame that FIX 43 measured as already full.
    // When the game frame time says there is no room, drain a token amount instead
    // and let the backlog be paid off after the area has appeared -- which is exactly
    // what deferring the mips was for.
    const size_t pending_before = mipq_pending();
    int rate;
    // FIX 94 (AI-assisted): the mip drain gets its own wall-clock cap. The f93 log showed
    // the idle drain (rate 16) costing 5-8 ms per frame against a 2 ms idle-lean budget
    // (~0.4 ms per glGenerateMipmap): the small dips after every area load. This caps the
    // drain's OWN time (min 1 chain per call), unlike reverted FIX 49, which clamped it
    // against the upload's time and collapsed the rate while a stream was still adding
    // chains. Idle frames add no chains, so the queue still always empties.
    float mip_ms = 1000.f;
#ifdef __SWITCH__
    if (m_in_update_blocking) {
      // FIX 94b: build the chains INSIDE the freeze. f94 deferred them (rate 0) and the
      // 780-1460-chain backlog then drained at 4-8 chains (~1.2 ms each) = 6-10 ms on every
      // gameplay frame after fade-in, with unfiltered textures meanwhile: worse than f93.
      // The frozen frame is invisible; ~10 ms of mips per 40 ms upload pass keeps the
      // f93 upload:mip ratio while ending the sweep with a small backlog.
      rate = 16;
      mip_ms = 10.f;
    } else
#endif
    if (!busy) {
      rate = 16;  // idle: nothing to protect, clear the backlog fast
#ifdef __SWITCH__
      mip_ms = m_frame_gap_ema_ms > 30.0 ? 2.0f : 4.0f;
#endif
    } else {
      // FIX 68 (AI-assisted): restore FIX 42a's backlog override inside the
      // token bands. The F67 hardware log (jak2 atoll, 2026-10-03) deferred 426
      // mip chains and drained them at rate 1-2 for the whole stream - 7-14 s
      // of unfiltered, shimmering textures AFTER "ready in 9.97s". That is
      // exactly the "ten seconds unfiltered" FIX 42a was written to prevent;
      // FIX 46b's flat token rates quietly re-introduced it whenever the
      // renderer holds the EMA above 30 ms - which is during every real
      // stream-in, i.e. precisely when the backlog is largest. The per-chain
      // submit cost is ~0.43 ms (FIX 46b's own measurement), so 6/frame is
      // ~2.6 ms of submit: real, but bounded, and it buys the backlog down 3-6x
      // faster. The band keys still shrink the rate when frames are dropping.
      const bool huge_backlog = pending_before > 256;
      if (m_frame_gap_ema_ms > 38.0) {
        rate = huge_backlog ? 4 : 2;  // frames already being dropped: minimal, not zero
      } else if (m_frame_gap_ema_ms > 30.0) {
        rate = huge_backlog ? 6 : 3;  // holding 30 fps: token, but backlog-aware
      } else {
        rate = huge_backlog ? 8 : 3;  // real headroom: catch up quickly
      }
#ifdef __SWITCH__
      // FIX 95 (AI-assisted): time-cap the STREAMING mip drain too. At ~1.2 ms/chain the
      // huge-backlog rates cost 5-10 ms on frames that already carry 7-8 ms of uploads on
      // a ~32 ms render: city/zoomer slow motion. Cap on the drain's OWN time (min 1
      // chain, so it always progresses - not the FIX 49 mistake); the backlog finishes on
      // idle frames (rate 16) once streaming ends.
      mip_ms = m_frame_gap_ema_ms > 30.0 ? 2.0f : 3.5f;
#endif
    }
#ifdef __SWITCH__
    // FIX 95: gameplay mips also live inside the measured free time (minus whatever the
    // upload stage was granted). No room: one chain every 4th frame, so the queue still
    // moves; the full drain happens on frames that really have the time.
    if (!m_in_update_blocking && !m_blackout && rate > 0) {
      const double free_ms = switch_free_ms_min();
      if (free_ms >= 0) {
        const double used = std::strncmp(m_budget_mode, "catchup", 7) == 0 ? g_loader_budget.ms : 0.0;
        const double left = free_ms - kFreeMarginMs - used;
        if (left < 0.8) {
          rate = (s_free_frame % 4) == 0 ? 1 : 0;
        } else {
          mip_ms = std::min(mip_ms, (float)left);
        }
      }
    }
#endif

    // FIX 49 REVERTED (AI-assisted): do NOT clamp `rate` against the budget the
    // upload already spent this frame.
    //
    // That was tried on hardware on 2026-09-27 and made the city strictly worse.
    // From that build's jak2 gk_stdout.txt:
    //
    //   1214 frames chose rate=1 (84% of all drain frames)
    //   [loader] mipmaps: 768 deferred chains left   (pegged at the ceiling)
    //   stage texture took 12.13 ms                  (vs 5.2 ms in the FIX 48 build)
    //   Loader::update slow setup: 19.5ms            (vs 9.4 ms in the FIX 48 build)
    //
    // and the user reported the FIX 38 symptom returning: the dead city never
    // loaded in, and NPCs / the zoomer did not appear on re-entry.
    //
    // The reasoning was wrong in a way worth recording. The upload stage does not
    // stop at the budget - `stage texture took` is 5.2 ms against a 4-5 ms
    // catchup allowance, i.e. it is *already* over budget before this block runs.
    // So `spent_ms >= budget_ms * 0.9` was true almost always, rate collapsed to 1
    // permanently, and the consequence was not "defer the mips to later frames" as
    // intended: an ever-growing 768-deep queue of textures held at MAX_LEVEL 0
    // means hundreds of textures stay mipmap-less, each still needing MAX_LEVEL
    // raised and a glGenerateMipmap, while the queue itself holds pressure up.
    // That fed back into the upload stage (5.2 -> 12.1 ms) and into frame time,
    // and the loader then throttled itself further - the exact death spiral FIX 38
    // removed. "Backlog beats frame time" (FIX 38) is the rule; this broke it.
    //
    // The rate control above is therefore left keyed on the frame-gap EMA only
    // (FIX 68 made the token rates backlog-aware), with FIX 38's guarantee that
    // a backlog is what justifies real work.

    const int did = rate > 0 ? mipq_process(rate, mip_ms) : 0;
    // FIX 49 (AI-assisted): diagnose-only survivor of the revert. Records what the
    // split actually was so the next hardware log can show, per frame, how the
    // budget was divided between the texture upload and the mip drain. This
    // changes no behaviour.
    g_last_mip_rate = (u32)rate;
    g_last_mip_did = (u32)did;

    static size_t s_last_bucket = (size_t)-1;
    const size_t left = mipq_pending();
    if (did > 0 && left / 256 != s_last_bucket) {
      s_last_bucket = left / 256;
      fmt::print("[loader] mipmaps: {} deferred chains left\n", left);
    }
  }
#endif

  // FIX 33: always drain a little GL garbage, even while another level is
  // staging. The old code only drained when the loader was idle, so a busy
  // loader could never actually free its deleted textures/buffers.
  {
    auto evt = scoped_prof("garbage");
    for (int i = 0; i < 5 && !m_garbage_buffers.empty(); i++) {
      glDeleteBuffers(1, &m_garbage_buffers.back());
      m_garbage_buffers.pop_back();
    }
#ifdef __SWITCH__
    // FIX 99 (AI-assisted): time-box the texture garbage drain during gameplay.
    // f98b jak3 log: every area unload was followed by 15-20 frames of
    // `slow setup: 10-12ms (did=0)` - 20 glDeleteTextures/frame at ~0.5 ms each
    // on nouveau. That landed exactly while the next area streams in, so the
    // FIX 95 free-time clamp saw no room, cut the stream budget to 1 ms, and
    // the area took 5-10 s (plus hitches). Frozen/blackout frames keep the old
    // 20/frame; gameplay deletes for ~1.5 ms (min 1; 3 ms once the queue is
    // large so memory still comes back in a couple of seconds).
    {
      const bool frozen = m_blackout || m_in_update_blocking;
      const double cap_ms = frozen ? 1000.0 : (m_garbage_textures.size() > 400 ? 3.0 : 1.5);
      Timer gc_timer;
      for (int i = 0; i < 20 && !m_garbage_textures.empty(); i++) {
        glDeleteTextures(1, &m_garbage_textures.back());
        m_garbage_textures.pop_back();
        if (gc_timer.getMs() > cap_ms) {
          break;
        }
      }
    }
#else
    for (int i = 0; i < 20 && !m_garbage_textures.empty(); i++) {
      glDeleteTextures(1, &m_garbage_textures.back());
      m_garbage_textures.pop_back();
    }
#endif
  }

  // FIX 52 (AI-assisted): everything this frame's loader submission consisted of has now
  // been enqueued, so this is the one place a fence can measure the whole of it. Must be
  // the last thing update() does, and must run before the slow-setup line below so that
  // line can print the measured cost.
  //
  // Guarded because the probe's state (m_loader_gpu_ema_ms etc.) and the budget that
  // consumes it are both __SWITCH__-only -- the desktop budget is a fixed constant set in
  // LoaderStages.cpp and never retuned, so there is nothing on desktop for this to feed.
#ifdef __SWITCH__
  gpu_cost_probe();
#endif

#ifdef __SWITCH__
  if (loader_timer.getMs() > 5 || m_loader_gpu_ema_ms > 2.0) {
    // FIX 49 (AI-assisted): report the split, not just the total. The pre-FIX-49
    // log could show `slow setup: 9.1ms` without saying how much of it was the
    // mip drain that ran *after* the upload; this makes the two line items
    // greppable side by side with `stage texture took`.
    //
    // FIX 52 (AI-assisted): and report `gpu=`, the measured GPU cost of this frame's
    // loader work (see gpu_cost_probe). The trigger widened to `|| gpu > 2ms` because
    // the whole point is that these two numbers disagree: a frame whose *submit* time
    // is 1 ms can still be 15 ms of GPU work, and that frame was previously invisible.
    // A negative `gpu=` means the probe hit its 8 ms wait timeout, i.e. the real cost
    // is at least that. (AI-assisted)
    //
    // FIX 54 (AI-assisted): `gpu=` is printed as INTEGER TENTHS OF A MILLISECOND
    // (`gpu=-137` means -13.7 ms), not as a `{:.1f}` double. The first FIX 52 build
    // crashed here: `pc_off=0xaba838` symbolized to
    // fmt::v11::detail::do_write_float<..., decimal_fp<double>, ...>, i.e. the crash
    // was inside fmt's float formatting, in this very call. This line already had two
    // `{:.1f}` doubles (submit time and budget) before FIX 52 added a third; that is
    // three doubles through fmt's variadic path on this target, and it does not
    // survive. Integer tenths keep the same 0.1 ms resolution, stay readable, and take
    // the third double back out of the argument list. Do not reintroduce a float here.
    fmt::print(
        "Loader::update slow setup: {:.1f}ms (mip rate={} did={}, budget={:.1f}ms, gpu={})\n",
        loader_timer.getMs(), g_last_mip_rate, g_last_mip_did, (double)g_loader_budget.ms,
        (int)(m_loader_gpu_last_ms * 10.0));
  }
#endif
}

std::optional<MercRef> Loader::get_merc_model(const char* model_name) {
  // don't think we need to lock here...
  const auto& it = m_all_merc_models.find(model_name);
  if (it != m_all_merc_models.end() && !it->second.empty()) {
    // it->second.front().parent_level->frames_since_last_used = 0;
    return it->second.front();
  } else {
    return std::nullopt;
  }
}

void Loader::do_reload_level(const std::string& name, TexturePool& texture_pool) {
  std::unique_ptr<LevelData> lev;
  {
    std::unique_lock<std::mutex> lk(m_loader_mutex);
    auto it = m_loaded_tfrag3_levels.find(name);
    if (it == m_loaded_tfrag3_levels.end()) {
      return;
    }
    lev = std::move(it->second);
    m_loaded_tfrag3_levels.erase(it);
  }
  fmt::print("force reload: unloading {}\n", name);
  // FIX 33: shared unload path - the level's buffers return to the pool and
  // get reused when it reloads.
  unload_level_gpu_objects(*lev, texture_pool);

  std::unique_lock lk(m_loader_mutex);
  if (m_level_to_load.empty()) {
    m_level_to_load = name;
    lk.unlock();
    m_loader_cv.notify_all();
  }
}

void Loader::do_reload_common(TexturePool& tex_pool) {
  fmt::print("loader: force reloading common level\n");
  {
    std::unique_lock lk(tex_pool.mutex());
    for (size_t i = 0;
         i < m_common_level.textures.size() && i < m_common_level.level->textures.size(); i++) {
      auto& tex = m_common_level.level->textures[i];
      if (tex.load_to_pool) {
        tex_pool.unload_texture(PcTextureId::from_combo_id(tex.combo_id),
                                m_common_level.textures[i]);
      }
    }
  }
  for (auto tex : m_common_level.textures) {
    glDeleteTextures(1, &tex);
  }
  // FIX 33: return the common level's merc buffers to the pool instead of
  // leaking them on every common reload.
  m_buffer_pool.release(m_common_level.merc_vertices);
  m_buffer_pool.release(m_common_level.merc_indices);
  for (auto& model : m_common_level.level->merc_data.models) {
    auto it = m_all_merc_models.find(model.name);
    if (it == m_all_merc_models.end())
      continue;
    MercRef ref{&model, m_common_level.load_id};
    auto ref_it = std::ranges::find(it->second, ref);
    if (ref_it != it->second.end())
      it->second.erase(ref_it);
  }
  m_common_level = LevelData{};
  load_common(tex_pool, "GAME");
}

void Loader::do_reload(TexturePool& texture_pool) {
  fmt::print("loader: force reloading all levels\n");
  // FIX 33: extract all levels first, then tear down their GPU objects
  // through the shared path (buffers return to the pool).
  std::vector<std::unique_ptr<LevelData>> levels;
  {
    std::unique_lock<std::mutex> lk(m_loader_mutex);
    levels.reserve(m_loaded_tfrag3_levels.size());
    for (auto& [name, lev] : m_loaded_tfrag3_levels) {
      levels.push_back(std::move(lev));
    }
    m_loaded_tfrag3_levels.clear();
  }
  for (auto& lev : levels) {
    unload_level_gpu_objects(*lev, texture_pool);
  }

  for (auto buf : m_garbage_buffers)
    glDeleteBuffers(1, &buf);
  m_garbage_buffers.clear();
  flush_texture_garbage();
  // A full reload wants a clean slate: actually delete the pooled buffers
  // instead of keeping them around for reuse (FIX 33).
  m_buffer_pool.clear();

  set_want_levels(m_desired_levels);
}