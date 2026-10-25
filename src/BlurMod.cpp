// Dynamic Container UI Blur - Created by ChrisNiruu
#include <pl/Config.hpp>
#include <pl/Mod.hpp>
#include <pl/ModMenu.hpp>
#include <pl/memory/Hook.hpp>
#include <pl/memory/Signature.hpp>

#include <EGL/egl.h>
#include <dlfcn.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <optional>
#include <string>

#include "BlurConfig.hpp"
#include "BlurRenderer.hpp"
#include "ScreenDetector.hpp"

namespace {

using Clock = std::chrono::steady_clock;
namespace mm = pl::modmenu;

constexpr const char *kModuleId = "dynamic_container_ui_blur";
constexpr const char *kCredit = "Created by ChrisNiruu";

bool parseBool(std::string_view v, bool fallback) {
  if (v == "true" || v == "1" || v == "on") return true;
  if (v == "false" || v == "0" || v == "off") return false;
  return fallback;
}
int parseInt(std::string_view v, int fallback) {
  const std::string s(v);
  char *end = nullptr;
  const double d = std::strtod(s.c_str(), &end);
  return end == s.c_str() ? fallback : static_cast<int>(d + (d < 0 ? -0.5 : 0.5));
}
float parseFloat(std::string_view v, float fallback) {
  const std::string s(v);
  char *end = nullptr;
  const float f = std::strtof(s.c_str(), &end);
  return end == s.c_str() ? fallback : f;
}
bool validColor(std::string_view v) {
  if ((v.size() != 7 && v.size() != 9) || v.front() != '#') return false;
  return std::all_of(v.begin() + 1, v.end(), [](unsigned char c) { return std::isxdigit(c) != 0; });
}
void tintToRgb(const std::string &hex, float out[3]) {
  out[0] = out[1] = out[2] = 0.0f;
  if (!validColor(hex)) return;
  const size_t start = hex.size() - 6;
  for (int i = 0; i < 3; ++i)
    out[i] = static_cast<float>(std::strtol(hex.substr(start + static_cast<size_t>(i) * 2, 2).c_str(), nullptr, 16)) / 255.0f;
}
int parseQuality(std::string_view v, int fallback) {
  if (v == "Performance") return 0;
  if (v == "Balanced") return 1;
  if (v == "Ultra") return 2;
  return std::clamp(parseInt(v, fallback), 0, 2);
}

void normalize(BlurConfig &c) {
  c.blurRadius = std::clamp(c.blurRadius, 0, 30);
  c.overlayOpacity = std::clamp(c.overlayOpacity, 0, 80);
  c.transitionMs = std::clamp(c.transitionMs, 0, 1000);
  c.quality = std::clamp(c.quality, 0, 2);
  c.downscale = std::clamp(c.downscale, 0.25f, 1.0f);
  c.fallbackFps = std::clamp(c.fallbackFps, 10, 60);
  if (!validColor(c.tintColor)) c.tintColor = "#000000";
}

bool groupEnabled(const BlurConfig &c, ScreenGroup g) {
  switch (g) {
  case ScreenGroup::Inventory: return c.inventory;
  case ScreenGroup::Chests: return c.chests;
  case ScreenGroup::Crafting: return c.crafting;
  case ScreenGroup::Processing: return c.processing;
  case ScreenGroup::Specialty: return c.specialty;
  case ScreenGroup::Storage: return c.storage;
  default: return false;
  }
}

} // namespace

class BlurMod {
public:
  static BlurMod &instance() {
    static BlurMod inst;
    return inst;
  }

  BlurMod() : mSelf(*ll::mod::NativeMod::current()) {}

  bool load() {
    auto &log = mSelf.getLogger();
    std::filesystem::create_directories(mSelf.getConfigDir());
    mConfig.emplace();
    if (!mConfig->load()) log.warn("Could not persist config.json");
    normalize(mConfig->value());

    if (!mDetector.loadScreens(mSelf.getResourceDir() / "screens.json"))
      log.warn("resources/screens.json missing or invalid: no screens will match");
    if (!mDetector.loadRules(mSelf.getResourceDir() / "hooks.json"))
      log.warn("resources/hooks.json has no usable rules: container detection is inactive "
               "(use 'Blur everywhere (debug)' to test the GL pipeline)");
    return true;
  }

  bool enable() {
    registerMenu();
    installHooks();
    return true;
  }

  bool disable() {
    mm::unregisterModule(kModuleId);
    mRenderHook.reset();
    mSwapHook.reset();
    sActive.store(false);
    return true;
  }

  static uintptr_t screenRenderDetour(void *a0, void *a1, void *a2, void *a3, void *a4, void *a5) {
    if (sActive.load(std::memory_order_relaxed)) {
      try { instance().onScreenRender(a0); } catch (...) {}
    }
    return sOrigRender ? sOrigRender(a0, a1, a2, a3, a4, a5) : 0;
  }

  static EGLBoolean swapDetour(EGLDisplay d, EGLSurface s) {
    if (sActive.load(std::memory_order_relaxed)) {
      try { instance().onSwap(); } catch (...) {}
    }
    return sOrigSwap ? sOrigSwap(d, s) : EGL_FALSE;
  }

  static inline uintptr_t (*sOrigRender)(void *, void *, void *, void *, void *, void *) = nullptr;
  static inline EGLBoolean (*sOrigSwap)(EGLDisplay, EGLSurface) = nullptr;

private:
  BlurConfig snapshot() {
    std::lock_guard lock(mConfigMutex);
    return mConfig->value();
  }

  void installHooks() {
    auto &log = mSelf.getLogger();
    sActive.store(true);

    if (void *swap = dlsym(RTLD_DEFAULT, "eglSwapBuffers")) {
      mSwapHook = pl::memory::HookHandle(swap, reinterpret_cast<void *>(&swapDetour),
                                         reinterpret_cast<pl::memory::FuncPtr *>(&sOrigSwap));
      if (!mSwapHook.installed()) log.warn("eglSwapBuffers hook failed (FPS fallback + debug mode off)");
    }

    for (const auto &rule : mDetector.rules()) {
      const uintptr_t addr = pl::memory::resolveSignature(rule.signature, "libminecraftpe.so");
      if (!addr) continue;
      mRule = rule;
      mRenderHook = pl::memory::HookHandle(reinterpret_cast<void *>(addr),
                                           reinterpret_cast<void *>(&screenRenderDetour),
                                           reinterpret_cast<pl::memory::FuncPtr *>(&sOrigRender));
      if (mRenderHook.installed()) {
        log.info("Screen render hook installed via rule '{}'", rule.id);
        return;
      }
    }
    log.warn("No screen render hook installed; container blur will not trigger.");
  }

  void onScreenRender(void *screen) {
    const BlurConfig cfg = snapshot();
    const auto now = Clock::now();
    const double gap = std::chrono::duration<double>(now - mLastCall).count();
    mLastCall = now;
    const float dt = static_cast<float>(std::clamp(gap, 0.0, 0.1));

    if (!cfg.enabled || cfg.debugBlurEverywhere) { resetState(); return; }

    std::string name;
    ScreenGroup group = ScreenGroup::None;
    if (mDetector.readScreenName(screen, mRule, name)) {
      if (name != mLastName) {
        mLastName = name;
        mSelf.getLogger().info("Screen: {}", name);
      }
      group = mDetector.classify(name);
    }
    const bool target = groupEnabled(cfg, group);

    if (gap > 0.5 && target) mFade = 0.0f;
    const float dur = std::max(cfg.transitionMs, 1) / 1000.0f;
    mFade = cfg.transitionMs == 0 ? (target ? 1.0f : 0.0f)
                                  : std::clamp(mFade + (target ? dt : -dt) / dur, 0.0f, 1.0f);

    if (!target && mFade <= 0.0f) { resetState(); return; }
    if (target && !mWasTarget) mOpenedAt = now;
    mWasTarget = target;

    if (!initRenderer()) return;

    if (target && cfg.lowEndFallback && !mFallback && sFps.load() > 0.0f &&
        std::chrono::duration<double>(now - mOpenedAt).count() > 0.6) {
      if (sFps.load() < static_cast<float>(cfg.fallbackFps)) {
        if (!mBelowSince) mBelowSince = now;
        else if (std::chrono::duration<double>(now - *mBelowSince).count() > 1.0) {
          mFallback = true;
          mSelf.getLogger().info("FPS below {}: using dark overlay fallback", cfg.fallbackFps);
        }
      } else {
        mBelowSince.reset();
      }
    }

    const float eased = mFade * mFade * (3.0f - 2.0f * mFade);
    drawFrame(cfg, eased, target && !mFallback);
  }

  void onSwap() {
    const BlurConfig cfg = snapshot();
    const auto now = Clock::now();
    if (mLastSwap.time_since_epoch().count() != 0) {
      const double dt = std::chrono::duration<double>(now - mLastSwap).count();
      if (dt > 0.0 && dt < 0.5) {
        const float inst = static_cast<float>(1.0 / dt);
        const float cur = sFps.load();
        sFps.store(cur <= 0.0f ? inst : cur * 0.9f + inst * 0.1f);
      }
    }
    mLastSwap = now;

    if (cfg.enabled && cfg.debugBlurEverywhere && initRenderer()) drawFrame(cfg, 1.0f, true);
  }

  void drawFrame(const BlurConfig &cfg, float fade, bool update) {
    static constexpr int kLevels[3] = {2, 3, 4};
    BlurParams p;
    p.radiusPx = cfg.blurRadius;
    p.scale = cfg.downscale;
    p.levels = kLevels[std::clamp(cfg.quality, 0, 2)];
    p.useBlur = !mFallback;
    p.overlay = mFallback ? std::max(cfg.overlayOpacity / 100.0f, 0.35f) : cfg.overlayOpacity / 100.0f;
    p.fade = fade;
    tintToRgb(cfg.tintColor, p.tint);
    const bool skip = cfg.quality == 0 && (mFrame++ & 1) != 0;
    p.doUpdate = update && !skip;
    mRenderer.render(p);
  }

  bool initRenderer() {
    if (mRenderer.ok()) return true;
    if (mGlFailed) return false;
    if (!mRenderer.init()) {
      mGlFailed = true;
      mSelf.getLogger().error("GL blur pipeline failed to initialise; blur disabled");
      return false;
    }
    return true;
  }

  void resetState() {
    mFade = 0.0f;
    mWasTarget = false;
    mFallback = false;
    mBelowSince.reset();
    mRenderer.invalidate();
  }

  void registerMenu() {
    const BlurConfig c = snapshot();
    auto s = [](auto v) { return std::to_string(v); };
    auto b = [](bool v) { return std::string(v ? "true" : "false"); };
    char down[16];
    std::snprintf(down, sizeof(down), "%.2f", c.downscale);

    const bool ok =
        mm::ModuleBuilder(kModuleId, "Dynamic Container UI Blur")
            .modId(mSelf.getId())
            .description(std::string("Frosted-glass blur behind inventories, chests, furnaces and more. ") + kCredit)
            .defaultEnabled(c.enabled)
            .config("inventory", "Player Inventory & Armor", mm::ConfigType::Toggle, b(c.inventory))
            .config("chests", "Chests, Ender Chest, Shulker, Barrel", mm::ConfigType::Toggle, b(c.chests))
            .config("crafting", "Crafting, Stonecutter, Loom, Smithing", mm::ConfigType::Toggle, b(c.crafting))
            .config("processing", "Furnace, Smoker, Blast Furnace, Brewing", mm::ConfigType::Toggle, b(c.processing))
            .config("specialty", "Anvil, Enchanter, Grindstone, Trading, Beacon", mm::ConfigType::Toggle, b(c.specialty))
            .config("storage", "Hopper, Dispenser, Dropper", mm::ConfigType::Toggle, b(c.storage))
            .config("blurRadius", "Blur Radius (px)", mm::ConfigType::SliderInt, s(c.blurRadius), "0", "30")
            .config("overlayOpacity", "Darkening Opacity (%)", mm::ConfigType::SliderInt, s(c.overlayOpacity), "0", "80")
            .config("transitionMs", "Fade Duration (ms)", mm::ConfigType::SliderInt, s(c.transitionMs), "0", "1000")
            .config("tintColor", "Tint Color", mm::ConfigType::Color, c.tintColor)
            .config("quality", "Render Quality", mm::ConfigType::Radio, s(c.quality), "Performance,Balanced,Ultra")
            .config("downscale", "Downscale Multiplier", mm::ConfigType::SliderFloat, down, "0.25", "1.0")
            .config("lowEndFallback", "Low-End Fallback (dark overlay)", mm::ConfigType::Toggle, b(c.lowEndFallback))
            .config("fallbackFps", "Fallback FPS Threshold", mm::ConfigType::SliderInt, s(c.fallbackFps), "10", "60")
            .config("debugBlurEverywhere", "Blur Everywhere (debug)", mm::ConfigType::Toggle, b(c.debugBlurEverywhere))
            .onToggle([](std::string_view id, bool on) { instance().onToggle(id, on); })
            .onConfigChanged([](std::string_view id, std::string_view k, std::string_view v) {
              instance().onConfigChanged(id, k, v);
            })
            .registerModule();
    if (!ok) { mSelf.getLogger().warn("Mod Menu registration failed"); return; }

    mm::ConfigSchemaBuilder sb;
    sb.category("containers", "Containers", "Which screens get the blur")
        .category("visual", "Visuals")
        .category("performance", "Performance")
        .category("about", "About")
        .defaultCategory("containers");

    auto node = [&](const char *key, const char *cat, const char *title, mm::ConfigControlTypeV2 type,
                    const std::string &def, const std::string &cur) {
      mm::ConfigNodeV2 n;
      n.id = key; n.key = key; n.category = cat; n.title = title; n.type = type;
      n.defaultValue = def; n.currentValue = cur; n.hasCurrentValue = true;
      return n;
    };
    using T = mm::ConfigControlTypeV2;
    BlurConfig d;
    auto toggle = [&](const char *k, const char *cat, const char *t, bool def, bool cur) {
      sb.node(node(k, cat, t, T::Toggle, b(def), b(cur)));
    };
    toggle("inventory", "containers", "Player Inventory & Armor", d.inventory, c.inventory);
    toggle("chests", "containers", "Chests, Ender Chest, Shulker Box, Barrel", d.chests, c.chests);
    toggle("crafting", "containers", "Crafting Table, Stonecutter, Loom, Smithing", d.crafting, c.crafting);
    toggle("processing", "containers", "Furnace, Smoker, Blast Furnace, Brewing Stand", d.processing, c.processing);
    toggle("specialty", "containers", "Anvil, Enchanter, Grindstone, Trading, Beacon", d.specialty, c.specialty);
    toggle("storage", "containers", "Hopper, Dispenser, Dropper", d.storage, c.storage);

    auto slider = [&](const char *k, const char *cat, const char *t, T type, const std::string &def,
                      const std::string &cur, const char *mn, const char *mx, const char *step, const char *unit) {
      auto n = node(k, cat, t, type, def, cur);
      n.minValue = mn; n.maxValue = mx; n.step = step; n.unit = unit;
      sb.node(std::move(n));
    };
    slider("blurRadius", "visual", "Blur Radius", T::SliderInt, s(d.blurRadius), s(c.blurRadius), "0", "30", "1", "px");
    slider("overlayOpacity", "visual", "Darkening Overlay Opacity", T::SliderInt, s(d.overlayOpacity), s(c.overlayOpacity), "0", "80", "1", "%");
    slider("transitionMs", "visual", "Blur Transition Speed (fade duration)", T::SliderInt, s(d.transitionMs), s(c.transitionMs), "0", "1000", "10", "ms");
    {
      auto n = node("tintColor", "visual", "Custom Tint Color", T::Color, d.tintColor, c.tintColor);
      n.colorAlpha = false;
      sb.node(std::move(n));
    }
    {
      auto n = node("quality", "performance", "Render Quality Preset", T::Choice, "1", s(c.quality));
      n.choiceStyle = mm::ConfigChoiceStyleV2::Segmented;
      n.options = {{"0", "Performance", "Fewer blur passes, refreshes every other frame", "", false, "", false},
                   {"1", "Balanced", "", "", false, "", false},
                   {"2", "Ultra", "Most blur passes", "", false, "", false}};
      sb.node(std::move(n));
    }
    slider("downscale", "performance", "Downscale Resolution Multiplier", T::SliderFloat, "0.5", down, "0.25", "1.0", "0.05", "x");
    toggle("lowEndFallback", "performance", "Low-End Fallback (dark overlay instead of blur)", d.lowEndFallback, c.lowEndFallback);
    slider("fallbackFps", "performance", "Fallback FPS Threshold", T::SliderInt, s(d.fallbackFps), s(c.fallbackFps), "10", "60", "1", "fps");
    toggle("debugBlurEverywhere", "performance", "Blur Everywhere (debug: tests the GL pipeline)", d.debugBlurEverywhere, c.debugBlurEverywhere);

    {
      mm::ConfigNodeV2 n;
      n.id = "credits"; n.category = "about"; n.type = T::Info;
      n.title = kCredit;
      n.description = "Dynamic Container UI Blur v" + mSelf.getVersion();
      sb.node(std::move(n));
    }
    if (!mm::setConfigSchemaJson(kModuleId, sb.toJson()))
      mSelf.getLogger().warn("Config schema rejected; using the basic menu layout");
  }

  bool matches(std::string_view id) const { return id == kModuleId; }

  void onToggle(std::string_view id, bool on) {
    if (!matches(id)) return;
    std::lock_guard lock(mConfigMutex);
    mConfig->value().enabled = on;
    mConfig->save();
  }

  void onConfigChanged(std::string_view id, std::string_view key, std::string_view v) {
    if (!matches(id) || key.empty()) return;
    std::lock_guard lock(mConfigMutex);
    auto &c = mConfig->value();
    if (key == "inventory") c.inventory = parseBool(v, c.inventory);
    else if (key == "chests") c.chests = parseBool(v, c.chests);
    else if (key == "crafting") c.crafting = parseBool(v, c.crafting);
    else if (key == "processing") c.processing = parseBool(v, c.processing);
    else if (key == "specialty") c.specialty = parseBool(v, c.specialty);
    else if (key == "storage") c.storage = parseBool(v, c.storage);
    else if (key == "blurRadius") c.blurRadius = parseInt(v, c.blurRadius);
    else if (key == "overlayOpacity") c.overlayOpacity = parseInt(v, c.overlayOpacity);
    else if (key == "transitionMs") c.transitionMs = parseInt(v, c.transitionMs);
    else if (key == "tintColor") { if (validColor(v)) c.tintColor = std::string(v); }
    else if (key == "quality") c.quality = parseQuality(v, c.quality);
    else if (key == "downscale") c.downscale = parseFloat(v, c.downscale);
    else if (key == "lowEndFallback") c.lowEndFallback = parseBool(v, c.lowEndFallback);
    else if (key == "fallbackFps") c.fallbackFps = parseInt(v, c.fallbackFps);
    else if (key == "debugBlurEverywhere") c.debugBlurEverywhere = parseBool(v, c.debugBlurEverywhere);
    else return;
    normalize(c);
    mConfig->save();
  }

  ll::mod::NativeMod &mSelf;
  std::optional<pl::config::ConfigFile<BlurConfig>> mConfig;
  std::mutex mConfigMutex;
  ScreenDetector mDetector;
  HookRule mRule;
  pl::memory::HookHandle mRenderHook, mSwapHook;
  BlurRenderer mRenderer;

  Clock::time_point mLastCall{}, mLastSwap{}, mOpenedAt{};
  std::optional<Clock::time_point> mBelowSince;
  std::string mLastName;
  float mFade = 0.0f;
  bool mWasTarget = false, mFallback = false, mGlFailed = false;
  unsigned mFrame = 0;

  static inline std::atomic<bool> sActive{false};
  static inline std::atomic<float> sFps{0.0f};
};

PL_REGISTER_MOD(BlurMod, BlurMod::instance())
