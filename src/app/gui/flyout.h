#pragma once

#include <memory>

namespace ws {
namespace gui {

struct GuiModel;

class Flyout {
public:
    virtual ~Flyout() = default;

    virtual bool create(GuiModel *model) = 0;
    virtual void destroy() = 0;

    virtual void showNearCursor() = 0;
    virtual void hide() = 0;
    void toggle() { visible() ? hide() : showNearCursor(); }

    virtual bool visible() const = 0;

    virtual bool wasActive() const = 0;
    virtual bool hasFocus() const = 0;

    virtual void resetPosition() = 0;

    virtual void pumpEvents() = 0;
    virtual void waitEvents(double timeoutSec) = 0;

    virtual bool renderFrame() = 0;
};

std::unique_ptr<Flyout> createFlyout();

} // namespace gui
} // namespace ws
