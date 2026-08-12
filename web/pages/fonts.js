function formatSize(bytes) {
      if (bytes >= 1048576) return (bytes / 1048576).toFixed(1) + ' MB';
      if (bytes >= 1024) return (bytes / 1024).toFixed(0) + ' KB';
      return bytes + ' B';
    }

    async function loadFonts() {
      const el = document.getElementById('families');
      try {
        const res = await fetch('/api/fonts');
        const data = await res.json();
        // Build rows with DOM APIs and textContent so on-device family names
        // (which can contain arbitrary characters) cannot break markup or
        // execute script via innerHTML / inline onclick interpolation.
        el.replaceChildren();
        if (!data.families || data.families.length === 0) {
          const p = document.createElement('p');
          p.className = 'empty';
          p.textContent = t('fonts.empty');
          el.appendChild(p);
          return;
        }
        for (const f of data.families) {
          const row = document.createElement('div');
          row.className = 'family';

          const info = document.createElement('div');
          info.className = 'family-info';
          const h3 = document.createElement('h3');
          h3.textContent = f.name;
          info.appendChild(h3);
          const meta = document.createElement('span');
          meta.className = 'family-meta';
          const sizes = (f.sizes || []).join(', ');
          const filesSizes = (f.files || []).map(fi => formatSize(fi.size)).join(' + ');
          meta.textContent = sizes + 'pt · ' + filesSizes;
          info.appendChild(meta);

          const btn = document.createElement('button');
          btn.className = 'btn btn-danger';
          btn.textContent = t('fonts.delete_btn');
          // Capture name in the closure rather than interpolating into onclick.
          const familyName = f.name;
          btn.addEventListener('click', () => deleteFamily(familyName));

          row.appendChild(info);
          row.appendChild(btn);
          el.appendChild(row);
        }
      } catch (e) {
        el.replaceChildren();
        const p = document.createElement('p');
        p.className = 'empty';
        p.textContent = t('fonts.load_failed');
        el.appendChild(p);
      }
    }

    async function deleteFamily(name) {
      if (!confirm(t('fonts.delete_confirm', {name: name}))) return;
      const status = document.getElementById('status');
      status.className = '';
      status.style.display = 'block';
      status.textContent = t('fonts.deleting', {name: name});
      try {
        const res = await fetch('/api/fonts/delete', {
          method: 'POST',
          headers: {'Content-Type': 'application/json'},
          body: JSON.stringify({family: name})
        });
        if (res.ok) {
          status.className = 'status-ok';
          status.textContent = t('fonts.deleted', {name: name});
        } else {
          status.className = 'status-err';
          status.textContent = t('fonts.delete_failed', {name: name});
        }
      } catch (err) {
        status.className = 'status-err';
        status.textContent = t('fonts.delete_error', {msg: err.message});
      }
      await loadFonts();
    }

    // ---- In-browser TTF/OTF converter ----
    // Runs entirely in whatever browser has this page open (phone or
    // laptop) — the device does zero heavy lifting, it just receives the
    // finished .cpfont through the same /api/fonts/upload endpoint the
    // manual "Upload Font" form below already uses. No ESP32 memory/stack
    // limits apply here, since none of this runs on the device.
    //
    // How it rasterizes without a WASM font-rendering engine: registers
    // the uploaded font via the FontFace API and draws each character on a
    // <canvas>, then reads the alpha channel back as a greyscale bitmap —
    // the browser's own text renderer does the actual glyph rasterization.
    // Glyph *existence* (does this font have character X at all) is
    // checked separately via a small hand-rolled parser for the font's
    // `cmap` table (formats 4 and 12), since Canvas alone can't tell "real
    // glyph" from "tofu fallback box" reliably.
    //
    // Same limitations as before, for the same reasons: Regular weight
    // only, no kerning/ligatures (both need real GPOS/GSUB table parsing,
    // out of scope here same as it was for the desktop and on-device
    // paths). Unlike the on-device path, though, there's no practical size
    // ceiling — this comfortably handles real "designer" fonts.

    const CPFONT_INTERVAL_PRESETS = {
      ascii: [[0x0020, 0x007E]],
      latin1: [[0x0020, 0x007E], [0x00A0, 0x00FF]],
      'latin-ext': [[0x0020, 0x007E], [0x0080, 0x00FF], [0x0100, 0x024F], [0x1E00, 0x1EFF], [0x2000, 0x206F]],
      cyrillic: [[0x0020, 0x007E], [0x0400, 0x04FF], [0x0500, 0x052F]],
      greek: [[0x0020, 0x007E], [0x0370, 0x03FF], [0x1F00, 0x1FFF]],
      // Wide multi-script span: only glyphs the font actually has (per the
      // cmap existence check below) ever get rasterized, so a Latin-only
      // font stays fast even with this selected — everything else is
      // skipped after a cheap lookup, not rendered and discarded.
      comprehensive: [
        [0x0020, 0x007E], [0x00A0, 0x00FF], [0x0100, 0x017F], [0x0180, 0x024F], [0x1E00, 0x1EFF],
        [0x0250, 0x02AF], [0x02B0, 0x02FF], [0x0300, 0x036F], [0x0370, 0x03FF], [0x1F00, 0x1FFF],
        [0x0400, 0x04FF], [0x0500, 0x052F], [0x2DE0, 0x2DFF], [0xA640, 0xA69F], [0x1C80, 0x1C8F],
        [0x0530, 0x058F], [0x0590, 0x05FF], [0x0600, 0x06FF], [0x0750, 0x077F], [0x08A0, 0x08FF],
        [0x0700, 0x074F], [0x0900, 0x097F], [0x0E00, 0x0E7F], [0x10A0, 0x10FF], [0x2000, 0x206F],
        [0x2070, 0x209F], [0x20A0, 0x20CF], [0x2100, 0x214F], [0x2150, 0x218F], [0x2190, 0x21FF],
        [0x2200, 0x22FF], [0x2300, 0x23FF], [0x2400, 0x243F], [0x2440, 0x245F], [0x2460, 0x24FF],
        [0x2500, 0x257F], [0x2580, 0x259F], [0x25A0, 0x25FF], [0x2600, 0x26FF], [0x2700, 0x27BF],
        [0x2800, 0x28FF], [0x3000, 0x303F], [0x3040, 0x309F], [0x30A0, 0x30FF], [0x3100, 0x312F],
        [0xAC00, 0xD7AF], [0x4E00, 0x9FFF], [0xE000, 0xF8FF], [0xFB00, 0xFB4F], [0xFE20, 0xFE2F],
        [0xFE30, 0xFE4F], [0xFE50, 0xFE6F], [0xFF00, 0xFFEF],
      ],
    };

    function familyFromSourceFilename(name) {
      return name.replace(/\.(ttf|otf)$/i, '')
        .replace(/[-_ ]?(bold\s*italic|bolditalic|boldoblique|bold|italic|oblique|regular)$/i, '');
    }

    // Sorts and merges overlapping/adjacent [start, end] ranges into the
    // smallest equivalent non-overlapping set.
    function mergeRanges(ranges) {
      if (ranges.length === 0) return [];
      const sorted = ranges.map(([a, b]) => [a, b]).sort((a, b) => a[0] - b[0]);
      const merged = [sorted[0]];
      for (let i = 1; i < sorted.length; i++) {
        const [start, end] = sorted[i];
        const last = merged[merged.length - 1];
        if (start <= last[1] + 1) {
          last[1] = Math.max(last[1], end);
        } else {
          merged.push([start, end]);
        }
      }
      return merged;
    }

    // Parses "0x2600-0x26FF, U+1F300-U+1F5FF, 0x20AC" into [[first,last],...].
    // Accepts single codepoints or first-last spans, each with or without a
    // 0x/U+ prefix, comma-separated. Silently skips anything unparseable
    // rather than failing the whole conversion over a typo.
    function parseCustomRanges(text) {
      if (!text || !text.trim()) return [];
      const ranges = [];
      for (let part of text.split(',')) {
        part = part.trim();
        if (!part) continue;
        const span = part.match(/^(?:u\+|0x)?([0-9a-f]+)\s*(?:-|\.\.)\s*(?:u\+|0x)?([0-9a-f]+)$/i);
        if (span) {
          const first = parseInt(span[1], 16);
          const last = parseInt(span[2], 16);
          if (Number.isFinite(first) && Number.isFinite(last) && first <= last) ranges.push([first, last]);
          continue;
        }
        const single = part.match(/^(?:u\+|0x)?([0-9a-f]+)$/i);
        if (single) {
          const cp = parseInt(single[1], 16);
          if (Number.isFinite(cp)) ranges.push([cp, cp]);
        }
      }
      return ranges;
    }

    const STANDARD_SIZES = [12, 14, 16, 18];

    // Parses "20, 24" into [20, 24], deduped and merged with STANDARD_SIZES,
    // sorted ascending. Sizes outside a sane 6-72pt range are dropped.
    function parseSizes(text) {
      const extra = (text || '').split(',')
        .map((s) => parseInt(s.trim(), 10))
        .filter((n) => Number.isFinite(n) && n >= 6 && n <= 72);
      return Array.from(new Set([...STANDARD_SIZES, ...extra])).sort((a, b) => a - b);
    }

    // --- Folder-based style auto-detection ---
    // Groups whatever TTF/OTF files are in the picked folder into
    // Regular/Bold/Italic/Bold Italic by filename, so the user doesn't
    // have to assign each file to a slot by hand.

    function fontSourceFilesOnly(fileList) {
      return Array.from(fileList).filter((f) => /\.(ttf|otf)$/i.test(f.name));
    }

    function detectStyleFromFilename(name) {
      const lower = name.toLowerCase();
      const hasBold = /bold/.test(lower);
      const hasItalic = /italic|oblique/.test(lower);
      if (hasBold && hasItalic) return 'bolditalic';
      if (hasBold) return 'bold';
      if (hasItalic) return 'italic';
      return 'regular';
    }

    // Returns { grouped: {regular,bold,italic,bolditalic}, extras: File[] }
    // extras holds any file whose detected style slot was already taken —
    // reported to the user rather than silently dropped.
    function groupStyleFiles(files) {
      const grouped = { regular: null, bold: null, italic: null, bolditalic: null };
      const extras = [];
      for (const f of fontSourceFilesOnly(files)) {
        const style = detectStyleFromFilename(f.name);
        if (!grouped[style]) grouped[style] = f;
        else extras.push(f);
      }
      return { grouped, extras };
    }

    function describeGrouping(grouped, extras, family) {
      const labels = [
        ['regular', t('fonts.style_regular')], ['bold', t('fonts.style_bold')],
        ['italic', t('fonts.style_italic')], ['bolditalic', t('fonts.style_bolditalic')],
      ];
      const parts = labels.map(([key, label]) => label + (grouped[key] ? ' \u2713' : ' \u2014'));
      let text = (family || t('fonts.no_family_yet')) + ': ' + parts.join(', ');
      if (extras.length > 0) {
        text += t('fonts.ignored_extra', {
          count: extras.length, plural: extras.length === 1 ? '' : 's',
          names: extras.map((f) => f.name).join(', '),
        });
      }
      return text;
    }

    function showConvertMessage(text, ok) {
      const status = document.getElementById('convertStatus');
      status.className = ok ? 'status-ok' : 'status-err';
      status.style.display = 'block';
      status.replaceChildren();
      const p = document.createElement('p');
      p.style.margin = '0';
      p.textContent = text;
      status.appendChild(p);
    }

    // --- sfnt / cmap parsing (big-endian, per the OpenType spec) ---

    function sfntReadTables(buf) {
      const view = new DataView(buf);
      let base = 0;
      if (view.getUint32(0, false) === 0x74746366) {  // 'ttcf' — TrueType Collection
        base = view.getUint32(12, false);
      }
      const numTables = view.getUint16(base + 4, false);
      const tables = {};
      let p = base + 12;
      for (let i = 0; i < numTables; i++) {
        const tag = String.fromCharCode(
          view.getUint8(p), view.getUint8(p + 1), view.getUint8(p + 2), view.getUint8(p + 3));
        tables[tag] = { offset: view.getUint32(p + 8, false), length: view.getUint32(p + 12, false) };
        p += 16;
      }
      return tables;
    }

    function cmapFormat4Lookup(view, off) {
      const segCountX2 = view.getUint16(off + 6, false);
      const segCount = segCountX2 / 2;
      const endCodeOff = off + 14;
      const startCodeOff = endCodeOff + segCountX2 + 2;
      const idDeltaOff = startCodeOff + segCountX2;
      const idRangeOffsetOff = idDeltaOff + segCountX2;
      return function (cp) {
        if (cp > 0xFFFF) return 0;
        let lo = 0, hi = segCount - 1, seg = -1;
        while (lo <= hi) {
          const mid = (lo + hi) >> 1;
          if (cp <= view.getUint16(endCodeOff + mid * 2, false)) { seg = mid; hi = mid - 1; }
          else lo = mid + 1;
        }
        if (seg === -1) return 0;
        const startCode = view.getUint16(startCodeOff + seg * 2, false);
        if (cp < startCode) return 0;
        const idDelta = view.getInt16(idDeltaOff + seg * 2, false);
        const idRangeOffset = view.getUint16(idRangeOffsetOff + seg * 2, false);
        if (idRangeOffset === 0) return (cp + idDelta) & 0xFFFF;
        const addr = idRangeOffsetOff + seg * 2 + idRangeOffset + (cp - startCode) * 2;
        const g = view.getUint16(addr, false);
        return g === 0 ? 0 : (g + idDelta) & 0xFFFF;
      };
    }

    function cmapFormat12Lookup(view, off) {
      const numGroups = view.getUint32(off + 12, false);
      const groupsOff = off + 16;
      return function (cp) {
        let lo = 0, hi = numGroups - 1;
        while (lo <= hi) {
          const mid = (lo + hi) >> 1;
          const g = groupsOff + mid * 12;
          const startChar = view.getUint32(g, false);
          const endChar = view.getUint32(g + 4, false);
          if (cp < startChar) hi = mid - 1;
          else if (cp > endChar) lo = mid + 1;
          else return view.getUint32(g + 8, false) + (cp - startChar);
        }
        return 0;
      };
    }

    function getCmapLookup(buf, tables) {
      if (!tables.cmap) return null;
      const view = new DataView(buf);
      const cmapOff = tables.cmap.offset;
      const numSubtables = view.getUint16(cmapOff + 2, false);
      const candidates = [];
      for (let i = 0; i < numSubtables; i++) {
        const rec = cmapOff + 4 + i * 8;
        candidates.push({
          platformID: view.getUint16(rec, false),
          encodingID: view.getUint16(rec + 2, false),
          offset: cmapOff + view.getUint32(rec + 4, false),
        });
      }
      const pref = (c) => (c.platformID === 3 && c.encodingID === 1) ? 0
        : (c.platformID === 0) ? 1
        : (c.platformID === 3 && c.encodingID === 10) ? 2 : 3;
      candidates.sort((a, b) => pref(a) - pref(b));
      for (const c of candidates) {
        const format = view.getUint16(c.offset, false);
        if (format === 4) return cmapFormat4Lookup(view, c.offset);
        if (format === 12) return cmapFormat12Lookup(view, c.offset);
      }
      return null;
    }

    // --- Canvas-based rasterizer ---

    function makeRasterizer(fontFamily, sizePx) {
      const canvasSize = Math.max(64, Math.ceil(sizePx * 2.5));
      const canvas = document.createElement('canvas');
      canvas.width = canvasSize;
      canvas.height = canvasSize;
      const ctx = canvas.getContext('2d', { willReadFrequently: true });
      const originX = 4;
      const originY = Math.round(canvasSize * 0.65);

      return function rasterize(char) {
        ctx.clearRect(0, 0, canvasSize, canvasSize);
        ctx.font = sizePx + 'px "' + fontFamily + '"';
        ctx.textBaseline = 'alphabetic';
        ctx.fillStyle = '#000';
        const metrics = ctx.measureText(char);
        ctx.fillText(char, originX, originY);
        const img = ctx.getImageData(0, 0, canvasSize, canvasSize);
        const data = img.data;
        let minX = canvasSize, minY = canvasSize, maxX = -1, maxY = -1;
        for (let y = 0; y < canvasSize; y++) {
          for (let x = 0; x < canvasSize; x++) {
            if (data[(y * canvasSize + x) * 4 + 3] > 8) {
              if (x < minX) minX = x;
              if (x > maxX) maxX = x;
              if (y < minY) minY = y;
              if (y > maxY) maxY = y;
            }
          }
        }
        const advance = Math.round(metrics.width * 16);  // 12.4 fixed point
        if (maxX < 0) return { width: 0, height: 0, left: 0, top: 0, advance, pixels: null };

        const width = maxX - minX + 1;
        const height = maxY - minY + 1;
        const pixels = new Uint8Array(width * height);
        for (let y = 0; y < height; y++) {
          for (let x = 0; x < width; x++) {
            pixels[y * width + x] = data[((minY + y) * canvasSize + (minX + x)) * 4 + 3];
          }
        }
        return { width, height, left: minX - originX, top: originY - minY, advance, pixels };
      };
    }

    function pack2Bit(grey, w, h) {
      const out = [];
      let acc = 0, bits = 0;
      for (let y = 0; y < h; y++) {
        for (let x = 0; x < w; x++) {
          const v = grey[y * w + x];
          const level = v >= 192 ? 3 : v >= 128 ? 2 : v >= 64 ? 1 : 0;
          acc = ((acc << 2) | level) & 0xFF;
          bits += 2;
          if (bits === 8) { out.push(acc); acc = 0; bits = 0; }
        }
      }
      if (bits > 0) out.push((acc << (8 - bits)) & 0xFF);
      return new Uint8Array(out);
    }

    // --- .cpfont v4 binary encoder (multi-style) ---
    // Byte layout matches lib/EpdFont/scripts/fontconvert_sdcard.py and
    // SdCardFont::load() exactly, verified against a real production file
    // (Header, then one 32-byte TOC entry per style — this IS the index,
    // right after the header, not at the end — then each style's own
    // intervals/glyphs/bitmap written sequentially in TOC order). Kerning
    // and ligature tables are left empty for every style (all-zero counts)
    // — still no GPOS/GSUB parsing here — which is a fully legal v4 file,
    // same as a single-style one.

    function buildCpfont(styles) {
      const HEADER_SIZE = 32, TOC_SIZE = 32, INTERVAL_SIZE = 12, GLYPH_SIZE = 16;
      let totalSize = HEADER_SIZE + styles.length * TOC_SIZE;
      for (const s of styles) {
        const bitmapSize = s.bitmapChunks.reduce((sum, c) => sum + c.length, 0);
        totalSize += s.intervals.length * INTERVAL_SIZE + s.glyphs.length * GLYPH_SIZE + bitmapSize;
      }

      const buf = new ArrayBuffer(totalSize);
      const view = new DataView(buf);
      const bytes = new Uint8Array(buf);

      const magic = [0x43, 0x50, 0x46, 0x4F, 0x4E, 0x54, 0x00, 0x00];  // "CPFONT\0\0"
      magic.forEach((b, i) => { bytes[i] = b; });
      view.setUint16(8, 4, true);              // version
      view.setUint16(10, 1, true);             // flags: is2Bit
      bytes[12] = styles.length;               // styleCount

      // First pass: write each style's TOC entry, and remember where its
      // data section will start so we can lay out the data in a second
      // pass once every dataOffset is already known.
      let dataCursor = HEADER_SIZE + styles.length * TOC_SIZE;
      const dataOffsets = [];
      for (let i = 0; i < styles.length; i++) {
        const s = styles[i];
        const tocOff = HEADER_SIZE + i * TOC_SIZE;
        dataOffsets.push(dataCursor);

        bytes[tocOff] = s.styleId;
        view.setUint32(tocOff + 4, s.intervals.length, true);
        view.setUint32(tocOff + 8, s.glyphs.length, true);
        bytes[tocOff + 12] = Math.min(255, Math.max(0, s.advanceY));
        view.setInt16(tocOff + 13, s.ascender, true);
        view.setInt16(tocOff + 15, s.descender, true);
        view.setUint16(tocOff + 17, 0, true);  // kernLeftEntryCount
        view.setUint16(tocOff + 19, 0, true);  // kernRightEntryCount
        bytes[tocOff + 21] = 0;                // kernLeftClassCount
        bytes[tocOff + 22] = 0;                // kernRightClassCount
        bytes[tocOff + 23] = 0;                // ligaturePairCount
        view.setUint32(tocOff + 24, dataCursor, true);  // dataOffset

        const bitmapSize = s.bitmapChunks.reduce((sum, c) => sum + c.length, 0);
        dataCursor += s.intervals.length * INTERVAL_SIZE + s.glyphs.length * GLYPH_SIZE + bitmapSize;
      }

      // Second pass: write each style's intervals, glyphs, and bitmap
      // bytes at the offset reserved for it above.
      for (let i = 0; i < styles.length; i++) {
        const s = styles[i];
        let p = dataOffsets[i];

        let offset = 0;
        for (const [start, end] of s.intervals) {
          view.setUint32(p, start, true);
          view.setUint32(p + 4, end, true);
          view.setUint32(p + 8, offset, true);
          offset += (end - start + 1);
          p += INTERVAL_SIZE;
        }

        for (const g of s.glyphs) {
          bytes[p] = g.width;
          bytes[p + 1] = g.height;
          view.setUint16(p + 2, g.advanceX, true);
          view.setInt16(p + 4, g.left, true);
          view.setInt16(p + 6, g.top, true);
          view.setUint16(p + 8, g.dataLength, true);
          view.setUint32(p + 12, g.dataOffset, true);
          p += GLYPH_SIZE;
        }

        for (const chunk of s.bitmapChunks) {
          bytes.set(chunk, p);
          p += chunk.length;
        }
      }

      return bytes;
    }

    // --- Orchestration ---

    // Rasterizes ONE style's file into the data buildCpfont needs for it.
    // Called once per provided style (Regular/Bold/Italic/BoldItalic).
    async function rasterizeStyle(file, sizePt, presetName, customRanges, onProgress) {
      const buf = await file.arrayBuffer();
      const tables = sfntReadTables(buf);
      const cmapLookup = getCmapLookup(buf, tables);
      if (!cmapLookup) throw new Error(t('fonts.cmap_read_failed', {name: file.name}));

      const fontFamily = 'FontStudioPreview' + Math.random().toString(36).slice(2);
      const fontFace = new FontFace(fontFamily, buf);
      await fontFace.load();
      document.fonts.add(fontFace);

      try {
        const sizePx = Math.round(sizePt * 150 / 72);  // match the desktop tool's 150dpi convention
        const probeCanvas = document.createElement('canvas');
        const probeCtx = probeCanvas.getContext('2d');
        probeCtx.font = sizePx + 'px "' + fontFamily + '"';
        const globalMetrics = probeCtx.measureText('Hg');
        const ascender = Math.round(globalMetrics.fontBoundingBoxAscent);
        const descender = -Math.round(globalMetrics.fontBoundingBoxDescent);
        const advanceY = ascender - descender;

        const requested = (CPFONT_INTERVAL_PRESETS[presetName] || CPFONT_INTERVAL_PRESETS.ascii).slice();
        requested.push(...(customRanges || []));
        requested.push([0xFFFD, 0xFFFD]);
        // Custom ranges can overlap the preset (or each other); merge before
        // the existence check below so no codepoint gets processed twice,
        // which would otherwise produce duplicate/overlapping intervals —
        // exactly the kind of layout the device's loader rejects.
        const mergedRequested = mergeRanges(requested);

        const validated = [];
        let totalGlyphs = 0;
        for (const [start, end] of mergedRequested) {
          let runStart = null;
          for (let cp = start; cp <= end; cp++) {
            const has = cmapLookup(cp) !== 0;
            if (has && runStart === null) runStart = cp;
            if (!has && runStart !== null) { validated.push([runStart, cp - 1]); totalGlyphs += cp - runStart; runStart = null; }
            if (cp === end && runStart !== null) { validated.push([runStart, end]); totalGlyphs += end - runStart + 1; }
          }
        }
        if (totalGlyphs === 0) throw new Error(t('fonts.no_glyphs_in_range'));

        // The device's SdCardFont::load() binary-searches this array and
        // separately rejects the WHOLE FILE if any interval's start isn't
        // strictly greater than the previous interval's end — i.e. it
        // requires the array sorted ascending by codepoint. Preset lists
        // like "comprehensive" aren't declared in codepoint order (they're
        // grouped by script for readability), so the merged runs from pass
        // 1 above come out in *request* order, not sorted order. Sorting
        // here is what keeps a valid-looking file from being silently
        // rejected on load (which otherwise looks like nothing happened —
        // the font just never actually loads).
        validated.sort((a, b) => a[0] - b[0]);

        const rasterize = makeRasterizer(fontFamily, sizePx);
        const glyphs = [];
        const bitmapChunks = [];
        let bitmapOffset = 0;
        let done = 0;

        for (const [start, end] of validated) {
          for (let cp = start; cp <= end; cp++) {
            const g = rasterize(String.fromCodePoint(cp));
            let dataLength = 0;
            if (g.pixels) {
              const packed = pack2Bit(g.pixels, g.width, g.height);
              bitmapChunks.push(packed);
              dataLength = packed.length;
            }
            glyphs.push({
              width: Math.min(g.width, 255), height: Math.min(g.height, 255),
              advanceX: Math.max(0, Math.min(0xFFFF, g.advance)),
              left: g.left, top: g.top, dataLength, dataOffset: bitmapOffset,
            });
            bitmapOffset += dataLength;
            done++;
            if (onProgress && done % 20 === 0) onProgress(done, totalGlyphs);
            if (done % 200 === 0) await new Promise((r) => setTimeout(r, 0));
          }
        }

        return { intervals: validated, glyphs, bitmapChunks, ascender, descender, advanceY };
      } finally {
        document.fonts.delete(fontFace);
      }
    }

    // styleFiles: { regular: File, bold?: File, italic?: File, bolditalic?: File }
    // Regular is required; the others bundle in only if provided. Style
    // IDs are fixed by the format: 0=Regular, 1=Bold, 2=Italic, 3=BoldItalic.
    // Label keys (not labels themselves) — resolved via t() at use time so a
    // language switch mid-session doesn't leave a stale English label baked
    // into this module-level constant.
    const STYLE_ORDER = [
      { key: 'regular', id: 0, labelKey: 'fonts.style_regular' },
      { key: 'bold', id: 1, labelKey: 'fonts.style_bold' },
      { key: 'italic', id: 2, labelKey: 'fonts.style_italic' },
      { key: 'bolditalic', id: 3, labelKey: 'fonts.style_bolditalic' },
    ];

    async function convertMultiStyleFont(styleFiles, sizePt, presetName, customRanges, onProgress) {
      if (!styleFiles.regular) throw new Error(t('fonts.regular_required'));
      const present = STYLE_ORDER.filter((s) => styleFiles[s.key]);
      const styles = [];
      for (let i = 0; i < present.length; i++) {
        const s = present[i];
        const result = await rasterizeStyle(styleFiles[s.key], sizePt, presetName, customRanges, (done, total) => {
          onProgress(t(s.labelKey), i + 1, present.length, done, total);
        });
        styles.push({ styleId: s.id, ...result });
      }
      return buildCpfont(styles);
    }

    // --- Zip-archive alternative to the folder picker ---
    // Google Fonts (and most other font sources) hand you a single .zip
    // rather than an already-unzipped folder. Rather than duplicating the
    // folder-picker's downstream logic (grouping, upload, status messages),
    // this extracts the matching entries from the zip into real File objects
    // and injects them into the *same* <input> the folder picker uses, via
    // the DataTransfer trick — then fires a synthetic 'change' event so every
    // existing listener (info line, submit handler) runs exactly as if the
    // user had picked an already-unzipped folder.
    async function loadZipIntoInput(zipFile, targetInput, extRegex) {
      if (typeof JSZip === 'undefined') {
        alert(t('fonts.jszip_unavailable'));
        return;
      }
      const zip = await JSZip.loadAsync(zipFile);
      const dt = new DataTransfer();
      for (const [path, entry] of Object.entries(zip.files)) {
        // Skip directory entries, macOS resource-fork junk, and anything
        // that doesn't match the extension the target input expects.
        if (entry.dir || path.includes('__MACOSX') || !extRegex.test(path)) continue;
        const blob = await entry.async('blob');
        // Flatten: Google Fonts zips nest files under e.g. static/, and
        // downstream code only ever looks at File.name, not any path.
        const baseName = path.split('/').pop();
        dt.items.add(new File([blob], baseName));
      }
      targetInput.files = dt.files;
      targetInput.dispatchEvent(new Event('change'));
    }

    document.getElementById('convertZipFile').addEventListener('change', async function () {
      if (!this.files[0]) return;
      const info = document.getElementById('convertPickedInfo');
      info.textContent = t('fonts.extracting_zip');
      try {
        await loadZipIntoInput(this.files[0], document.getElementById('convertFiles'), /\.(ttf|otf)$/i);
      } catch (err) {
        info.textContent = t('fonts.zip_extract_error', {msg: err.message});
      }
    });

    document.getElementById('fontZipFile').addEventListener('change', async function () {
      if (!this.files[0]) return;
      const info = document.getElementById('pickedInfo');
      info.textContent = t('fonts.extracting_zip');
      try {
        await loadZipIntoInput(this.files[0], document.getElementById('fontFiles'), /\.cpfont$/i);
      } catch (err) {
        info.textContent = t('fonts.zip_extract_error', {msg: err.message});
      }
    });

    // Update the summary line whenever a folder is (re)picked, so the user
    // can see the detected Regular/Bold/Italic/Bold Italic mapping before
    // hitting convert — no per-slot pickers to manually assign anymore.
    // This also fires after a zip is extracted into this input above, since
    // loadZipIntoInput() dispatches a synthetic 'change' event.
    document.getElementById('convertFiles').addEventListener('change', function () {
      const info = document.getElementById('convertPickedInfo');
      const { grouped, extras } = groupStyleFiles(this.files);
      if (!grouped.regular && !grouped.bold && !grouped.italic && !grouped.bolditalic) {
        info.textContent = t('fonts.no_ttf_found');
        return;
      }
      const anySource = grouped.regular || grouped.bold || grouped.italic || grouped.bolditalic;
      const family = sanitizeFamily(familyFromSourceFilename(anySource.name));
      info.textContent = describeGrouping(grouped, extras, family);
    });

    document.getElementById('convertForm').addEventListener('submit', async function(e) {
      e.preventDefault();

      const { grouped: styleFiles, extras } = groupStyleFiles(document.getElementById('convertFiles').files);
      if (!styleFiles.regular) {
        showConvertMessage(t('fonts.no_regular'), false);
        return;
      }
      const sizes = parseSizes(document.getElementById('convertExtraSizes').value);
      const customRanges = parseCustomRanges(document.getElementById('convertCustomRanges').value);
      const intervals = document.getElementById('convertIntervals').value;
      const family = sanitizeFamily(familyFromSourceFilename(styleFiles.regular.name));

      const submitBtn = e.target.querySelector('button[type="submit"]');
      submitBtn.disabled = true;
      const extrasNote = extras.length > 0
        ? t('fonts.ignored_extra_note', {count: extras.length, plural: extras.length === 1 ? '' : 's'}) : '';
      showConvertMessage(t('fonts.converting_sizes', {
        family: family, count: sizes.length, plural: sizes.length === 1 ? '' : 's',
        sizes: sizes.join(', '), extras: extrasNote,
      }), true);

      const uploaded = [];
      const failed = [];
      try {
        for (let s = 0; s < sizes.length; s++) {
          const sizePt = sizes[s];
          const sizeProgress = '[' + (s + 1) + '/' + sizes.length + ' sizes, ' + sizePt + 'pt] ';
          try {
            const cpfontBytes = await convertMultiStyleFont(styleFiles, sizePt, intervals, customRanges,
              (styleLabel, styleIdx, styleTotal, done, total) => {
                showConvertMessage(t('fonts.converting_style', {
                  progress: sizeProgress, family: family, style: styleLabel,
                  styleIdx: styleIdx, styleTotal: styleTotal, done: done, total: total,
                }), true);
              });

            showConvertMessage(t('fonts.uploading', {progress: sizeProgress, family: family}), true);
            const formData = new FormData();
            formData.append('family', family);
            const filename = family + '_' + sizePt + '.cpfont';
            formData.append('file', new Blob([cpfontBytes]), filename);

            const res = await fetch('/api/fonts/upload', { method: 'POST', body: formData });
            const data = await res.json();
            if (data.ok) {
              uploaded.push(sizePt);
            } else {
              failed.push(sizePt + 'pt: ' + (data.error || 'unknown error'));
            }
          } catch (err) {
            failed.push(sizePt + 'pt: ' + err.message);
          }
        }

        if (failed.length === 0) {
          showConvertMessage(t('fonts.done_ok', {family: family, sizes: uploaded.join(', ')}), true);
        } else if (uploaded.length > 0) {
          showConvertMessage(t('fonts.done_partial', {sizes: uploaded.join(', '), errors: failed.join('; ')}), true);
        } else {
          showConvertMessage(t('fonts.done_failed_only', {errors: failed.join('; ')}), false);
        }
        await loadFonts();
      } catch (err) {
        showConvertMessage(t('fonts.conversion_error', {msg: err.message}), false);
      } finally {
        submitBtn.disabled = false;
      }
    });

    // Derive family name from a .cpfont filename: take everything before the
    // last '-' or '_' (that separator precedes the size suffix, e.g. Bookerly_12.cpfont).
    function familyFromFilename(name) {
      const stem = name.replace(/\.cpfont$/i, '');
      const cut = Math.max(stem.lastIndexOf('-'), stem.lastIndexOf('_'));
      return cut > 0 ? stem.slice(0, cut) : stem;
    }

    // Sanitize to match firmware's [A-Za-z0-9_-]+ pattern.
    function sanitizeFamily(raw) {
      return raw.replace(/[^A-Za-z0-9_-]/g, '_');
    }

    function cpfontFilesOnly(fileList) {
      return Array.from(fileList).filter(f => /\.cpfont$/i.test(f.name));
    }

    document.getElementById('fontFiles').addEventListener('change', function() {
      const info = document.getElementById('pickedInfo');
      const files = cpfontFilesOnly(this.files);
      if (files.length === 0) {
        info.textContent = t('fonts.no_files_in_folder');
        return;
      }
      const family = sanitizeFamily(familyFromFilename(files[0].name));
      info.textContent = t('fonts.picked_summary', {
        count: files.length, plural: files.length === 1 ? '' : 's', family: family,
      });
    });

    document.getElementById('uploadForm').addEventListener('submit', async function(e) {
      e.preventDefault();
      const status = document.getElementById('status');
      const files = cpfontFilesOnly(document.getElementById('fontFiles').files);
      if (files.length === 0) {
        status.className = 'status-err';
        status.style.display = 'block';
        status.textContent = t('fonts.no_cpfont_selected');
        return;
      }

      // A directory picker may include files from multiple family subfolders.
      // Reject that up front — otherwise files[0]'s family is silently reused
      // for every upload, corrupting the install layout.
      const families = [...new Set(files.map(f => sanitizeFamily(familyFromFilename(f.name))))];
      if (families.length !== 1) {
        status.className = 'status-err';
        status.style.display = 'block';
        status.textContent = t('fonts.select_single_family');
        return;
      }
      const family = families[0];

      status.className = '';
      status.style.display = 'block';

      let uploaded = 0;
      for (const file of files) {
        status.textContent = t('fonts.uploading_progress', {current: uploaded + 1, total: files.length, name: file.name});
        const formData = new FormData();
        formData.append('family', family);
        formData.append('file', file, file.name);
        try {
          const res = await fetch('/api/fonts/upload', { method: 'POST', body: formData });
          const data = await res.json();
          if (!data.ok) {
            status.className = 'status-err';
            status.textContent = t('fonts.upload_failed_on', {name: file.name, msg: data.error || 'unknown error'});
            await loadFonts();
            return;
          }
        } catch (err) {
          status.className = 'status-err';
          status.textContent = t('fonts.upload_error_on', {name: file.name, msg: err.message});
          await loadFonts();
          return;
        }
        uploaded++;
      }

      status.className = 'status-ok';
      status.textContent = t('fonts.upload_done', {
        count: uploaded, plural: uploaded === 1 ? '' : 's', family: family,
      });
      await loadFonts();
    });

    loadFonts();
