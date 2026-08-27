#include "RecentBooksActivity.h"

#include <Arduino.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>

#include <algorithm>
#include <memory>

#include "BookActions.h"
#include "FileBrowserActionActivity.h"
#include "MappedInputManager.h"
#include "RecentBooksStore.h"
#include "activities/util/ConfirmationActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr size_t MAX_LIST_RECENT_BOOKS = 10;
constexpr unsigned long LONG_PRESS_MS = 1000;
}  // namespace

void RecentBooksActivity::loadRecentBooks() {
  recentBooks.clear();
  const auto& books = RECENT_BOOKS.getBooks();
  recentBooks.reserve(std::min(books.size(), MAX_LIST_RECENT_BOOKS));

  // onEnter() prunes missing paths immediately before this initial load, and
  // every in-screen mutation keeps the persistent store in sync. Rechecking
  // Storage.exists() here used to duplicate up to ten FAT directory lookups on
  // every entry into Recent Books.
  for (const auto& book : books) {
    if (recentBooks.size() >= MAX_LIST_RECENT_BOOKS) break;
    recentBooks.push_back(book);
  }
}

void RecentBooksActivity::onEnter() {
  Activity::onEnter();

  if (RECENT_BOOKS.pruneMissing()) {
    RECENT_BOOKS.saveToFile();
  }

  loadRecentBooks();
  selectorIndex = 0;
  requestUpdate();
}

void RecentBooksActivity::onExit() {
  Activity::onExit();
  recentBooks.clear();
}

void RecentBooksActivity::loop() {

  if (longPressFired) {
    if (!mappedInput.isPressed(MappedInputManager::Button::Confirm)) {
      longPressFired = false;
    }
    return;
  }

  if (!recentBooks.empty() && selectorIndex < recentBooks.size() &&
      mappedInput.isPressed(MappedInputManager::Button::Confirm) && mappedInput.getHeldTime() >= LONG_PRESS_MS) {
    longPressFired = true;
    showBookActionMenu(selectorIndex, true);
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (!recentBooks.empty() && selectorIndex < static_cast<int>(recentBooks.size())) {
      LOG_DBG("RBA", "Selected recent book: %s", recentBooks[selectorIndex].path.c_str());
      onSelectBook(recentBooks[selectorIndex].path);
      return;
    }
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    onGoHome();
  }

  int listSize = static_cast<int>(recentBooks.size());

  buttonNavigator.onNextRelease([this, listSize] {
    selectorIndex = ButtonNavigator::nextIndex(static_cast<int>(selectorIndex), listSize);
    requestUpdate();
  });

  buttonNavigator.onPreviousRelease([this, listSize] {
    selectorIndex = ButtonNavigator::previousIndex(static_cast<int>(selectorIndex), listSize);
    requestUpdate();
  });

  buttonNavigator.onNextContinuous([this, listSize] {
    selectorIndex = ButtonNavigator::nextIndex(static_cast<int>(selectorIndex), listSize);
    requestUpdate();
  });

  buttonNavigator.onPreviousContinuous([this, listSize] {
    selectorIndex = ButtonNavigator::previousIndex(static_cast<int>(selectorIndex), listSize);
    requestUpdate();
  });
}

void RecentBooksActivity::reloadAfterBookAction() {
  loadRecentBooks();
  if (recentBooks.empty()) {
    selectorIndex = 0;
  } else if (selectorIndex >= recentBooks.size()) {
    selectorIndex = recentBooks.size() - 1;
  }
  requestUpdate(true);
}

void RecentBooksActivity::promptDeleteBook(const RecentBook& book) {
  const std::string path = book.path;
  auto handler = [this, path](const ActivityResult& res) {
    if (res.isCancelled) {
      LOG_DBG("RBA", "Delete cancelled");
      return;
    }

    LOG_DBG("RBA", "Attempting to delete: %s", path.c_str());
    BookActions::clearFileMetadata(path);
    if (!Storage.remove(path.c_str())) {
      LOG_ERR("RBA", "Failed to delete file: %s", path.c_str());
      return;
    }

    RECENT_BOOKS.removeByPath(path);
    reloadAfterBookAction();
  };

  const std::string heading = tr(STR_DELETE) + std::string("? ");
  startActivityForResult(std::make_unique<ConfirmationActivity>(renderer, mappedInput, heading, book.title),
                         std::move(handler));
}

void RecentBooksActivity::promptRemoveBook(const std::string& path, const std::string& title) {
  auto handler = [this, path](const ActivityResult& res) {
    if (res.isCancelled) {
      LOG_DBG("RBA", "Remove from recents cancelled");
      return;
    }
    if (RECENT_BOOKS.removeByPath(path)) {
      LOG_DBG("RBA", "Removed from recents: %s", path.c_str());
      reloadAfterBookAction();
    }
  };

  startActivityForResult(
      std::make_unique<ConfirmationActivity>(renderer, mappedInput, tr(STR_REMOVE_FROM_RECENTS), title,
                                             /*ignoreInitialConfirmRelease=*/false),
      std::move(handler));
}

void RecentBooksActivity::showBookActionMenu(const size_t bookIndex, const bool ignoreInitialConfirmRelease) {
  if (bookIndex >= recentBooks.size()) return;

  const RecentBook book = recentBooks[bookIndex];
  std::vector<FileBrowserActionActivity::MenuItem> items =
      BookActions::buildBookActionItems(book.path, /*includeRemoveFromRecents=*/true);

  startActivityForResult(
      std::make_unique<FileBrowserActionActivity>(renderer, mappedInput, book.title, std::move(items),
                                                  ignoreInitialConfirmRelease),
      [this, book](const ActivityResult& result) {
        if (result.isCancelled) return;

        const auto* actionResult = std::get_if<FileBrowserActionResult>(&result.data);
        if (!actionResult) {
          LOG_ERR("RBA", "Book action result missing");
          return;
        }

        switch (static_cast<FileBrowserAction>(actionResult->action)) {
          case FileBrowserAction::Delete:
            promptDeleteBook(book);
            return;
          case FileBrowserAction::DeleteCache:
            startActivityForResult(
                std::make_unique<ConfirmationActivity>(
                    renderer, mappedInput, BookActions::confirmationHeading(StrId::STR_DELETE_CACHE), book.title),
                [this, book](const ActivityResult& confirmation) {
                  if (!confirmation.isCancelled) {
                    if (!BookActions::clearBookCache(book.path)) {
                      LOG_ERR("RBA", "Failed to clear book cache for: %s", book.path.c_str());
                    } else {
                      BookActions::drawToast(renderer, tr(STR_BOOK_CACHE_DELETED));
                      delay(1000);
                    }
                  }
                  reloadAfterBookAction();
                });
            return;
          case FileBrowserAction::DeleteStats:
            startActivityForResult(
                std::make_unique<ConfirmationActivity>(
                    renderer, mappedInput, BookActions::confirmationHeading(StrId::STR_DELETE_BOOK_STATS), book.title),
                [this, book](const ActivityResult& confirmation) {
                  if (!confirmation.isCancelled) {
                    if (!BookActions::deleteBookStats(book.path)) {
                      LOG_ERR("RBA", "Failed to delete book stats for: %s", book.path.c_str());
                    } else {
                      BookActions::drawToast(renderer, tr(STR_BOOK_STATS_DELETED));
                      delay(1000);
                    }
                  }
                  reloadAfterBookAction();
                });
            return;
          case FileBrowserAction::ToggleCompleted: {
            bool completed = false;
            if (BookActions::toggleEpubCompleted(book.path, book.title, completed)) {
              BookActions::drawToast(renderer, completed ? tr(STR_MARKED_FINISHED) : tr(STR_MARKED_UNFINISHED));
              delay(1000);
            }
            reloadAfterBookAction();
            return;
          }
          case FileBrowserAction::RemoveFromRecents:
            promptRemoveBook(book.path, book.title);
            return;
          case FileBrowserAction::PinFavorite:
          case FileBrowserAction::UnpinFavorite:
          case FileBrowserAction::SetSleepFolder:
          case FileBrowserAction::ClearSleepFolder:
          case FileBrowserAction::SetSleepOverlay:
          case FileBrowserAction::SetTimeoutSleepOverlay:
          case FileBrowserAction::Rename:
          case FileBrowserAction::BookInfo:
            return;
        }
      });
}

void RecentBooksActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto safeArea = UITheme::getInstance().getScreenSafeArea(renderer, true, false);

  GUI.drawHeader(renderer, Rect{safeArea.x, metrics.topPadding, safeArea.width, metrics.headerHeight},
                 tr(STR_MENU_RECENT_BOOKS));

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = safeArea.y + safeArea.height - contentTop - metrics.verticalSpacing;

  if (recentBooks.empty()) {
    renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, contentTop + 20, tr(STR_NO_RECENT_BOOKS));
  } else {
    GUI.drawList(
        renderer, Rect{safeArea.x, contentTop, safeArea.width, contentHeight}, recentBooks.size(), selectorIndex,
        [this](int index) { return recentBooks[index].title; }, [this](int index) { return recentBooks[index].author; },
        [this](int index) { return UITheme::getFileIcon(recentBooks[index].path); });
  }

  const auto labels = mappedInput.mapLabels(tr(STR_HOME), tr(STR_OPEN), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}