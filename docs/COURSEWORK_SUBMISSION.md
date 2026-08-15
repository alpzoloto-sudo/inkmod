# inkMOD / InkOS: поэтапная архитектура читалки для Xteink X4

## Цель работы

Перевести чтение EPUB, FB2 и FB2.ZIP на общую архитектуру, не ухудшая
стабильность существующего устройства. Целевая платформа — ESP32-C3 без PSRAM
и с единственным framebuffer 800×480 (48 KB), поэтому модель документа и
страницы не могут загружать книгу целиком в оперативную память.

## Реализованные слои

```text
Legacy hardware / lib/hal
        ↓
src/hal/ReaderHal + LegacyReaderHal
        ↓
src/reader/core/io/IByteReader
        ↓
DocumentCatalog / DocumentModel
        ↓
EPUB and FB2 catalog adapters
        ↓
LayoutEngine → RenderPage → ReaderSession
```

| Требование | Реализация |
| --- | --- |
| Независимый HAL | `src/hal/ReaderHal.h`, `LegacyReaderHal.*` |
| Унифицированный поток байтов | `src/reader/core/io/IByteReader.h`, `FsByteReader`, `MemoryByteReader`, `ZipEntryByteReader` |
| Ленивая модель документа | `src/reader/document/DocumentModel.h` |
| EPUB / FB2 общий каталог | `EpubCatalogAdapter.*`, `Fb2CatalogAdapter.*` |
| Общая пагинация | `src/reader/layout/LayoutEngine.*`, `PageAnchor.h` |
| Команды и исполнитель отрисовки | `src/reader/render/RenderPage.h`, `RenderPageRenderer.*` |
| Сессия чтения и история страниц | `src/reader/session/ReaderSession.*` |
| Прогресс по книге | `src/reader/core/DocumentProgress.*` |
| Фоновые фазы подготовки | `src/reader/core/ReaderJobQueue.*` |
| Память и производительность | `src/reader/core/Diagnostics.h` |

## Управление памятью

Новые core-модули не используют `new`, `malloc`, `std::vector` или
`std::string` в горячем пути. `RenderPage` и история навигации имеют
фиксированную ёмкость; файл и ZIP-данные читаются потоково. Deflate ZIP-entry
не считается случайно доступным: его необходимо извлечь в SD-cache, а не в
RAM. Это предотвращает фрагментацию кучи и перезапуск при открытии большой
книги.

## Совместимость и безопасность внедрения

Новая ветка пока защищена `USE_NEW_READER=0`. Поэтому прошивка продолжает
использовать проверенный `EpubReaderActivity`, а новые интерфейсы компилируются
и тестируются независимо. После прохождения набора EPUB/FB2/FB2.ZIP на X4
флаг позволит включать новый маршрут постепенно.

## Проверка

Подтверждённая команда сборки и прошивки:

```powershell
D:\.platformio\penv\Scripts\pio.exe run -e tiny -t upload
```

Последний успешный лог подтвердил `Hash of data verified` и перезапуск
ESP32-C3. Для host-проверки добавлены тесты `test/reader_core/`:
байтовые reader-ы, ZIP-STORED границы, pagination, bounded history, job queue
и единый расчёт progress.

## Что остаётся следующим этапом

1. Реализовать ленивый content cursor для EPUB/FB2 поверх существующих
   parser-ов.
2. Подключить `ReaderSession` к экрану за feature flag для тестовой группы.
3. Вынести image resolver и versioned SD-cache нового layout.
4. Перевести activity/menu/persistence только после visual и memory acceptance
   на реальном X4.

Такой порядок исключает «big bang» замену ридера и сохраняет работающую
прошивку в каждой промежуточной точке.
