# Реестр снятия архитектурных зависимостей

Этот реестр отличает «новый интерфейс существует» от «старый компонент уже
можно удалить». Статус меняется только после corpus + X4 hardware проверки.

| Модуль | Текущий backend | Новый контракт | Статус |
| --- | --- | --- | --- |
| Display | `lib/hal/HalDisplay` → `EInkDisplay` | `src/hal/ReaderHal.h` | facade added; driver retained |
| Storage | `lib/hal/HalStorage` | `src/hal/ReaderHal.h`, `IByteReader` | facade/core contract added |
| Input | `MappedInputManager` | `reader::hal::Input` | interface only |
| Power/sleep | `HalPowerManager`/`HalGPIO` | `reader::hal::Power` | interface only |
| FB2 stream | legacy `native/IByteReader` | `src/reader/core/io/IByteReader.h` | migrated compatibility header |
| EPUB parser | `lib/Epub` | `DocumentCatalog` via `EpubCatalogAdapter` | metadata/spine adapter added; lazy content cursor pending |
| FB2 parser | `lib/Fb2/native` | `DocumentCatalog` via `Fb2CatalogAdapter` | metadata/section-size adapter added; lazy content cursor pending |
| Layout | `Epub::Section` + `Page` | `PageAnchor` + `RenderPage` | pending implementation |
| Reader UI | `EpubReaderActivity` | `ReaderSession` | session/layout coordinator added; activity migration pending |

`USE_NEW_READER=0` remains the release default until EPUB, FB2 and FB2.ZIP
pass the baseline corpus on X4.
