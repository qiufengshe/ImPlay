#include <imgui.h>
#include <imgui_internal.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>
#include <fonts/fontawesome.h>
#include <fonts/unifont.h>
#define GL_SILENCE_DEPRECATION
#if defined(IMGUI_IMPL_OPENGL_ES2)
#include <GLES2/gl2.h>
#endif
#include <GLFW/glfw3.h>
#ifdef _WIN32
#define GLFW_EXPOSE_NATIVE_WIN32
#elif defined(__APPLE__)
#define GLFW_EXPOSE_NATIVE_COCOA
#else
#define GLFW_EXPOSE_NATIVE_X11
#endif
#include <GLFW/glfw3native.h>
#include <thread>
#include "window.h"

namespace ImPlay {
Window::Window() {
  const char* title = "ImPlay";

  config.load();
  initGLFW(title);
  initImGui();

#ifdef _WIN32
  HWND hwnd = glfwGetWin32Window(window);
  int64_t wid = config.mpvWid ? static_cast<uint32_t>((intptr_t)hwnd) : 0;
  mpv = new Mpv(wid);
#else
  mpv = new Mpv();
#endif
  player = new Player(&config, window, mpv, title);
}

Window::~Window() {
  delete player;
  delete mpv;

  exitImGui();
  exitGLFW();
}

bool Window::run(Helpers::OptionParser& parser) {
  mpv->wakeupCb() = [](Mpv* ctx) { glfwPostEmptyEvent(); };
  mpv->updateCb() = [this](Mpv* ctx) {
    if (ctx->wantRender()) requestRender();
  };
  mpv->renderCb() = [this](std::function<void(int, int)> cb) {
#ifdef __APPLE__
    std::lock_guard<std::mutex> lk(contextMutex);
#endif
    glfwMakeContextCurrent(window);
    cb(width, height);
    glfwSwapBuffers(window);
    glfwMakeContextCurrent(nullptr);
  };
  dispatch.wakeup() = []() { glfwPostEmptyEvent(); };

  glfwMakeContextCurrent(window);
  if (!player->init(parser)) return false;
  glfwMakeContextCurrent(nullptr);
#if defined(__APPLE__) && defined(GLFW_PATCHED)
  const char** openedFileNames = glfwGetOpenedFilenames();
  if (openedFileNames != nullptr) {
    int count = 0;
    while (openedFileNames[count] != nullptr) count++;
    player->setDrop(count, openedFileNames);
  }
#endif

  std::atomic_bool shutdown = false;

  std::thread renderThread([&]() {
    while (!glfwWindowShouldClose(window)) {
      {
        std::unique_lock<std::mutex> lk(renderMutex);
        auto timeout = std::chrono::milliseconds(waitTimeout);
        renderCond.wait_for(lk, timeout, [&]() { return wantRender; });
        wantRender = false;
      }
      render();
    }
    shutdown = true;
  });

  while (!shutdown) {
    glfwWaitEvents();
    mpv->runLoop() = false;
    mpv->waitEvent();
    dispatch.process();

    bool hasInputEvents = !ImGui::GetCurrentContext()->InputEventsQueue.empty();
    if ((!glfwGetWindowAttrib(window, GLFW_VISIBLE) || glfwGetWindowAttrib(window, GLFW_ICONIFIED)) &&
        !hasInputEvents && ImGui::GetCurrentContext()->Viewports.Size == 1) {
      waitTimeout = INT_MAX;
      continue;
    }
    double delta = glfwGetTime() - lastRenderAt;
    waitTimeout = hasInputEvents ? std::max(defaultTimeout, (int)delta * 1000) : 1000;
    if (hasInputEvents && (delta > (double)defaultTimeout / 1000)) requestRender();
  }

  renderThread.join();

  return true;
}

void Window::render() {
#ifdef __APPLE__
  std::lock_guard<std::mutex> lk(contextMutex);
#endif
  glfwMakeContextCurrent(window);
  ImGui_ImplOpenGL3_NewFrame();
  ImGui_ImplGlfw_NewFrame();
  ImGui::NewFrame();

  player->draw();
  ImGui::Render();

  glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
  glClear(GL_COLOR_BUFFER_BIT);

  if (player->hasFile() || mpv->forceWindow()) mpv->render(width, height);
  ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
  glfwSwapBuffers(window);
  glfwMakeContextCurrent(nullptr);

  if (ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
    dispatch.sync(
        [](void* data) {
          ImGui::UpdatePlatformWindows();
          ImGui::RenderPlatformWindowsDefault();
        },
        nullptr);
  }
}

void Window::requestRender() {
  std::unique_lock<std::mutex> lk(renderMutex);
  wantRender = true;
  lk.unlock();
  renderCond.notify_one();
  lastRenderAt = glfwGetTime();
}

void Window::initGLFW(const char* title) {
  if (!glfwInit()) {
    std::cout << "Failed to initialize GLFW!" << std::endl;
    std::abort();
  }

#if defined(__APPLE__)
  glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
  glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
  glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
  glfwWindowHint(GLFW_COCOA_RETINA_FRAMEBUFFER, GLFW_FALSE);
#else
  glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
  glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
#endif
  glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);

  GLFWmonitor* monitor = glfwGetPrimaryMonitor();
  const GLFWvidmode* mode = glfwGetVideoMode(monitor);
  width = mode->width * 0.4;
  height = mode->height * 0.4;

  window = glfwCreateWindow(width, height, title, nullptr, nullptr);
  if (window == nullptr) {
    std::cout << "Failed to create window!" << std::endl;
    std::abort();
  }
  glfwSetWindowSizeLimits(window, 640, 480, GLFW_DONT_CARE, GLFW_DONT_CARE);
  glfwSetWindowPos(window, (mode->width - width) / 2, (mode->height - height) / 2);

  glfwSetWindowUserPointer(window, this);
  glfwMakeContextCurrent(window);
  glfwSwapInterval(1);
  glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
  glClear(GL_COLOR_BUFFER_BIT);
  glfwSwapBuffers(window);
  glfwMakeContextCurrent(nullptr);

  glfwSetFramebufferSizeCallback(window, [](GLFWwindow* window, int w, int h) {
    auto win = static_cast<Window*>(glfwGetWindowUserPointer(window));
    win->width = w;
    win->height = h;
    glViewport(0, 0, w, h);
  });
  glfwSetWindowCloseCallback(window, [](GLFWwindow* window) {
    auto win = static_cast<Window*>(glfwGetWindowUserPointer(window));
    win->player->shutdown();
  });
  glfwSetWindowRefreshCallback(window, [](GLFWwindow* window) {
    auto win = static_cast<Window*>(glfwGetWindowUserPointer(window));
    if (win->player->hasFile())
      win->mpv->runLoop() = true;
    else {
      win->requestRender();
      win->dispatch.process();
    }
  });
  glfwSetWindowPosCallback(window, [](GLFWwindow* window, int x, int y) {
    auto win = static_cast<Window*>(glfwGetWindowUserPointer(window));
    if (win->player->hasFile()) win->mpv->runLoop() = true;
  });
  glfwSetCursorPosCallback(window, [](GLFWwindow* window, double x, double y) {
    auto win = static_cast<Window*>(glfwGetWindowUserPointer(window));
    if (ImGui::GetIO().WantCaptureMouse) return;
    win->player->setCursor(x, y);
#ifdef GLFW_PATCHED
    if (win->mpv->allowDrag() && win->height - y > 150 && glfwGetTime() - win->lastMousePressAt > 0.01) {
      if (glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS) glfwDragWindow(window);
    }
#endif
  });
  glfwSetMouseButtonCallback(window, [](GLFWwindow* window, int button, int action, int mods) {
    auto win = static_cast<Window*>(glfwGetWindowUserPointer(window));
    if (button == GLFW_MOUSE_BUTTON_LEFT && action == GLFW_PRESS) win->lastMousePressAt = glfwGetTime();
    if (!ImGui::GetIO().WantCaptureMouse) win->player->setMouse(button, action, mods);
  });
  glfwSetScrollCallback(window, [](GLFWwindow* window, double x, double y) {
    auto win = static_cast<Window*>(glfwGetWindowUserPointer(window));
    if (!ImGui::GetIO().WantCaptureMouse) win->player->setScroll(x, y);
  });
  glfwSetKeyCallback(window, [](GLFWwindow* window, int key, int scancode, int action, int mods) {
    auto win = static_cast<Window*>(glfwGetWindowUserPointer(window));
    if (!ImGui::GetIO().WantCaptureKeyboard) win->player->setKey(key, scancode, action, mods);
  });
  glfwSetDropCallback(window, [](GLFWwindow* window, int count, const char* paths[]) {
    auto win = static_cast<Window*>(glfwGetWindowUserPointer(window));
    if (!ImGui::GetIO().WantCaptureMouse) win->player->setDrop(count, paths);
  });
}

void Window::initImGui() {
  IMGUI_CHECKVERSION();
  ImGui::CreateContext();

  float scale = config.Scale;
  if (scale == 0) {
    glfwGetWindowContentScale(window, &scale, nullptr);
#if defined(__APPLE__)
    scale /= 2.0f;
#endif
  }

  float fontSize = config.FontSize * scale;
  float iconSize = (config.FontSize - 2) * scale;

  ImGuiIO& io = ImGui::GetIO();
  io.IniFilename = nullptr;
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
  io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
  io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;
  io.DisplayFramebufferScale = ImVec2(scale, scale);

  ImGuiStyle& style = ImGui::GetStyle();
  style.ScaleAllSizes(scale);

  ImFontConfig cfg;
  cfg.SizePixels = fontSize;
  io.Fonts->AddFontDefault(&cfg);
  cfg.MergeMode = true;
  ImWchar fontAwesomeRange[] = {ICON_MIN_FA, ICON_MAX_FA, 0};
  const ImWchar* unifontRange = config.buildGlyphRanges();
  io.Fonts->AddFontFromMemoryCompressedTTF(font_awesome_compressed_data, font_awesome_compressed_size, iconSize, &cfg,
                                           fontAwesomeRange);
  if (config.FontPath.empty())
    io.Fonts->AddFontFromMemoryCompressedTTF(unifont_compressed_data, unifont_compressed_size, 0, &cfg, unifontRange);
  else
    io.Fonts->AddFontFromFileTTF(config.FontPath.c_str(), 0, &cfg, unifontRange);
  io.Fonts->Build();

  glfwMakeContextCurrent(window);
  ImGui_ImplGlfw_InitForOpenGL(window, true);
#if defined(__APPLE__)
  ImGui_ImplOpenGL3_Init("#version 150");
#else
  ImGui_ImplOpenGL3_Init("#version 130");
#endif
  glfwMakeContextCurrent(nullptr);
}

void Window::exitGLFW() {
  glfwDestroyWindow(window);
  glfwTerminate();
}

void Window::exitImGui() {
  glfwMakeContextCurrent(window);
  ImGui_ImplOpenGL3_Shutdown();
  ImGui_ImplGlfw_Shutdown();
  ImGui::DestroyContext();
  glfwMakeContextCurrent(nullptr);
}
}  // namespace ImPlay