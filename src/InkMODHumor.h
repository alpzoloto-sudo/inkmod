#pragma once

#include <GfxRenderer.h>

#include <algorithm>
#include <cstdint>
#include <iterator>

namespace InkMODHumor {

enum class Moment : uint8_t {
  EmptyFolder,
  NoBookmarks,
  NoFootnotes,
  EmptyTextFile,
  EmptyCatalog,
  CacheCleared,
  CacheFailed,
  NoWifi,
  ClockSyncFailed,
};

inline const char* phrase(const Moment moment, const uint32_t seed = 0) {
  static constexpr const char* EMPTY_FOLDER[] = {
      "Здесь так пусто, что даже EPUB не завалялся.",
      "Папка чистая. Подозрительно чистая.",
      "Я посмотрела даже под ковром. Ничего.",
  };
  static constexpr const char* NO_BOOKMARKS[] = {
      "Память пока справляется сама.",
      "Ничего не отмечено. Книга пока без флажков.",
      "Закладок нет. Видимо, всё и так запоминается.",
  };
  static constexpr const char* NO_FOOTNOTES[] = {
      "Автор решил сегодня ничего не пояснять.",
      "Сносок нет. Читаем без подсказок автора.",
      "Никаких мелких букв. Можно расслабиться.",
  };
  static constexpr const char* EMPTY_TXT[] = {
      "Файл есть. Слова, похоже, забыли положить.",
      "Я открыла. Там абсолютная литературная тишина.",
      "Ноль букв. Очень минималистичное произведение.",
  };
  static constexpr const char* EMPTY_CATALOG[] = {
      "Полки есть, книг пока не завезли.",
      "Каталог посмотрел на меня и промолчал.",
      "Тут пусто. Даже скачать нечего.",
  };
  static constexpr const char* CACHE_CLEARED[] = {
      "Следы преступления уничтожены.",
      "Кэш ушёл. Он многое знал.",
      "Чисто. Как будто мы здесь и не читали.",
  };
  static constexpr const char* CACHE_FAILED[] = {
      "Я попыталась прибраться. Кэш оказал сопротивление.",
      "Не всё захотело удаляться добровольно.",
      "Уборка сорвалась. Подробности знает лог.",
  };
  static constexpr const char* NO_WIFI[] = {
      "Интернет сегодня решил почитать без нас.",
      "Wi-Fi спрятался. Я его тоже не вижу.",
      "До сети не докричались.",
  };
  static constexpr const char* CLOCK_FAILED[] = {
      "Время есть. Синхронизировать его не получилось.",
      "Часы решили жить в своём часовом поясе.",
      "Сеть и время сегодня не договорились.",
  };

  const char* const* values = nullptr;
  size_t count = 0;
  switch (moment) {
    case Moment::EmptyFolder: values = EMPTY_FOLDER; count = std::size(EMPTY_FOLDER); break;
    case Moment::NoBookmarks: values = NO_BOOKMARKS; count = std::size(NO_BOOKMARKS); break;
    case Moment::NoFootnotes: values = NO_FOOTNOTES; count = std::size(NO_FOOTNOTES); break;
    case Moment::EmptyTextFile: values = EMPTY_TXT; count = std::size(EMPTY_TXT); break;
    case Moment::EmptyCatalog: values = EMPTY_CATALOG; count = std::size(EMPTY_CATALOG); break;
    case Moment::CacheCleared: values = CACHE_CLEARED; count = std::size(CACHE_CLEARED); break;
    case Moment::CacheFailed: values = CACHE_FAILED; count = std::size(CACHE_FAILED); break;
    case Moment::NoWifi: values = NO_WIFI; count = std::size(NO_WIFI); break;
    case Moment::ClockSyncFailed: values = CLOCK_FAILED; count = std::size(CLOCK_FAILED); break;
  }
  return count == 0 ? "" : values[seed % count];
}

// Hard-bounded renderer. It never draws a line unless it fits inside the
// supplied rectangle. wrappedText() also truncates the final permitted line.
inline void drawBounded(GfxRenderer& renderer, const Moment moment, const int x, const int y, const int width,
                        const int height, const int fontId, const bool black = true, const uint32_t seed = 0,
                        const EpdFontFamily::Style style = EpdFontFamily::REGULAR) {
  constexpr int SIDE_INSET = 12;
  constexpr int LINE_GAP = 3;

  if (width <= SIDE_INSET * 2 || height <= 0) return;

  const int lineHeight = std::max(1, renderer.getLineHeight(fontId));
  const int maxLines = (height + LINE_GAP) / (lineHeight + LINE_GAP);
  if (maxLines <= 0) return;

  const int usableWidth = width - SIDE_INSET * 2;
  const auto lines = renderer.wrappedText(fontId, phrase(moment, seed), usableWidth, maxLines, style);
  if (lines.empty()) return;

  const int blockHeight = static_cast<int>(lines.size()) * lineHeight +
                          (static_cast<int>(lines.size()) - 1) * LINE_GAP;
  int lineY = y + std::max(0, (height - blockHeight) / 2);

  for (const auto& line : lines) {
    if (lineY + lineHeight > y + height) break;
    const int textWidth = renderer.getTextWidth(fontId, line.c_str(), style);
    const int lineX = x + std::max(SIDE_INSET, (width - textWidth) / 2);
    renderer.drawText(fontId, lineX, lineY, line.c_str(), black, style);
    lineY += lineHeight + LINE_GAP;
  }
}

}  // namespace InkMODHumor
