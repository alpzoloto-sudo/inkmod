#pragma once

#include <string>
#include "activities/Activity.h"

class BookInfoActivity final : public Activity {
 public:
  BookInfoActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string path)
      : Activity("BookInfo", renderer, mappedInput), path_(std::move(path)) {}
  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;
 private:
  std::string path_;
};
