# Optimization checkpoint

This branch keeps the proven release reader path while reducing first-open, cache and low-memory overhead on Xteink X4.

Final pass highlights:

- FB2 and FB2.ZIP release scans bypass developer-only per-I/O profiling.
- FB2.ZIP DEFLATE allocation failures are checked and staging adapts to contiguous heap.
- FB2 XML entity parsing and section-id indexing avoid temporary strings.
- Styled FB2 text writes XHTML wrappers directly instead of allocating tag strings per run.
- Late FB2 chapter/image lookups skip persisted records without materializing earlier ids/titles.
- Logical FB2 virtual-chapter bounds use one section-index pass.
- FB2 OPF/NCX and image-index generation stream numeric ids/hrefs directly instead of building thousands of temporary concatenated strings.
- EPUB/FB2 `BookMetadataCache` skips temporary spine/TOC records without deserializing strings when only offsets or spine indexes are needed.
- Existing bounded ZIP read-ahead, page-cache spooling, lazy FB2 images, SD-font survival and Quick Resume behavior remain unchanged.

This checkpoint must pass host tests plus both PlatformIO `developer` and `release` builds before hardware validation.
