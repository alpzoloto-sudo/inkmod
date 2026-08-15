# InkMOD regression corpus

Keep the actual copyrighted test books outside the repository. Before a public
release, run `scripts/book_corpus_check.py` against your local stress corpus and
then verify the same files on a real X3/X4.

Recommended cases:

- EPUB with one XHTML/HTML member >= 1 MiB.
- EPUB with 2,000+ spine items / very large TOC.
- EPUB with many PNG/JPEG illustrations and embedded fonts.
- FB2 >= 40 MiB with hundreds of sections.
- FB2.ZIP whose uncompressed FB2 is several MiB or larger.
- FB2 with many `<binary>` images.
- XTC/XTCH 2-bit grayscale with rapid page turns.
- TXT/Markdown with long UTF-8 names and Cyrillic text.

A release is not considered hardware-verified until it opens, pages through,
sleeps/wakes, and reopens the large EPUB and large FB2 cases without a reset.
