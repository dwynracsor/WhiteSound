#pragma once

#include <memory>

namespace ws {
namespace gui {

struct GuiModel;

// A borderless, always-on-top window that draws the flyout with ImGui.
//
// The host owns the rendering backend and the window's own event handling:
// moving by dragging the header, hiding on focus loss and on Esc, remembering a
// dragged position. The shell only drives it, which keeps the same shell code
// working for every backend:
//
//   auto flyout = ws::gui::createFlyout();
//   flyout->create(&model);
//   while (running) {
//       flyout->pumpEvents();                       // service all events
//       if (flyout->visible()) {
//           syncModel(model);
//           const bool changed = flyout->renderFrame();
//           ...consume model.requestHide / requestQuit...
//       } else {
//           flyout->waitEvents(0.05);               // stay idle
//       }
//   }
//
// Exactly one backend is compiled in (GLFW + OpenGL3 by default, or the native
// Win32 + DirectX 11 one); createFlyout() is defined by it.
class Flyout {
public:
    virtual ~Flyout() = default;

    // Create the window and its rendering resources. `model` must outlive the
    // host. Returns false if no window/renderer could be created; the shell then
    // falls back to a tray-only mode.
    virtual bool create(GuiModel *model) = 0;
    // Release the window and its renderer resources. Idempotent: the shell calls
    // it once at shutdown and the destructor calls it again, so a second call
    // must be a harmless no-op.
    virtual void destroy() = 0;

    // Show the panel anchored to the cursor (or to the last dragged position).
    virtual void showNearCursor() = 0;
    virtual void hide() = 0;
    void toggle() { visible() ? hide() : showNearCursor(); }

    virtual bool visible() const = 0;

    // The panel has held focus at least once since it was last shown, usedwith
    // hasFocus() by the shell as a safety net to close it when focus is lost.
    virtual bool wasActive() const = 0;
    virtual bool hasFocus() const = 0;

    // Forget a dragged position so the next show() goes back to the cursor.
    virtual void resetPosition() = 0;

    // Drain the thread's event queue without blocking. Dispatches events for the
    // flyout window and for any other window the shell created on this thread
    // (the tray window).
    virtual void pumpEvents() = 0;
    // Block up to `timeoutSec` seconds waiting for an event (used while hidden).
    virtual void waitEvents(double timeoutSec) = 0;

    // Renders a single frame. Returns true if the model changed.
    virtual bool renderFrame() = 0;
};

// Create the flyout host for the compiled backend.
std::unique_ptr<Flyout> createFlyout();

} // namespace gui
} // namespace ws
