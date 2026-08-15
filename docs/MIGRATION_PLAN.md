# План поэтапной миграции reader architecture

Этот план следует [архитектурному аудиту](ARCHITECTURE_AUDIT.md). Цель —
получить самостоятельную архитектуру inkMOD для Xteink X4, не зависящую
архитектурно от CrossPoint, **без остановки развития рабочего ридера**.

## Принципы

1. Один небольшой, собираемый и проверяемый этап за раз.
2. Старый путь остаётся рабочим, пока новый не прошёл hardware acceptance.
3. Не писать whole-book model, ZIP item или изображение в RAM.
4. Не добавлять глобальные singleton для нового reader core.
5. Не менять `freeink-sdk` ради reader refactor: его драйверы уже стоят за
   существующим HAL и должны остаться заменяемой реализацией.
6. Каждое изменение бинарного кэша получает новый version, документацию в
   `docs/file-formats.md` и явный путь invalidation/migration.
7. Каждый перенесённый файл проходит license/provenance review до merge.

## Ресурсный контракт X4

Требование из ТЗ «20 MB RAM для reader core» технически неприменимо к ESP32-C3
без PSRAM. Вместо него применяем измеримый контракт:

| Объект | Правило |
| --- | --- |
| Framebuffer | ровно один, около 48 KB; владелец — display backend |
| Document/index | компактный, lazy, SD-backed; размер RAM зависит от текущей главы, не от книги |
| Layout scratch | заранее ограниченный, освобождается после страницы/чанка; максимальный размер фиксируется измерением в фазе 0 |
| Изображения | decode только при попадании в текущую страницу; screen-sized pixel cache на SD; raw bitmap не удерживать между страницами |
| ZIP | stream/seek или extract-to-cache; не `readFileToMemory()` для произвольного большого item |
| Heap safety | перед тяжёлым действием измерять free/max allocation; при недостатке памяти вернуть recoverable error и очистить временный state |

До фазы 6 запрещено задавать «магические» размеры buffer без профилирования на
X4. Базовые метрики — free heap, minimum free heap, largest allocatable block,
open-file count, duration, число refresh — логируются в debug build.

## Этап 0 — baseline и контрольный набор

**Цель:** получить измеримый эталон до изменений.

* Зафиксировать `tiny` build, размер RAM/flash, cold/warm open, turn-page,
  cache rebuild и min-free heap для EPUB, FB2 и FB2.ZIP.
* Сформировать непубличный/локальный corpus с манифестом: маленький EPUB,
  большой EPUB, EPUB с CSS/таблицами/картинками, большой FB2, FB2.ZIP,
  повреждённый ZIP, кириллица/украинский/RTL, очень длинный токен.
* Использовать существующие открытые fixtures в `test/epubs/` и simulator
  smoke test; не добавлять в Git книги с неясным copyright.
* Ввести таблицу expected results: title/TOC, page progression, image
  placement, bookmarks, resume after sleep, no reboot.

**Готово, когда:** для каждого образца есть лог + фото/скрин страницы и
сохранён cache state. Этот этап не меняет runtime architecture.

## Этап 1 — аудит и freeze границ

**Цель:** зафиксировать стартовую архитектуру и запретить случайный big-bang.

* Результаты: `ARCHITECTURE_AUDIT.md` и этот план.
* Явно считать `lib/Epub/Epub/Page.*`, `Section.*`, `ParsedText.*` legacy
  mixed layer до его разрезания; не добавлять туда новые форматы.
* Зафиксировать feature flag `USE_NEW_READER=0` (ещё без переключения в UI).
* Создать `THIRD_PARTY.md` в отдельном документационном PR после проверки
  лицензий `freeink-sdk`, Xml/ZIP/decoder/font зависимостей.

**Готово, когда:** согласован список владельцев/границ и rollback-путь.

## Этап 2 — совместимый HAL/facade

**Цель:** предоставить новые интерфейсы, не трогая стабильные драйверы.

Предлагаемое дерево:

```text
src/hal/
  Display.h          // frame lifecycle, orientation, refresh request
  Storage.h          // open reader/writer, stat, atomic rename
  Input.h            // logical buttons/events
  Power.h            // sleep request + wake reason
  Clock.h
  impl/LegacyHal*.cpp // thin delegation to lib/hal/Hal*
```

* `Display` не раскрывает framebuffer в document/layout слоям.
* `Storage` возвращает RAII handle и фиксирует правило одного активного reader.
* `Input` использует логические `MappedInputManager::Button`, а не номера GPIO.
* Создать contract tests на simulator с fake backend; production implementation
  только делегирует текущим `HalDisplay`, `Storage`, `gpio`, `powerManager`.

**Не делать:** не переписывать `lib/hal`, не менять boot/sleep sequencing,
не менять refresh waveform.

**Готово, когда:** `tiny` и simulator собираются, а existing UI работает через
старый код без изменений поведения.

## Этап 3 — Core, Storage и потоковое чтение

**Цель:** дать parser-ам нейтральный источник байтов.

* Переместить/адаптировать существующий `lib/Fb2/native/IByteReader.h` в
  `src/core/io/IByteReader.h`; старый путь временно включает adapter header.
* Реализовать `FsByteReader`, `CacheByteReader`, `ZipEntryByteReader`.
* `ZipEntryByteReader` обязан работать чанками и явно сообщать, поддерживает
  ли seek. Для deflate с дорогим backward seek используется extract-to-cache,
  а не скрытая повторная распаковка в RAM.
* Ввести `BookId` (path + stable signature) и `CacheKey`; все cache paths
  строятся в одном месте.
* Atomic cache write: temp file → close/sync → rename; прерванная операция
  очищается при следующем открытии.

**Готово, когда:** unit tests читают одинаковые данные из file/cache/ZIP,
а `USE_NEW_READER=0` продолжает открывать существующие EPUB/FB2.

### Состояние в текущей ветке

Уже добавлены `IByteReader` с 64-битными offsets, `FsByteReader`,
`MemoryByteReader` и reader для ZIP_STORED entry. Deflate-entry намеренно не
притворяется seekable: будущий FB2.ZIP путь обязан извлечь XML в SD cache.
Также добавлены allocation-free `MemoryStats` и `ScopedPerfTimer`. Новый путь
ещё не включён в UI, поэтому legacy reader остаётся функциональным default.

## Этап 4 — lazy DocumentModel и format adapters

**Цель:** общая модель документа без render/UI зависимостей.

```text
DocumentModel
  metadata() / toc()
  openChapter(ChapterId) -> ChapterSource
  resolve(ResourceRef) -> ByteReader
  estimateProgress(anchor)
```

Модель хранит метаданные, TOC, spine/sections и ссылки на ресурсы, но не DOM
всей книги и не bitmap. В ней нет `GfxRenderer`, `Activity`, `WiFi`, `String`
для whole-book text или `std::function` в hot path.

Порядок adapters:

1. `EpubAdapter`: сначала metadata/TOC/resource lookup поверх текущего
   `Epub`/`ZipFile`, без замены HTML layout.
2. `Fb2Adapter`: использовать существующий scan/index и `IByteReader`;
   глава отдаётся lazy.
3. `Fb2ZipAdapter`: распаковать исходный FB2 только в cache, показать
   progress по фазам `проверка → распаковка → индекс`; не формировать пакет
   всей книги в RAM.

**Готово, когда:** один и тот же inspector выводит title/author/TOC для EPUB,
FB2 и FB2.ZIP, а проверка не рисует страницу и не держит file handle после
возврата.

## Этап 5 — layout engine и virtual pages

**Цель:** получить format-neutral pagination.

* Ввести `LayoutConstraints` (usable rect, font metric provider, paragraph
  policy, hyphenation, image policy) и `TextMeasurer`.
* Из parser-а выходят semantic blocks: text, heading, image reference,
  horizontal rule, table fragment, anchor. Parser не решает пиксели.
* Layout возвращает `PageAnchor` и фиксированно ограниченный список
  `RenderCommand`; это не DOM и не постоянный `vector` всех страниц книги.
* `PageAnchor` минимум содержит `BookId`, chapter/spine id, source offset or
  semantic block id, intra-block position и version layout spec. Он является
  ключом resume/bookmark/percent, а не только номером page.
* Пагинация выполняется чанками. В памяти допустима текущая страница и малое
  окно соседних страниц; anchors/cache остаются на SD.

**Совместимость:** существующий `section.bin` version 40 продолжает
обслуживаться legacy reader. Новый engine пишет в отдельный versioned namespace
до финального migration tool.

**Готово, когда:** на одном EPUB новый engine строит anchors и visual diff
с baseline не содержит обрезанного/вышедшего за экран текста.

## Этап 6 — renderer, изображения и кэш

**Цель:** renderer исполняет commands, но не читает EPUB/FB2.

* Создать `RenderCommand` для текста, линии, прямоугольника, image placement,
  table fragment и background.
* `EinkRenderer` — единственный слой с `GfxRenderer`/framebuffer; он получает
  `Display` facade, preselected font и command buffer.
* Image resolver сначала проверяет screen-sized pixel cache, затем декодирует
  source потоково. На ошибке command пропускается с диагностикой, страница не
  падает.
* Использовать LRU на SD для raw FB2 images и pixel cache. Eviction выполняет
  storage service вне render critical section.
* Добавить thumbnails/cover policy отдельно от content image policy.

**Готово, когда:** изображения не уменьшаются произвольно, не пересекают текст,
а отказ decode не вызывает reboot/OOM.

## Этап 7 — Reader UI и persistence

**Цель:** activity становится координатором, а не владельцем engine internals.

* `ReaderSession` получает `DocumentModel`, `LayoutEngine`, `EinkRenderer`,
  `ReaderPersistence`; `EpubReaderActivity` адаптирует кнопки/меню/статус-bar.
* Реализовать `ReaderState`: page counter mode, orientation, font/layout spec,
  anchor, progress, auto-page-turn, timer state. Сохранять debounce-ом.
* При wake восстановить сохранённый counter mode и anchor; не подменять
  «по книге» на «по главе».
* Закладки, footnotes, KOReader sync и stats мигрировать adapter-ами после
  базового forward/back/resume.

**Готово, когда:** открыть → листать → сон → wake → продолжить сохраняет
визуальную позицию и выбранный режим счётчика на X4.

## Этап 8 — Library/UI и постепенное включение

**Цель:** library видит единые метаданные, новый reader включается безопасно.

* File browser/search/recent books обращаются к lightweight metadata index,
  а не к полному parse каждой книги при каждом вводе символа.
* Новый reader включается только для одного формата и тестовой группы через
  compile-time/runtime flag; default остаётся legacy.
* Добавить «очистить кэш нового reader» отдельно от пользовательских settings.
* Собрать telemetry только в debug logs: fallback count, cache hit, OOM guard,
  failed image, restore failure.

**Готово, когда:** быстрый поиск не делает metadata parse, а fallback открывает
книгу legacy reader при recoverable ошибке нового пути.

## Этап 9 — retirement legacy пути

Удаление старого пути разрешено только после двух релизных циклов, когда:

* EPUB, FB2, FB2.ZIP прошли corpus и hardware matrix;
* cache migration и rollback проверены;
* нет известного OOM/reboot регресса;
* licenses внесены в `THIRD_PARTY.md`;
* все feature flags удалены отдельным PR;
* `docs/file-formats.md`, CHANGELOG и troubleshooting обновлены.

## Формат каждого PR

Каждый PR должен содержать:

1. один слой или одну связь между слоями;
2. список изменённых public interfaces;
3. heap/stack rationale для новых allocation;
4. тесты и команду проверки;
5. hardware checklist: открыть обычную/тяжёлую книгу, листать, сон/wake,
   проверить SD errors и `Min Free` в serial log;
6. fallback/rollback: какой feature flag или cache namespace вернуть.

Минимальные gate-команды:

```powershell
D:\.platformio\penv\Scripts\pio.exe run -e tiny
D:\.platformio\penv\Scripts\pio.exe run -e simulator
python .\scripts\run_simulator_smoke_test.py
```

Simulator не заменяет X4: отдельно проверяются память, SD contention, refresh,
ghosting, deep sleep и реальные кнопки.

## Первые три практических PR

| PR | Изменение | Что намеренно не меняется |
| --- | --- | --- |
| `docs: audit reader architecture` | этот аудит, migration plan, corpus manifest template | runtime |
| `refactor: add reader core IO contracts` | `IByteReader`/cache identity interfaces + host tests | ReaderActivity, cache writer, renderer |
| `refactor: add legacy-backed document metadata adapter` | EPUB metadata/TOC adapter только для diagnostic screen/test | pagination, image handling, UI default |

Такой порядок начинает переход уже сейчас, но не создаёт «полу-новый» reader,
который нельзя стабильно протестировать на ESP32-C3.
