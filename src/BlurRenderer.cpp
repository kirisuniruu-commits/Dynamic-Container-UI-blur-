#include "BlurRenderer.hpp"

#include <EGL/egl.h>
#include <GLES3/gl3.h>

#include <algorithm>
#include <android/log.h>
#include <cmath>

#define TAG "ContainerBlur"
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

namespace {

const char *kVert = R"(#version 300 es
out vec2 vUv;
void main() {
  vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));
  vUv = p;
  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
})";

const char *kDownFrag = R"(#version 300 es
precision mediump float;
in vec2 vUv; out vec4 o;
uniform sampler2D uTex; uniform vec2 uHalfPixel; uniform float uOffset;
void main() {
  vec2 h = uHalfPixel * uOffset;
  vec4 s = texture(uTex, vUv) * 4.0;
  s += texture(uTex, vUv - h);
  s += texture(uTex, vUv + h);
  s += texture(uTex, vUv + vec2(h.x, -h.y));
  s += texture(uTex, vUv - vec2(h.x, -h.y));
  o = vec4((s / 8.0).rgb, 1.0);
})";

const char *kUpFrag = R"(#version 300 es
precision mediump float;
in vec2 vUv; out vec4 o;
uniform sampler2D uTex; uniform vec2 uHalfPixel; uniform float uOffset;
void main() {
  vec2 h = uHalfPixel * uOffset;
  vec4 s = texture(uTex, vUv + vec2(-h.x * 2.0, 0.0));
  s += texture(uTex, vUv + vec2(-h.x, h.y)) * 2.0;
  s += texture(uTex, vUv + vec2(0.0, h.y * 2.0));
  s += texture(uTex, vUv + vec2(h.x, h.y)) * 2.0;
  s += texture(uTex, vUv + vec2(h.x * 2.0, 0.0));
  s += texture(uTex, vUv + vec2(h.x, -h.y)) * 2.0;
  s += texture(uTex, vUv + vec2(0.0, -h.y * 2.0));
  s += texture(uTex, vUv + vec2(-h.x, -h.y)) * 2.0;
  o = vec4((s / 12.0).rgb, 1.0);
})";

const char *kCompFrag = R"(#version 300 es
precision mediump float;
in vec2 vUv; out vec4 o;
uniform sampler2D uTex; uniform vec3 uTint; uniform float uOverlay;
uniform float uFade; uniform float uBlurMix;
void main() {
  vec3 blur = texture(uTex, vUv).rgb;
  vec3 col = mix(uTint, mix(blur, uTint, uOverlay), uBlurMix);
  float a = mix(uOverlay, 1.0, uBlurMix) * uFade;
  o = vec4(col, a);
})";

GLuint compile(GLenum type, const char *src) {
  GLuint s = glCreateShader(type);
  glShaderSource(s, 1, &src, nullptr);
  glCompileShader(s);
  GLint ok = 0;
  glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
  if (!ok) {
    char log[512];
    glGetShaderInfoLog(s, sizeof(log), nullptr, log);
    LOGE("shader compile failed: %s", log);
    glDeleteShader(s);
    return 0;
  }
  return s;
}

GLuint link(const char *frag) {
  GLuint vs = compile(GL_VERTEX_SHADER, kVert);
  GLuint fs = compile(GL_FRAGMENT_SHADER, frag);
  if (!vs || !fs) return 0;
  GLuint p = glCreateProgram();
  glAttachShader(p, vs);
  glAttachShader(p, fs);
  glLinkProgram(p);
  glDeleteShader(vs);
  glDeleteShader(fs);
  GLint ok = 0;
  glGetProgramiv(p, GL_LINK_STATUS, &ok);
  if (!ok) {
    char log[512];
    glGetProgramInfoLog(p, sizeof(log), nullptr, log);
    LOGE("program link failed: %s", log);
    glDeleteProgram(p);
    return 0;
  }
  return p;
}

struct GlState {
  GLint prog, vao, activeTex, tex0, sampler0, readFbo, drawFbo;
  GLint vp[4], sc[4];
  GLint bSrcRgb, bDstRgb, bSrcA, bDstA, bEqRgb, bEqA;
  GLboolean blend, depth, cull, scissor, stencil, depthMask, colorMask[4];

  void save() {
    glGetIntegerv(GL_CURRENT_PROGRAM, &prog);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &vao);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &activeTex);
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &tex0);
    glGetIntegerv(GL_SAMPLER_BINDING, &sampler0);
    glActiveTexture(static_cast<GLenum>(activeTex));
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &readFbo);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &drawFbo);
    glGetIntegerv(GL_VIEWPORT, vp);
    glGetIntegerv(GL_SCISSOR_BOX, sc);
    glGetIntegerv(GL_BLEND_SRC_RGB, &bSrcRgb);
    glGetIntegerv(GL_BLEND_DST_RGB, &bDstRgb);
    glGetIntegerv(GL_BLEND_SRC_ALPHA, &bSrcA);
    glGetIntegerv(GL_BLEND_DST_ALPHA, &bDstA);
    glGetIntegerv(GL_BLEND_EQUATION_RGB, &bEqRgb);
    glGetIntegerv(GL_BLEND_EQUATION_ALPHA, &bEqA);
    blend = glIsEnabled(GL_BLEND);
    depth = glIsEnabled(GL_DEPTH_TEST);
    cull = glIsEnabled(GL_CULL_FACE);
    scissor = glIsEnabled(GL_SCISSOR_TEST);
    stencil = glIsEnabled(GL_STENCIL_TEST);
    glGetBooleanv(GL_DEPTH_WRITEMASK, &depthMask);
    glGetBooleanv(GL_COLOR_WRITEMASK, colorMask);
  }

  static void set(GLenum cap, GLboolean on) {
    if (on) glEnable(cap); else glDisable(cap);
  }

  void restore() const {
    glUseProgram(static_cast<GLuint>(prog));
    glBindVertexArray(static_cast<GLuint>(vao));
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(tex0));
    glBindSampler(0, static_cast<GLuint>(sampler0));
    glActiveTexture(static_cast<GLenum>(activeTex));
    glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(readFbo));
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(drawFbo));
    glViewport(vp[0], vp[1], vp[2], vp[3]);
    glScissor(sc[0], sc[1], sc[2], sc[3]);
    glBlendFuncSeparate(static_cast<GLenum>(bSrcRgb), static_cast<GLenum>(bDstRgb),
                        static_cast<GLenum>(bSrcA), static_cast<GLenum>(bDstA));
    glBlendEquationSeparate(static_cast<GLenum>(bEqRgb), static_cast<GLenum>(bEqA));
    set(GL_BLEND, blend);
    set(GL_DEPTH_TEST, depth);
    set(GL_CULL_FACE, cull);
    set(GL_SCISSOR_TEST, scissor);
    set(GL_STENCIL_TEST, stencil);
    glDepthMask(depthMask);
    glColorMask(colorMask[0], colorMask[1], colorMask[2], colorMask[3]);
    while (glGetError() != GL_NO_ERROR) {}
  }
};

bool currentTargetSize(int fbo, const GlState &st, int &w, int &h) {
  if (fbo == 0) {
    EGLDisplay d = eglGetCurrentDisplay();
    EGLSurface s = eglGetCurrentSurface(EGL_DRAW);
    EGLint ew = 0, eh = 0;
    if (d != EGL_NO_DISPLAY && s != EGL_NO_SURFACE &&
        eglQuerySurface(d, s, EGL_WIDTH, &ew) && eglQuerySurface(d, s, EGL_HEIGHT, &eh) &&
        ew > 0 && eh > 0) {
      w = ew;
      h = eh;
      return true;
    }
  }
  w = st.vp[2];
  h = st.vp[3];
  return w > 0 && h > 0;
}

} // namespace

bool BlurRenderer::init() {
  if (mReady) return true;
  mDown = link(kDownFrag);
  mUp = link(kUpFrag);
  mComposite = link(kCompFrag);
  if (!mDown || !mUp || !mComposite) return false;

  mLocDown[0] = glGetUniformLocation(mDown, "uHalfPixel");
  mLocDown[1] = glGetUniformLocation(mDown, "uOffset");
  mLocUp[0] = glGetUniformLocation(mUp, "uHalfPixel");
  mLocUp[1] = glGetUniformLocation(mUp, "uOffset");
  mLocComp[0] = glGetUniformLocation(mComposite, "uTint");
  mLocComp[1] = glGetUniformLocation(mComposite, "uOverlay");
  mLocComp[2] = glGetUniformLocation(mComposite, "uFade");
  mLocComp[3] = glGetUniformLocation(mComposite, "uBlurMix");
  mReady = true;
  return true;
}

void BlurRenderer::freeTargets() {
  for (auto &l : mLevels) {
    if (l.fbo) glDeleteFramebuffers(1, &l.fbo);
    if (l.tex) glDeleteTextures(1, &l.tex);
  }
  mLevels.clear();
  mHasBlur = false;
}

bool BlurRenderer::ensureTargets(int w0, int h0, int levels) {
  if (mLevels.size() == static_cast<size_t>(levels + 1) && mLastW0 == w0 && mLastH0 == h0)
    return true;
  freeTargets();
  mLevels.resize(static_cast<size_t>(levels + 1));
  for (int i = 0; i <= levels; ++i) {
    Level &l = mLevels[static_cast<size_t>(i)];
    l.w = std::max(1, w0 >> i);
    l.h = std::max(1, h0 >> i);
    glGenTextures(1, &l.tex);
    glBindTexture(GL_TEXTURE_2D, l.tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, l.w, l.h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glGenFramebuffers(1, &l.fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, l.fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, l.tex, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
      LOGE("blur framebuffer %d incomplete", i);
      freeTargets();
      return false;
    }
  }
  mLastW0 = w0;
  mLastH0 = h0;
  return true;
}

void BlurRenderer::drawTriangle() { glDrawArrays(GL_TRIANGLES, 0, 3); }

void BlurRenderer::render(const BlurParams &p) {
  if (!mReady) return;

  GlState st;
  st.save();

  int srcW = 0, srcH = 0;
  if (!currentTargetSize(st.drawFbo, st, srcW, srcH)) {
    st.restore();
    return;
  }

  glBindVertexArray(0);
  glBindSampler(0, 0);
  glActiveTexture(GL_TEXTURE0);
  glDisable(GL_DEPTH_TEST);
  glDisable(GL_CULL_FACE);
  glDisable(GL_STENCIL_TEST);
  glDisable(GL_SCISSOR_TEST);
  glDisable(GL_BLEND);
  glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

  const int levels = std::clamp(p.levels, 1, 5);
  const bool wantBlur = p.useBlur && p.radiusPx > 0;

  if (wantBlur && p.doUpdate) {
    const int w0 = std::max(1, static_cast<int>(std::lround(srcW * p.scale)));
    const int h0 = std::max(1, static_cast<int>(std::lround(srcH * p.scale)));
    if (ensureTargets(w0, h0, levels)) {
      glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(st.drawFbo));
      glBindFramebuffer(GL_DRAW_FRAMEBUFFER, mLevels[0].fbo);
      glBlitFramebuffer(0, 0, srcW, srcH, 0, 0, w0, h0, GL_COLOR_BUFFER_BIT, GL_LINEAR);

      const float offset =
          std::clamp(static_cast<float>(p.radiusPx) * p.scale / static_cast<float>(1 << levels), 0.25f, 8.0f);

      glUseProgram(mDown);
      glUniform1f(mLocDown[1], offset);
      for (int i = 0; i < levels; ++i) {
        const Level &src = mLevels[static_cast<size_t>(i)];
        const Level &dst = mLevels[static_cast<size_t>(i + 1)];
        glBindFramebuffer(GL_FRAMEBUFFER, dst.fbo);
        glViewport(0, 0, dst.w, dst.h);
        glBindTexture(GL_TEXTURE_2D, src.tex);
        glUniform2f(mLocDown[0], 0.5f / static_cast<float>(src.w), 0.5f / static_cast<float>(src.h));
        drawTriangle();
      }

      glUseProgram(mUp);
      glUniform1f(mLocUp[1], offset);
      for (int i = levels; i >= 1; --i) {
        const Level &src = mLevels[static_cast<size_t>(i)];
        const Level &dst = mLevels[static_cast<size_t>(i - 1)];
        glBindFramebuffer(GL_FRAMEBUFFER, dst.fbo);
        glViewport(0, 0, dst.w, dst.h);
        glBindTexture(GL_TEXTURE_2D, src.tex);
        glUniform2f(mLocUp[0], 0.5f / static_cast<float>(src.w), 0.5f / static_cast<float>(src.h));
        drawTriangle();
      }
      mHasBlur = true;
    }
  }

  const bool blurMix = wantBlur && mHasBlur && !mLevels.empty();
  glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(st.drawFbo));
  glViewport(0, 0, srcW, srcH);
  glEnable(GL_BLEND);
  glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ZERO, GL_ONE);
  glBlendEquation(GL_FUNC_ADD);
  glUseProgram(mComposite);
  glBindTexture(GL_TEXTURE_2D, blurMix ? mLevels[0].tex : 0);
  glUniform3f(mLocComp[0], p.tint[0], p.tint[1], p.tint[2]);
  glUniform1f(mLocComp[1], p.overlay);
  glUniform1f(mLocComp[2], p.fade);
  glUniform1f(mLocComp[3], blurMix ? 1.0f : 0.0f);
  drawTriangle();

  st.restore();
}
