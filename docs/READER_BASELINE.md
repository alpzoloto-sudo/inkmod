# Базовая проверка reader перед миграцией

Этот документ фиксирует процедуру этапа 0 из
[MIGRATION_PLAN.md](MIGRATION_PLAN.md). Он не добавляет книги в репозиторий и
не меняет прошивку.

## Обязательные образцы

| ID | Формат и источник | Что проверяет |
| --- | --- | --- |
| `epub-small` | `test/epubs/test_reader_rendering_matrix.epub` | обычный EPUB, heading, paragraph, CSS, быстрый повторный вход |
| `epub-images` | `test/epubs/test_mixed_images.epub` | PNG/JPEG, placement, cache изображения, full-width image |
| `epub-tables` | `test/epubs/test_tables.epub` | таблицы, переносы и page boundaries |
| `epub-rtl` | `test/language/RTL/RTL_test.epub` | bidi/RTL и fallback glyphs |
| `fb2-large-local` | локальная большая FB2, не добавляется в Git | scan/index, lazy chapter rendering, OOM guard |
| `fb2-zip-local` | локальная большая FB2.ZIP, не добавляется в Git | ZIP detection, extract-to-cache, progress и повторный вход |
| `zip-corrupt-local` | специально повреждённый ZIP вне Git | recoverable error, отсутствие reboot и cleanup temp cache |

Для локальных файлов в отчёт записывают только размер, SHA-256 (по желанию) и
feature profile, но не путь с персональными данными и не сам файл.

## Порядок проверки X4

1. Очистить cache только целевой книги в `/.inkmod/epub_<hash>/`; не удалять
   `wifi.json` и `inkmod-settings.json`.
2. Перезапустить устройство, открыть книгу первый раз и записать время до
   первого отображённого текста.
3. Пролистать минимум 10 страниц, в том числе через изображение/таблицу.
4. Выйти в библиотеку, снова открыть книгу и записать warm-open time.
5. Открыть reader menu, изменить один layout option, вернуться к книге.
6. Усыпить устройство, разбудить, проверить anchor, режим счётчика страниц и
   отсутствие полос/наложений status bar.
7. После тяжёлых FB2 и FB2.ZIP открыть ещё одну книгу и повторно открыть
   тяжёлую: не допускаются reboot и сообщение OOM без возврата в библиотеку.

## Что записывать

| Метрика | Источник | Критерий |
| --- | --- | --- |
| cold/warm open | секундомер или serial timestamp | есть сравнимое значение для последующих PR |
| `Free`, `Min Free`, `MaxAlloc` | строки `[MEM]` serial log | `Min Free` и `MaxAlloc` не деградируют от страницы к странице |
| open/cache phases | UI и `[FB2]`/`[EPS]`/`[BMC]` log | пользователь видит, что операция идёт; завершение/ошибка однозначны |
| render correctness | фото экрана | текст в safe area, изображения не перекрывают текст |
| resume/progress | UI после sleep/wake | position и выбранный counter mode сохранены |
| recovery | serial + UI | ошибка файла возвращает в управляемый экран, не вызывает restart |

Шаблон одной строки отчёта:

```text
2026-08-08 | tiny | fb2-zip-local | 48.5 MB | cold 00:00 | warm 00:00 |
free/min/max: ... | pages: 10 | sleep: pass | images: n/a | result: pass/fail | log: <local name>
```

## Автоматические проверки

Перед аппаратной проверкой выполнить:

```powershell
D:\.platformio\penv\Scripts\pio.exe run -e tiny
D:\.platformio\penv\Scripts\pio.exe run -e simulator
python .\scripts\run_simulator_smoke_test.py
```

Simulator подтверждает entry/render path, но не подтверждает SD contention,
heap fragmentation, e-ink refresh, сон и реальные кнопки. Эти пункты остаются
обязательными на X4.

