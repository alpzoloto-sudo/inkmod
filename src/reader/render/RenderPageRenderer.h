// Executes format-neutral page commands. A hardware adapter implements
// RenderExecutor; pagination never reaches into GfxRenderer directly.
#pragma once

#include "RenderPage.h"

namespace reader {

class RenderExecutor {
 public:
  virtual ~RenderExecutor() = default;
  virtual void drawText(const RenderRect& rect, StringRef text, const TextStyle& style) = 0;
  virtual void drawImage(const RenderRect& rect, ResourceId resource) = 0;
  virtual void drawLine(const RenderRect& rect) = 0;
  virtual void fillRect(const RenderRect& rect) = 0;
};

class RenderPageRenderer {
 public:
  static void render(const RenderPage& page, RenderExecutor& executor);
};

}  // namespace reader
