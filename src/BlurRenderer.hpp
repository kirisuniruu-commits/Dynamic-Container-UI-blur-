#pragma once
#include <cstdint>
#include <vector>

struct BlurParams {
  int radiusPx = 12;
  float scale = 0.5f;
  int levels = 3;
  float overlay = 0.35f;
  float tint[3] = {0, 0, 0};
  float fade = 1.0f;
  bool useBlur = true;
  bool doUpdate = true;
};

class BlurRenderer {
public:
  bool init();
  void render(const BlurParams &p);
  void invalidate() { mHasBlur = false; }
  bool ok() const { return mReady; }

private:
  struct Level {
    uint32_t tex = 0, fbo = 0;
    int w = 0, h = 0;
  };
  bool ensureTargets(int w0, int h0, int levels);
  void freeTargets();
  void drawTriangle();

  bool mReady = false;
  bool mHasBlur = false;
  uint32_t mDown = 0, mUp = 0, mComposite = 0;
  int mLocDown[2] = {-1, -1}, mLocUp[2] = {-1, -1};
  int mLocComp[4] = {-1, -1, -1, -1};
  std::vector<Level> mLevels;
  int mLastW0 = 0, mLastH0 = 0;
};
