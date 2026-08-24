#pragma once

#include <GfxRenderer.h>
#include <I18n.h>

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
  static constexpr const char* EMPTY_FOLDER_EN[] = {
      "Nothing here. Not even a stray EPUB.",
      "This folder is suspiciously clean.",
      "I even checked under the rug. Nothing.",
  };
  static constexpr const char* EMPTY_FOLDER_RU[] = {
      "Здесь так пусто, что даже EPUB не завалялся.",
      "Папка чистая. Подозрительно чистая.",
      "Я посмотрела даже под ковром. Ничего.",
  };
  static constexpr const char* EMPTY_FOLDER_UK[] = {
      "Тут так порожньо, що навіть EPUB не завалявся.",
      "Папка чиста. Підозріло чиста.",
      "Я навіть під килимком перевірила. Нічого.",
  };

  static constexpr const char* NO_BOOKMARKS_EN[] = {
      "Memory is managing on its own so far.",
      "Nothing marked yet. This book has no flags.",
      "No bookmarks. Apparently everything is memorable.",
  };
  static constexpr const char* NO_BOOKMARKS_RU[] = {
      "Память пока справляется сама.",
      "Ничего не отмечено. Книга пока без флажков.",
      "Закладок нет. Видимо, всё и так запоминается.",
  };
  static constexpr const char* NO_BOOKMARKS_UK[] = {
      "Пам'ять поки справляється сама.",
      "Нічого не позначено. Книга поки без прапорців.",
      "Закладок немає. Мабуть, усе й так запам'ятовується.",
  };

  static constexpr const char* NO_FOOTNOTES_EN[] = {
      "The author has nothing to add today.",
      "No footnotes. Reading without author hints.",
      "No fine print. You can relax.",
  };
  static constexpr const char* NO_FOOTNOTES_RU[] = {
      "Автор решил сегодня ничего не пояснять.",
      "Сносок нет. Читаем без подсказок автора.",
      "Никаких мелких букв. Можно расслабиться.",
  };
  static constexpr const char* NO_FOOTNOTES_UK[] = {
      "Автор сьогодні вирішив нічого не пояснювати.",
      "Виносок немає. Читаємо без підказок автора.",
      "Жодного дрібного шрифту. Можна розслабитися.",
  };

  static constexpr const char* EMPTY_TXT_EN[] = {
      "The file is here. The words seem to be missing.",
      "I opened it. Absolute literary silence.",
      "Zero letters. Very minimalist literature.",
  };
  static constexpr const char* EMPTY_TXT_RU[] = {
      "Файл есть. Слова, похоже, забыли положить.",
      "Я открыла. Там абсолютная литературная тишина.",
      "Ноль букв. Очень минималистичное произведение.",
  };
  static constexpr const char* EMPTY_TXT_UK[] = {
      "Файл є. Слова, схоже, забули покласти.",
      "Я відкрила. Там абсолютна літературна тиша.",
      "Нуль літер. Дуже мінімалістичний твір.",
  };

  static constexpr const char* EMPTY_CATALOG_EN[] = {
      "The shelves are here. The books have not arrived yet.",
      "The catalog looked at me and stayed silent.",
      "Nothing here. Not even something to download.",
  };
  static constexpr const char* EMPTY_CATALOG_RU[] = {
      "Полки есть, книг пока не завезли.",
      "Каталог посмотрел на меня и промолчал.",
      "Тут пусто. Даже скачать нечего.",
  };
  static constexpr const char* EMPTY_CATALOG_UK[] = {
      "Полиці є, книжок поки не завезли.",
      "Каталог подивився на мене й промовчав.",
      "Тут порожньо. Навіть завантажити нічого.",
  };

  static constexpr const char* CACHE_CLEARED_EN[] = {
      "The evidence has been erased.",
      "The cache is gone. It knew too much.",
      "Clean. As if we never read here.",
  };
  static constexpr const char* CACHE_CLEARED_RU[] = {
      "Следы преступления уничтожены.",
      "Кэш ушёл. Он многое знал.",
      "Чисто. Как будто мы здесь и не читали.",
  };
  static constexpr const char* CACHE_CLEARED_UK[] = {
      "Сліди злочину знищено.",
      "Кеш пішов. Він знав надто багато.",
      "Чисто. Наче ми тут і не читали.",
  };

  static constexpr const char* CACHE_FAILED_EN[] = {
      "I tried to clean up. The cache resisted.",
      "Not everything agreed to be deleted.",
      "Cleanup failed. The log knows the details.",
  };
  static constexpr const char* CACHE_FAILED_RU[] = {
      "Я попыталась прибраться. Кэш оказал сопротивление.",
      "Не всё захотело удаляться добровольно.",
      "Уборка сорвалась. Подробности знает лог.",
  };
  static constexpr const char* CACHE_FAILED_UK[] = {
      "Я спробувала прибрати. Кеш чинив опір.",
      "Не все погодилося видалятися добровільно.",
      "Прибирання зірвалося. Подробиці знає лог.",
  };

  static constexpr const char* NO_WIFI_EN[] = {
      "The internet decided to read without us today.",
      "Wi-Fi is hiding. I cannot see it either.",
      "Could not reach the network.",
  };
  static constexpr const char* NO_WIFI_RU[] = {
      "Интернет сегодня решил почитать без нас.",
      "Wi-Fi спрятался. Я его тоже не вижу.",
      "До сети не докричались.",
  };
  static constexpr const char* NO_WIFI_UK[] = {
      "Інтернет сьогодні вирішив почитати без нас.",
      "Wi-Fi сховався. Я його теж не бачу.",
      "До мережі не докричалися.",
  };

  static constexpr const char* CLOCK_FAILED_EN[] = {
      "Time exists. Synchronizing it did not work.",
      "The clock decided to live in its own time zone.",
      "The network and the clock could not agree today.",
  };
  static constexpr const char* CLOCK_FAILED_RU[] = {
      "Время есть. Синхронизировать его не получилось.",
      "Часы решили жить в своём часовом поясе.",
      "Сеть и время сегодня не договорились.",
  };
  static constexpr const char* CLOCK_FAILED_UK[] = {
      "Час є. Синхронізувати його не вдалося.",
      "Годинник вирішив жити у власному часовому поясі.",
      "Мережа й час сьогодні не домовилися.",
  };

  const char* const* values = nullptr;
  size_t count = 0;

#define INKMOD_HUMOR_PICK(base) \
  do { \
    switch (I18N.getLanguage()) { \
      case Language::RU: values = base##_RU; count = std::size(base##_RU); break; \
      case Language::UK: values = base##_UK; count = std::size(base##_UK); break; \
      case Language::EN: default: values = base##_EN; count = std::size(base##_EN); break; \
    } \
  } while (false)

  switch (moment) {
    case Moment::EmptyFolder: INKMOD_HUMOR_PICK(EMPTY_FOLDER); break;
    case Moment::NoBookmarks: INKMOD_HUMOR_PICK(NO_BOOKMARKS); break;
    case Moment::NoFootnotes: INKMOD_HUMOR_PICK(NO_FOOTNOTES); break;
    case Moment::EmptyTextFile: INKMOD_HUMOR_PICK(EMPTY_TXT); break;
    case Moment::EmptyCatalog: INKMOD_HUMOR_PICK(EMPTY_CATALOG); break;
    case Moment::CacheCleared: INKMOD_HUMOR_PICK(CACHE_CLEARED); break;
    case Moment::CacheFailed: INKMOD_HUMOR_PICK(CACHE_FAILED); break;
    case Moment::NoWifi: INKMOD_HUMOR_PICK(NO_WIFI); break;
    case Moment::ClockSyncFailed: INKMOD_HUMOR_PICK(CLOCK_FAILED); break;
  }
#undef INKMOD_HUMOR_PICK

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
