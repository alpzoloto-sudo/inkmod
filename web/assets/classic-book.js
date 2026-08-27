// inkMOD Classic Book — shared browser-side normalization for EPUB optimization
// and FB2 -> EPUB preparation. The device renderer has the same profile, so a
// book looks the same whether it was prepared in the browser or copied to SD.
(() => {
  if (typeof JSZip === 'undefined') return;

  const MARKER = '/* inkMOD Classic Book v1 */';
  const CSS = `${MARKER}
/* Compact printed-book prose */
p, .paragraph {
  margin-top: 0 !important;
  margin-bottom: 0 !important;
  padding-top: 0 !important;
  padding-bottom: 0 !important;
  text-indent: 1.5em !important;
  text-align: justify !important;
}

/* Chapter titles and subtitles */
h1, h2, h3, h4, h5, h6,
subtitle, .subtitle, .title-line, .subheading {
  display: block !important;
  text-align: center !important;
  font-weight: bold !important;
  margin-top: 1em !important;
  margin-bottom: 0.5em !important;
  text-indent: 0 !important;
}

/* Quotes / FB2 cite and epigraph */
blockquote, cite, .cite, .quote, .epigraph {
  display: block !important;
  margin-top: 0.6em !important;
  margin-bottom: 0.6em !important;
  margin-left: 1.2em !important;
  margin-right: 1.2em !important;
  text-indent: 0 !important;
}
blockquote p, cite p, .cite p, .quote p, .epigraph p {
  margin: 0 !important;
  padding-top: 0 !important;
  padding-bottom: 0 !important;
  text-indent: 1em !important;
  text-align: justify !important;
}

/* Poems: keep real line breaks and never apply a prose red line */
poem, .poem, .poetry {
  display: block !important;
  margin-top: 0.6em !important;
  margin-bottom: 0.6em !important;
  margin-left: 1.2em !important;
  margin-right: 1.2em !important;
}
poem p, poem v,
.poem p, .poem .v, .poem .verse, .poem .line,
.poetry p, .poetry .line,
.v, .verse {
  text-indent: 0 !important;
  margin-top: 0 !important;
  margin-bottom: 0 !important;
  padding-top: 0 !important;
  padding-bottom: 0 !important;
  text-align: left !important;
}

/* Stanzas */
stanza, .stanza, .stanza-break {
  display: block !important;
  margin-top: 0 !important;
  margin-bottom: 0.6em !important;
}
stanza:last-child, .stanza:last-child { margin-bottom: 0 !important; }

/* FB2 structural helpers */
.text-author {
  text-align: right !important;
  text-indent: 0 !important;
}
.date {
  text-align: center !important;
  text-indent: 0 !important;
}
.empty-line {
  margin-top: 0.6em !important;
  margin-bottom: 0 !important;
  text-indent: 0 !important;
}
.note p, .note-label {
  margin: 0 !important;
  text-indent: 0 !important;
  text-align: left !important;
}
`;

  const styleTag = () => `<style type="text/css">\n${CSS}\n</style>`;

  async function applyClassicBookStyle(epubBlob) {
    const zip = await JSZip.loadAsync(epubBlob);
    let changed = false;

    // Put the profile at the end of every existing CSS file. This keeps the
    // package valid for readers that ignore XHTML <style>, while !important
    // makes the intended geometry deterministic in ordinary EPUB engines.
    for (const [path, entry] of Object.entries(zip.files)) {
      if (entry.dir || !/\.css$/i.test(path)) continue;
      let css = await entry.async('text');
      if (css.includes(MARKER)) continue;
      css += `\n\n${CSS}\n`;
      zip.file(path, css, { compression: 'DEFLATE' });
      changed = true;
    }

    // Also inject the profile into each reading document. This covers EPUBs
    // with no stylesheet at all and makes browser-side prepared FB2 packages
    // self-contained.
    for (const [path, entry] of Object.entries(zip.files)) {
      if (entry.dir || !/\.(xhtml|html|htm)$/i.test(path)) continue;
      let text = await entry.async('text');
      if (text.includes(MARKER)) continue;

      if (/<\/head\s*>/i.test(text)) {
        text = text.replace(/<\/head\s*>/i, `${styleTag()}\n</head>`);
      } else if (/<html\b[^>]*>/i.test(text)) {
        text = text.replace(/<html\b[^>]*>/i, match => `${match}\n<head>${styleTag()}</head>`);
      } else {
        continue;
      }
      zip.file(path, text, { compression: 'DEFLATE' });
      changed = true;
    }

    if (!changed) return epubBlob;

    // EPUB requires this file to be stored, not deflated. Replacing the key
    // keeps it at the beginning of JSZip's existing insertion order.
    if (zip.file('mimetype')) {
      zip.file('mimetype', 'application/epub+zip', { compression: 'STORE' });
    }

    return zip.generateAsync({
      type: 'blob',
      mimeType: 'application/epub+zip',
      compression: 'DEFLATE',
      compressionOptions: { level: 8 }
    });
  }

  window.inkmodApplyClassicBookStyle = applyClassicBookStyle;

  // Normal EPUB optimization. The returned File later becomes the exact cache
  // source, so optimization and browser cache preparation see the same book.
  if (typeof convertEpubFile === 'function') {
    const originalConvertEpubFile = convertEpubFile;
    convertEpubFile = async function(file, progressCallback) {
      const converted = await originalConvertEpubFile(file, progressCallback);
      try {
        return await applyClassicBookStyle(converted);
      } catch (error) {
        console.warn('[Classic Book] EPUB normalization skipped:', error);
        return converted;
      }
    };
  }

  // FB2 preparation and real FB2 -> EPUB conversion share this function.
  // Wrapping it here therefore covers both the hidden prepared package/cache
  // path and the user-visible conversion path.
  if (typeof convertFb2ToEpub === 'function') {
    const originalConvertFb2ToEpub = convertFb2ToEpub;
    convertFb2ToEpub = async function(file, progressCallback) {
      const converted = await originalConvertFb2ToEpub(file, progressCallback);
      try {
        return await applyClassicBookStyle(converted);
      } catch (error) {
        console.warn('[Classic Book] FB2 package normalization skipped:', error);
        return converted;
      }
    };
  }
})();
