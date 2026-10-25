#pragma once
#include <string>

struct BlurConfig {
  int version = 1;

  bool enabled = true;

  bool inventory = true;
  bool chests = true;
  bool crafting = true;
  bool processing = true;
  bool specialty = true;
  bool storage = true;

  int blurRadius = 12;
  int overlayOpacity = 35;
  int transitionMs = 250;
  std::string tintColor = "#000000";

  int quality = 1;
  float downscale = 0.5f;
  bool lowEndFallback = true;
  int fallbackFps = 25;

  bool debugBlurEverywhere = false;
};
