#include "RenderPageRenderer.h"

namespace reader {

void RenderPageRenderer::render(const RenderPage& page, RenderExecutor& executor) {
  for (uint16_t index = 0; index < page.count(); ++index) {
    const RenderCommand& command = page[index];
    switch (command.type) {
      case RenderCommandType::DrawText:
        executor.drawText(command.rect, command.text, command.style);
        break;
      case RenderCommandType::DrawImage:
        executor.drawImage(command.rect, command.resource);
        break;
      case RenderCommandType::DrawLine:
        executor.drawLine(command.rect);
        break;
      case RenderCommandType::FillRect:
        executor.fillRect(command.rect);
        break;
    }
  }
}

}  // namespace reader
