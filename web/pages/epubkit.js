(function () {
  'use strict';

  const DEVICE_PROFILES = {
    x4: {width: 480, height: 800, levels: [0, 85, 170, 255]},
    x3: {width: 528, height: 792, levels: [0, 85, 170, 255]}
  };
  const FONT_EXTENSIONS = new Set(['ttf', 'otf', 'woff', 'woff2', 'eot']);
  const IMAGE_EXTENSIONS = new Set(['png', 'gif', 'webp', 'bmp', 'jpeg', 'jpg', 'tif', 'tiff']);
  const OS_ARTIFACTS = new Set(['.DS_Store', 'Thumbs.db', 'desktop.ini', '._.DS_Store']);
  const STORE_META_NAMES = new Set([
    'calibre:timestamp', 'calibre:title_sort', 'calibre:author_link_map',
    'calibre:series', 'calibre:series_index', 'calibre:rating',
    'calibre:user_categories', 'calibre:user_metadata', 'ibooks:version',
    'ibooks:specified-fonts', 'Sigil version', 'dtb:uid'
  ]);
  const STORE_META_PREFIXES = ['calibre:', 'ibooks:', 'amazon:', 'kindle:'];
  const TEXT_SKIP_TAGS = new Set(['script', 'style', 'pre', 'code', 'kbd', 'samp']);
  const KEEP_ATTRS = new Set([
    'class', 'id', 'href', 'src', 'style', 'alt', 'title', 'type', 'name',
    'content', 'charset', 'http-equiv', 'xmlns', 'version', 'media-type',
    'properties', 'rel', 'media', 'width', 'height', 'colspan', 'rowspan',
    'scope', 'headers', 'border', 'cellpadding', 'cellspacing', 'lang'
  ]);
  const encoder = new TextEncoder();
  const decoder = new TextDecoder('utf-8');
  const state = {
    books: [],
    device: 'x4',
    cancelled: false,
    processing: false,
    activeSocket: null,
    results: []
  };

  const el = id => document.getElementById(id);
  const ui = {
    drop: el('epubkitDropZone'), input: el('epubkitFileInput'), files: el('epubkitFileList'),
    options: el('epubkitOptions'), process: el('epubkitProcess'), cancel: el('epubkitCancel'),
    progress: el('epubkitProgress'), progressItems: el('epubkitProgressItems'),
    stageCounter: el('epubkitStageCounter'), results: el('epubkitResults'),
    resultItems: el('epubkitResultItems'), quality: el('epubkitQuality'),
    qualityValue: el('epubkitQualityValue'), upload: el('epubkitUpload'), path: el('epubkitPath')
  };

  function tr(key, fallback, values) {
    try {
      const translated = typeof t === 'function' ? t(key, values || {}) : key;
      return translated && translated !== key ? translated : fallback;
    } catch (_) {
      return fallback;
    }
  }

  function escapeHtml(value) {
    return String(value == null ? '' : value)
      .replaceAll('&', '&amp;').replaceAll('<', '&lt;').replaceAll('>', '&gt;')
      .replaceAll('"', '&quot;').replaceAll("'", '&#039;');
  }

  function formatBytes(bytes) {
    let value = Number(bytes) || 0;
    const units = ['B', 'KB', 'MB', 'GB'];
    let unit = 0;
    while (value >= 1024 && unit < units.length - 1) { value /= 1024; unit++; }
    return `${value.toFixed(unit ? 1 : 0)} ${units[unit]}`;
  }

  function extension(path) {
    const clean = String(path || '').split(/[?#]/)[0];
    const index = clean.lastIndexOf('.');
    return index < 0 ? '' : clean.slice(index + 1).toLowerCase();
  }

  function dirname(path) {
    const normalized = normalizeZipPath(path);
    const index = normalized.lastIndexOf('/');
    return index < 0 ? '' : normalized.slice(0, index);
  }

  function basename(path) {
    const normalized = normalizeZipPath(path);
    return normalized.slice(normalized.lastIndexOf('/') + 1);
  }

  function normalizeZipPath(path) {
    const output = [];
    for (const part of String(path || '').replace(/\\/g, '/').split('/')) {
      if (!part || part === '.') continue;
      if (part === '..') {
        if (!output.length) throw new Error('Unsafe path in EPUB');
        output.pop();
      } else {
        output.push(part);
      }
    }
    return output.join('/');
  }

  function decodeHref(value) {
    try { return decodeURIComponent(value); } catch (_) { return value; }
  }

  function resolveZipPath(fromFile, href) {
    const value = decodeHref(String(href || '')).split('#')[0].split('?')[0];
    if (!value) return '';
    return normalizeZipPath(`${dirname(fromFile)}/${value}`);
  }

  function relativeZipPath(fromFile, target) {
    const from = dirname(fromFile).split('/').filter(Boolean);
    const to = normalizeZipPath(target).split('/').filter(Boolean);
    while (from.length && to.length && from[0] === to[0]) { from.shift(); to.shift(); }
    return `${'../'.repeat(from.length)}${to.join('/')}` || basename(target);
  }

  function safeFilename(title, author, original) {
    let name = author && title ? `${author} - ${title}` : (title || author || original.replace(/\.epub$/i, '') || 'optimized');
    name = name.normalize('NFC').replace(/[\\/:*?"<>|\x00-\x1f\x7f]/g, ch => ({'/':'-','\\':'-',':':' -','|':'-','"':"'"}[ch] || ''));
    name = name.replace(/\s+/g, ' ').replace(/-{2,}/g, '-').trim().slice(0, 200).trim();
    return `${name || 'optimized'}.epub`;
  }

  function localName(node) {
    return String(node && (node.localName || node.nodeName) || '').replace(/^.*:/, '').toLowerCase();
  }

  function elementsByLocalName(root, name) {
    const target = name.toLowerCase();
    return Array.from(root.getElementsByTagName('*')).filter(node => localName(node) === target);
  }

  function firstByLocalName(root, name) {
    return elementsByLocalName(root, name)[0] || null;
  }

  function parseXml(text, label) {
    const source = String(text || '').replace(/&(?!#\d+;|#x[\da-f]+;|amp;|lt;|gt;|quot;|apos;)/gi, '&amp;');
    const doc = new DOMParser().parseFromString(source, 'application/xml');
    if (doc.getElementsByTagName('parsererror').length) {
      throw new Error(`${label || 'XML'} is malformed`);
    }
    return doc;
  }

  function parseRecoverableHtml(text) {
    try {
      return {doc: parseXml(text, 'XHTML'), recovered: false};
    } catch (_) {
      return {doc: new DOMParser().parseFromString(String(text || ''), 'text/html'), recovered: true};
    }
  }

  function serializeDocument(doc) {
    let output = new XMLSerializer().serializeToString(doc);
    if (!/^\s*<\?xml/i.test(output)) output = `<?xml version="1.0" encoding="utf-8"?>\n${output}`;
    return output;
  }

  function readU16(view, offset) { return view.getUint16(offset, true); }
  function readU32(view, offset) { return view.getUint32(offset, true); }
  function writeU16(view, offset, value) { view.setUint16(offset, value & 0xffff, true); }
  function writeU32(view, offset, value) { view.setUint32(offset, value >>> 0, true); }

  class LazyEpubZip {
    constructor(file, entries) {
      this.file = file;
      this.entries = entries;
      this.map = new Map(entries.map(entry => [entry.name, entry]));
      this.fallbackZip = null;
    }

    static async open(file) {
      const tailSize = Math.min(file.size, 65557);
      const tailOffset = file.size - tailSize;
      const tail = new Uint8Array(await file.slice(tailOffset).arrayBuffer());
      const tailView = new DataView(tail.buffer, tail.byteOffset, tail.byteLength);
      let eocd = -1;
      for (let offset = tail.length - 22; offset >= 0; offset--) {
        if (readU32(tailView, offset) === 0x06054b50 && offset + 22 + readU16(tailView, offset + 20) <= tail.length) {
          eocd = offset;
          break;
        }
      }
      if (eocd < 0) throw new Error(tr('epubkit.invalid_zip', 'Not a valid EPUB/ZIP file.'));
      const count = readU16(tailView, eocd + 10);
      const directorySize = readU32(tailView, eocd + 12);
      const directoryOffset = readU32(tailView, eocd + 16);
      if (count === 0xffff || directorySize === 0xffffffff || directoryOffset === 0xffffffff || directoryOffset + directorySize > file.size) {
        throw new Error(tr('epubkit.zip64', 'ZIP64 EPUB files are not supported.'));
      }
      const bytes = new Uint8Array(await file.slice(directoryOffset, directoryOffset + directorySize).arrayBuffer());
      const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
      const entries = [];
      let cursor = 0;
      for (let index = 0; index < count; index++) {
        if (cursor + 46 > bytes.length || readU32(view, cursor) !== 0x02014b50) throw new Error('Damaged ZIP directory');
        const nameLength = readU16(view, cursor + 28);
        const extraLength = readU16(view, cursor + 30);
        const commentLength = readU16(view, cursor + 32);
        const end = cursor + 46 + nameLength + extraLength + commentLength;
        if (end > bytes.length) throw new Error('Damaged ZIP entry');
        const flags = readU16(view, cursor + 8);
        let name = decoder.decode(bytes.subarray(cursor + 46, cursor + 46 + nameLength));
        name = name.replace(/\\/g, '/');
        if (name.startsWith('/') || name.split('/').includes('..')) throw new Error(`Unsafe path in EPUB: ${name}`);
        const compressedSize = readU32(view, cursor + 20);
        const uncompressedSize = readU32(view, cursor + 24);
        const localOffset = readU32(view, cursor + 42);
        if (compressedSize === 0xffffffff || uncompressedSize === 0xffffffff || localOffset === 0xffffffff) throw new Error('ZIP64 entry is not supported');
        entries.push({
          name: normalizeZipPath(name), dir: name.endsWith('/'), flags,
          method: readU16(view, cursor + 10), time: readU16(view, cursor + 12),
          date: readU16(view, cursor + 14), crc: readU32(view, cursor + 16),
          compressedSize, uncompressedSize, localOffset, dataOffset: null
        });
        cursor = end;
      }
      return new LazyEpubZip(file, entries);
    }

    get(path) { return this.map.get(normalizeZipPath(path)); }
    has(path) { return this.map.has(normalizeZipPath(path)); }

    async compressedBlob(entry) {
      if (entry.dataOffset == null) {
        const header = new Uint8Array(await this.file.slice(entry.localOffset, entry.localOffset + 30).arrayBuffer());
        if (header.length !== 30 || readU32(new DataView(header.buffer), 0) !== 0x04034b50) throw new Error(`Bad local ZIP header: ${entry.name}`);
        const view = new DataView(header.buffer);
        entry.dataOffset = entry.localOffset + 30 + readU16(view, 26) + readU16(view, 28);
      }
      return this.file.slice(entry.dataOffset, entry.dataOffset + entry.compressedSize);
    }

    async readBytes(pathOrEntry) {
      const entry = typeof pathOrEntry === 'string' ? this.get(pathOrEntry) : pathOrEntry;
      if (!entry || entry.dir) throw new Error(`Missing EPUB entry: ${typeof pathOrEntry === 'string' ? pathOrEntry : ''}`);
      if (entry.flags & 1) throw new Error(tr('epubkit.encrypted', 'The EPUB contains encrypted ZIP entries.'));
      const compressed = await this.compressedBlob(entry);
      if (entry.method === 0) return new Uint8Array(await compressed.arrayBuffer());
      if (entry.method !== 8) throw new Error(`Unsupported ZIP compression method ${entry.method}`);
      try {
        const stream = compressed.stream().pipeThrough(new DecompressionStream('deflate-raw'));
        return new Uint8Array(await new Response(stream).arrayBuffer());
      } catch (error) {
        if (typeof JSZip === 'undefined') throw error;
        if (!this.fallbackZip) this.fallbackZip = JSZip.loadAsync(this.file);
        const zip = await this.fallbackZip;
        const fallback = zip.file(entry.name);
        if (!fallback) throw error;
        return fallback.async('uint8array');
      }
    }

    async readText(pathOrEntry) { return decoder.decode(await this.readBytes(pathOrEntry)); }
    async readBlob(pathOrEntry, type) { return new Blob([await this.readBytes(pathOrEntry)], {type: type || ''}); }
  }

  function findOpfPath(zip, containerText) {
    if (containerText) {
      try {
        const doc = parseXml(containerText, 'container.xml');
        const rootfile = firstByLocalName(doc, 'rootfile');
        const path = rootfile && rootfile.getAttribute('full-path');
        if (path && zip.has(path)) return normalizeZipPath(path);
      } catch (_) { /* fallback below */ }
    }
    const entry = zip.entries.find(item => !item.dir && extension(item.name) === 'opf');
    if (!entry) throw new Error(tr('epubkit.no_opf', 'No OPF package document was found.'));
    return entry.name;
  }

  function metadataFromOpf(opfDoc) {
    const textOf = name => {
      const node = firstByLocalName(opfDoc, name);
      return node ? (node.textContent || '').trim() : '';
    };
    const metadata = {title: textOf('title'), author: textOf('creator'), language: textOf('language'), series: '', seriesIndex: '', coverId: '', coverHref: ''};
    for (const meta of elementsByLocalName(opfDoc, 'meta')) {
      const name = meta.getAttribute('name') || '';
      const property = meta.getAttribute('property') || '';
      if (name === 'calibre:series') metadata.series = meta.getAttribute('content') || '';
      if (name === 'calibre:series_index') metadata.seriesIndex = meta.getAttribute('content') || '';
      if (property === 'belongs-to-collection') metadata.series = (meta.textContent || '').trim();
      if (property === 'group-position') metadata.seriesIndex = (meta.textContent || '').trim();
      if (name === 'cover') metadata.coverId = meta.getAttribute('content') || '';
    }
    const items = elementsByLocalName(opfDoc, 'item');
    let coverItem = items.find(item => (item.getAttribute('properties') || '').split(/\s+/).includes('cover-image'));
    if (!coverItem && metadata.coverId) coverItem = items.find(item => item.getAttribute('id') === metadata.coverId);
    if (!coverItem) coverItem = items.find(item => /cover/i.test(item.getAttribute('id') || '') && /^image\//i.test(item.getAttribute('media-type') || ''));
    if (coverItem) {
      metadata.coverId = coverItem.getAttribute('id') || metadata.coverId;
      metadata.coverHref = coverItem.getAttribute('href') || '';
    }
    return metadata;
  }

  async function inspectBook(file) {
    const book = {id: `epubkit-${Date.now()}-${Math.random().toString(36).slice(2)}`, file, metadata: {}, coverUrl: '', error: ''};
    try {
      const zip = await LazyEpubZip.open(file);
      const container = zip.has('META-INF/container.xml') ? await zip.readText('META-INF/container.xml') : '';
      const opfPath = findOpfPath(zip, container);
      const opfDoc = parseXml(await zip.readText(opfPath), 'OPF');
      book.metadata = metadataFromOpf(opfDoc);
      book.opfPath = opfPath;
      if (book.metadata.coverHref) {
        const coverPath = resolveZipPath(opfPath, book.metadata.coverHref);
        if (zip.has(coverPath)) {
          book.coverUrl = URL.createObjectURL(await zip.readBlob(coverPath, `image/${extension(coverPath)}`));
        }
      }
    } catch (error) {
      book.error = error.message || String(error);
    }
    return book;
  }

  async function addFiles(fileList) {
    const picked = Array.from(fileList || []).filter(file => /\.epub$/i.test(file.name));
    for (const file of picked) {
      const book = await inspectBook(file);
      state.books.push(book);
      renderBooks();
      await new Promise(resolve => setTimeout(resolve, 0));
    }
  }

  function renderBooks() {
    ui.files.innerHTML = state.books.map(book => {
      const meta = book.metadata || {};
      const cover = book.coverUrl ? `<img src="${book.coverUrl}" alt="">` : `<span>${escapeHtml(tr('epubkit.no_cover', 'No cover'))}</span>`;
      const detail = [meta.author, meta.series, formatBytes(book.file.size)].filter(Boolean).join(' · ');
      return `<article class="epubkit-file-card" data-book="${book.id}">
        <div class="epubkit-cover">${cover}</div>
        <div class="epubkit-file-main">
          <div class="epubkit-file-name">${escapeHtml(meta.title || book.file.name)}</div>
          <div class="epubkit-file-meta">${escapeHtml(detail)}</div>
          ${book.error ? `<div class="epubkit-file-error">${escapeHtml(book.error)}</div>` : `<div class="epubkit-file-edit">
            <input data-edit="title" value="${escapeHtml(meta.title || '')}" placeholder="${escapeHtml(tr('epubkit.title', 'Title'))}">
            <input data-edit="author" value="${escapeHtml(meta.author || '')}" placeholder="${escapeHtml(tr('epubkit.author', 'Author'))}">
          </div>`}
        </div>
        <button class="epubkit-remove" type="button" data-remove="${book.id}" aria-label="Remove">×</button>
      </article>`;
    }).join('');
    ui.files.hidden = state.books.length === 0;
    ui.options.hidden = !state.books.some(book => !book.error);
  }

  function removeBook(id) {
    const index = state.books.findIndex(book => book.id === id);
    if (index < 0 || state.processing) return;
    const [book] = state.books.splice(index, 1);
    if (book.coverUrl) URL.revokeObjectURL(book.coverUrl);
    renderBooks();
  }

  function setPreset(preset) {
    const values = preset === 'quick'
      ? {grayscale: true, contrast: true, fonts: false, css: false, cover: false, metadata: false, text: true, light: false, quality: 70}
      : {grayscale: true, contrast: true, fonts: true, css: true, cover: true, metadata: true, text: true, light: false, quality: 70};
    if (preset !== 'custom') {
      el('epubkitGrayscale').checked = values.grayscale;
      el('epubkitContrast').checked = values.contrast;
      el('epubkitFonts').checked = values.fonts;
      el('epubkitCss').checked = values.css;
      el('epubkitCover').checked = values.cover;
      el('epubkitMetadata').checked = values.metadata;
      el('epubkitText').checked = values.text;
      el('epubkitLightNovel').checked = values.light;
      setQuality(values.quality, false);
    }
    document.querySelectorAll('#epubkitPresets button').forEach(button => button.classList.toggle('active', button.dataset.preset === preset));
  }

  function setQuality(value, custom) {
    const quality = Math.max(20, Math.min(95, Number(value) || 70));
    ui.quality.value = quality;
    ui.qualityValue.textContent = `${quality}%`;
    document.querySelectorAll('.epubkit-quality-buttons button').forEach(button => button.classList.toggle('active', Number(button.dataset.quality) === quality));
    if (custom !== false) setPreset('custom');
  }

  function optionsFromUi(book) {
    const card = document.querySelector(`[data-book="${book.id}"]`);
    const edit = field => card && card.querySelector(`[data-edit="${field}"]`) ? card.querySelector(`[data-edit="${field}"]`).value.trim() : '';
    return {
      device: state.device, grayscale: el('epubkitGrayscale').checked,
      contrast: el('epubkitContrast').checked, removeFonts: el('epubkitFonts').checked,
      removeCss: el('epubkitCss').checked, generateCover: el('epubkitCover').checked,
      cleanMetadata: el('epubkitMetadata').checked, textCleanup: el('epubkitText').checked,
      lightNovel: el('epubkitLightNovel').checked, quality: Number(ui.quality.value) || 70,
      title: edit('title'), author: edit('author')
    };
  }

  ui.drop.addEventListener('click', () => !state.processing && ui.input.click());
  ui.drop.addEventListener('keydown', event => { if ((event.key === 'Enter' || event.key === ' ') && !state.processing) ui.input.click(); });
  ui.drop.addEventListener('dragover', event => { event.preventDefault(); ui.drop.classList.add('dragover'); });
  ui.drop.addEventListener('dragleave', () => ui.drop.classList.remove('dragover'));
  ui.drop.addEventListener('drop', event => { event.preventDefault(); ui.drop.classList.remove('dragover'); if (!state.processing) addFiles(event.dataTransfer.files); });
  ui.input.addEventListener('change', () => { addFiles(ui.input.files); ui.input.value = ''; });
  ui.files.addEventListener('click', event => { const button = event.target.closest('[data-remove]'); if (button) removeBook(button.dataset.remove); });
  document.querySelectorAll('#epubkitDevice button').forEach(button => button.addEventListener('click', () => {
    state.device = button.dataset.device;
    document.querySelectorAll('#epubkitDevice button').forEach(item => item.classList.toggle('active', item === button));
  }));
  document.querySelectorAll('#epubkitPresets button').forEach(button => button.addEventListener('click', () => setPreset(button.dataset.preset)));
  document.querySelectorAll('.epubkit-options-grid input').forEach(input => input.addEventListener('change', () => setPreset('custom')));
  document.querySelectorAll('.epubkit-quality-buttons button').forEach(button => button.addEventListener('click', () => setQuality(button.dataset.quality)));
  ui.quality.addEventListener('input', () => setQuality(ui.quality.value));
  ui.upload.addEventListener('change', () => { ui.path.closest('.epubkit-path').style.opacity = ui.upload.checked ? '1' : '.5'; ui.path.disabled = !ui.upload.checked; });
  ui.cancel.addEventListener('click', () => { state.cancelled = true; if (state.activeSocket) state.activeSocket.close(); });

  const CRC_TABLE = (() => {
    const table = new Uint32Array(256);
    for (let i = 0; i < 256; i++) {
      let value = i;
      for (let bit = 0; bit < 8; bit++) value = (value & 1) ? (0xedb88320 ^ (value >>> 1)) : (value >>> 1);
      table[i] = value >>> 0;
    }
    return table;
  })();

  function crc32(bytes) {
    let crc = 0xffffffff;
    for (let i = 0; i < bytes.length; i++) crc = CRC_TABLE[(crc ^ bytes[i]) & 0xff] ^ (crc >>> 8);
    return (crc ^ 0xffffffff) >>> 0;
  }

  async function deflateRaw(bytes) {
    try {
      const stream = new Blob([bytes]).stream().pipeThrough(new CompressionStream('deflate-raw'));
      return new Uint8Array(await new Response(stream).arrayBuffer());
    } catch (_) {
      return null;
    }
  }

  function dosDateTime(date) {
    const value = date || new Date();
    const year = Math.max(1980, value.getFullYear());
    return {
      time: ((value.getHours() & 31) << 11) | ((value.getMinutes() & 63) << 5) | ((Math.floor(value.getSeconds() / 2)) & 31),
      date: (((year - 1980) & 127) << 9) | (((value.getMonth() + 1) & 15) << 5) | (value.getDate() & 31)
    };
  }

  class EpubZipBuilder {
    constructor() {
      this.parts = [];
      this.central = [];
      this.offset = 0;
    }

    append(part) {
      const blob = part instanceof Blob ? part : new Blob([part]);
      this.parts.push(blob);
      this.offset += blob.size;
    }

    localHeader(record) {
      const name = encoder.encode(record.name);
      const bytes = new Uint8Array(30 + name.length);
      const view = new DataView(bytes.buffer);
      writeU32(view, 0, 0x04034b50); writeU16(view, 4, 20); writeU16(view, 6, 0x0800);
      writeU16(view, 8, record.method); writeU16(view, 10, record.time); writeU16(view, 12, record.date);
      writeU32(view, 14, record.crc); writeU32(view, 18, record.compressedSize); writeU32(view, 22, record.uncompressedSize);
      writeU16(view, 26, name.length); writeU16(view, 28, 0); bytes.set(name, 30);
      return bytes;
    }

    centralHeader(record) {
      const name = encoder.encode(record.name);
      const bytes = new Uint8Array(46 + name.length);
      const view = new DataView(bytes.buffer);
      writeU32(view, 0, 0x02014b50); writeU16(view, 4, 20); writeU16(view, 6, 20); writeU16(view, 8, 0x0800);
      writeU16(view, 10, record.method); writeU16(view, 12, record.time); writeU16(view, 14, record.date);
      writeU32(view, 16, record.crc); writeU32(view, 20, record.compressedSize); writeU32(view, 24, record.uncompressedSize);
      writeU16(view, 28, name.length); writeU16(view, 30, 0); writeU16(view, 32, 0); writeU16(view, 34, 0);
      writeU16(view, 36, 0); writeU32(view, 38, 0); writeU32(view, 42, record.offset); bytes.set(name, 46);
      return bytes;
    }

    addRecord(record, compressedBlob) {
      if (this.central.length >= 65535 || this.offset + compressedBlob.size >= 0xffffffff) throw new Error('The optimized EPUB exceeds the classic ZIP format limit.');
      record.offset = this.offset;
      this.append(this.localHeader(record));
      this.append(compressedBlob);
      this.central.push(record);
    }

    async addCopied(entry, compressedBlob) {
      this.addRecord({
        name: entry.name, method: entry.method, time: entry.time, date: entry.date,
        crc: entry.crc, compressedSize: entry.compressedSize, uncompressedSize: entry.uncompressedSize
      }, compressedBlob);
    }

    async addData(name, value, compress) {
      const bytes = typeof value === 'string'
        ? encoder.encode(value)
        : (ArrayBuffer.isView(value)
            ? new Uint8Array(value.buffer, value.byteOffset, value.byteLength)
            : new Uint8Array(await value.arrayBuffer()));
      let payload = null;
      let method = 0;
      if (compress !== false && bytes.length > 48) {
        const compressed = await deflateRaw(bytes);
        if (compressed && compressed.length < bytes.length) { payload = compressed; method = 8; }
      }
      if (!payload) payload = bytes;
      const dt = dosDateTime(new Date());
      this.addRecord({name: normalizeZipPath(name), method, time: dt.time, date: dt.date, crc: crc32(bytes), compressedSize: payload.length, uncompressedSize: bytes.length}, new Blob([payload]));
    }

    finish() {
      const directoryOffset = this.offset;
      for (const record of this.central) this.append(this.centralHeader(record));
      const directorySize = this.offset - directoryOffset;
      const end = new Uint8Array(22);
      const view = new DataView(end.buffer);
      writeU32(view, 0, 0x06054b50); writeU16(view, 4, 0); writeU16(view, 6, 0);
      writeU16(view, 8, this.central.length); writeU16(view, 10, this.central.length);
      writeU32(view, 12, directorySize); writeU32(view, 16, directoryOffset); writeU16(view, 20, 0);
      this.append(end);
      return new Blob(this.parts, {type: 'application/epub+zip'});
    }
  }

  function canvasToBlob(canvas, quality) {
    return new Promise((resolve, reject) => canvas.toBlob(blob => blob ? resolve(blob) : reject(new Error('JPEG encoder failed')), 'image/jpeg', quality / 100));
  }

  async function imageDimensions(blob) {
    const bytes = new Uint8Array(await blob.slice(0, Math.min(blob.size, 65536)).arrayBuffer());
    if (bytes.length >= 24 && bytes[0] === 0x89 && decoder.decode(bytes.subarray(1, 4)) === 'PNG') {
      const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
      return {width: view.getUint32(16, false), height: view.getUint32(20, false)};
    }
    if (bytes.length >= 10 && (decoder.decode(bytes.subarray(0, 3)) === 'GIF')) {
      const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
      return {width: view.getUint16(6, true), height: view.getUint16(8, true)};
    }
    if (bytes.length >= 26 && bytes[0] === 0x42 && bytes[1] === 0x4d) {
      const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
      return {width: Math.abs(view.getInt32(18, true)), height: Math.abs(view.getInt32(22, true))};
    }
    if (bytes.length >= 12 && bytes[0] === 0xff && bytes[1] === 0xd8) {
      let cursor = 2;
      while (cursor + 9 < bytes.length) {
        if (bytes[cursor] !== 0xff) { cursor++; continue; }
        const marker = bytes[cursor + 1];
        if (marker === 0xd8 || marker === 0xd9) { cursor += 2; continue; }
        const length = (bytes[cursor + 2] << 8) | bytes[cursor + 3];
        if (length < 2 || cursor + 2 + length > bytes.length) break;
        if ((marker >= 0xc0 && marker <= 0xc3) || (marker >= 0xc5 && marker <= 0xc7) || (marker >= 0xc9 && marker <= 0xcb) || (marker >= 0xcd && marker <= 0xcf)) {
          return {height: (bytes[cursor + 5] << 8) | bytes[cursor + 6], width: (bytes[cursor + 7] << 8) | bytes[cursor + 8]};
        }
        cursor += 2 + length;
      }
    }
    return null;
  }

  async function decodeImage(blob, options) {
    if (typeof createImageBitmap === 'function') {
      try {
        const dimensions = await imageDimensions(blob);
        if (dimensions && dimensions.width && dimensions.height) {
          const profile = DEVICE_PROFILES[options.device] || DEVICE_PROFILES.x4;
          let boxWidth = profile.width, boxHeight = profile.height;
          if (options.lightNovel && dimensions.width > dimensions.height) {
            if (dimensions.width / dimensions.height > 1.8) boxWidth = profile.width * 2;
            else { boxWidth = profile.height; boxHeight = profile.width; }
          }
          const scale = Math.min(1, boxWidth / dimensions.width, boxHeight / dimensions.height, 1024 / dimensions.width, 1024 / dimensions.height);
          if (scale < 1) {
            return await createImageBitmap(blob, {
              resizeWidth: Math.max(1, Math.round(dimensions.width * scale)),
              resizeHeight: Math.max(1, Math.round(dimensions.height * scale)),
              resizeQuality: 'high'
            });
          }
        }
      } catch (_) { /* retry with the browser's normal decoder */ }
      return createImageBitmap(blob);
    }
    return new Promise((resolve, reject) => {
      const url = URL.createObjectURL(blob);
      const image = new Image();
      image.onload = () => { URL.revokeObjectURL(url); resolve(image); };
      image.onerror = () => { URL.revokeObjectURL(url); reject(new Error('Unsupported or damaged image')); };
      image.src = url;
    });
  }

  function enhancePixels(ctx, width, height, options) {
    const image = ctx.getImageData(0, 0, width, height);
    const data = image.data;
    const count = width * height;
    if (options.grayscale) {
      const histogram = options.contrast ? new Uint32Array(256) : null;
      for (let index = 0; index < data.length; index += 4) {
        const gray = Math.max(0, Math.min(255, Math.round(data[index] * .299 + data[index + 1] * .587 + data[index + 2] * .114)));
        data[index] = data[index + 1] = data[index + 2] = gray;
        if (histogram) histogram[gray]++;
      }
      let low = 0, high = 255;
      if (histogram) {
        const cutoff = Math.floor(count * .01);
        let sum = 0;
        while (low < 255 && sum + histogram[low] <= cutoff) sum += histogram[low++];
        sum = 0;
        while (high > 0 && sum + histogram[high] <= cutoff) sum += histogram[high--];
        if (high <= low) { low = 0; high = 255; }
      }
      let current = new Float32Array(width + 2);
      let next = new Float32Array(width + 2);
      for (let y = 0; y < height; y++) {
        next.fill(0);
        for (let x = 0; x < width; x++) {
          const offset = (y * width + x) * 4;
          let gray = data[offset];
          if (options.contrast) {
            gray = (gray - low) * 255 / Math.max(1, high - low);
            gray = (gray - 128) * 1.5 + 128;
          }
          gray = Math.max(0, Math.min(255, gray + current[x + 1]));
          const quantized = Math.max(0, Math.min(255, Math.round(gray / 85) * 85));
          data[offset] = data[offset + 1] = data[offset + 2] = quantized;
          data[offset + 3] = 255;
          const error = gray - quantized;
          current[x + 2] += error * 7 / 16;
          next[x] += error * 3 / 16;
          next[x + 1] += error * 5 / 16;
          next[x + 2] += error / 16;
        }
        const swap = current; current = next; next = swap;
      }
    } else if (options.contrast) {
      for (let index = 0; index < data.length; index += 4) {
        data[index] = Math.max(0, Math.min(255, (data[index] - 128) * 1.5 + 128));
        data[index + 1] = Math.max(0, Math.min(255, (data[index + 1] - 128) * 1.5 + 128));
        data[index + 2] = Math.max(0, Math.min(255, (data[index + 2] - 128) * 1.5 + 128));
        data[index + 3] = 255;
      }
    }
    ctx.putImageData(image, 0, 0);
  }

  async function processImage(blob, path, options) {
    const image = await decodeImage(blob, options);
    const sourceWidth = image.width || image.naturalWidth;
    const sourceHeight = image.height || image.naturalHeight;
    if (!sourceWidth || !sourceHeight) throw new Error(`Cannot decode ${basename(path)}`);
    let slices = [{x: 0, y: 0, width: sourceWidth, height: sourceHeight, rotate: false, suffix: ''}];
    if (options.lightNovel && sourceWidth > sourceHeight) {
      if (sourceWidth / sourceHeight > 1.8) {
        const middle = Math.floor(sourceWidth / 2);
        slices = [
          {x: middle, y: 0, width: sourceWidth - middle, height: sourceHeight, rotate: false, suffix: '_part1'},
          {x: 0, y: 0, width: middle, height: sourceHeight, rotate: false, suffix: '_part2'}
        ];
      } else {
        slices = [{x: 0, y: 0, width: sourceWidth, height: sourceHeight, rotate: true, suffix: ''}];
      }
    }
    const profile = DEVICE_PROFILES[options.device] || DEVICE_PROFILES.x4;
    const stem = basename(path).replace(/\.[^.]+$/, '');
    const folder = dirname(path);
    const output = [];
    for (const slice of slices) {
      if (state.cancelled) throw new Error(tr('epubkit.cancelled', 'Cancelled.'));
      const orientedWidth = slice.rotate ? slice.height : slice.width;
      const orientedHeight = slice.rotate ? slice.width : slice.height;
      const scale = Math.min(1, profile.width / orientedWidth, profile.height / orientedHeight, 1024 / orientedWidth, 1024 / orientedHeight);
      const width = Math.max(1, Math.round(orientedWidth * scale));
      const height = Math.max(1, Math.round(orientedHeight * scale));
      const canvas = document.createElement('canvas');
      canvas.width = width; canvas.height = height;
      const ctx = canvas.getContext('2d', {willReadFrequently: true});
      ctx.fillStyle = '#fff'; ctx.fillRect(0, 0, width, height);
      ctx.imageSmoothingEnabled = true; ctx.imageSmoothingQuality = 'high';
      if (slice.rotate) {
        ctx.save(); ctx.translate(0, height); ctx.rotate(-Math.PI / 2);
        ctx.drawImage(image, slice.x, slice.y, slice.width, slice.height, 0, 0, height, width);
        ctx.restore();
      } else {
        ctx.drawImage(image, slice.x, slice.y, slice.width, slice.height, 0, 0, width, height);
      }
      enhancePixels(ctx, width, height, options);
      const encoded = await canvasToBlob(canvas, options.quality);
      canvas.width = canvas.height = 1;
      output.push({path: normalizeZipPath(`${folder ? folder + '/' : ''}${stem}${slice.suffix}.jpg`), blob: encoded, width, height});
      await new Promise(resolve => setTimeout(resolve, 0));
    }
    if (typeof image.close === 'function') image.close();
    return output;
  }

  async function generateCover(title, author, options) {
    const profile = DEVICE_PROFILES[options.device] || DEVICE_PROFILES.x4;
    const canvas = document.createElement('canvas');
    canvas.width = profile.width; canvas.height = profile.height;
    const ctx = canvas.getContext('2d', {willReadFrequently: true});
    ctx.fillStyle = '#fff'; ctx.fillRect(0, 0, canvas.width, canvas.height);
    ctx.strokeStyle = '#777'; ctx.lineWidth = 3; ctx.strokeRect(25, 25, canvas.width - 50, canvas.height - 50);
    ctx.fillStyle = '#222'; ctx.textAlign = 'center';
    const drawWrapped = (text, y, maxWidth, font, lineHeight, maxLines) => {
      ctx.font = font;
      const words = String(text || '').split(/\s+/).filter(Boolean);
      const lines = [];
      let line = '';
      for (const word of words) {
        const candidate = line ? `${line} ${word}` : word;
        if (ctx.measureText(candidate).width > maxWidth && line) { lines.push(line); line = word; }
        else line = candidate;
      }
      if (line) lines.push(line);
      lines.slice(0, maxLines).forEach((value, index) => ctx.fillText(value, canvas.width / 2, y + index * lineHeight));
    };
    drawWrapped(title || 'Untitled', canvas.height * .36, canvas.width - 80, 'bold 34px sans-serif', 42, 5);
    drawWrapped(author || '', canvas.height * .72, canvas.width - 100, '22px sans-serif', 28, 3);
    enhancePixels(ctx, canvas.width, canvas.height, {...options, grayscale: true});
    const blob = await canvasToBlob(canvas, options.quality);
    canvas.width = canvas.height = 1;
    return blob;
  }

  function manifestInfo(opfDoc, opfPath) {
    return elementsByLocalName(opfDoc, 'item').map(element => {
      const href = element.getAttribute('href') || '';
      let path = '';
      try { path = resolveZipPath(opfPath, href); } catch (_) { /* invalid manifest path */ }
      return {
        element, id: element.getAttribute('id') || '', href, path,
        mediaType: (element.getAttribute('media-type') || '').toLowerCase(),
        properties: (element.getAttribute('properties') || '').split(/\s+/).filter(Boolean)
      };
    });
  }

  function findCoverItem(opfDoc) {
    const items = elementsByLocalName(opfDoc, 'item');
    let coverId = '';
    for (const meta of elementsByLocalName(opfDoc, 'meta')) {
      if ((meta.getAttribute('name') || '').toLowerCase() === 'cover') coverId = meta.getAttribute('content') || '';
    }
    return items.find(item => (item.getAttribute('properties') || '').split(/\s+/).includes('cover-image'))
      || items.find(item => coverId && item.getAttribute('id') === coverId)
      || items.find(item => /cover/i.test(item.getAttribute('id') || '') && /^image\//i.test(item.getAttribute('media-type') || ''))
      || null;
  }

  function appendOpfElement(opfDoc, parent, name) {
    return parent.appendChild(opfDoc.createElementNS(parent.namespaceURI || opfDoc.documentElement.namespaceURI || 'http://www.idpf.org/2007/opf', name));
  }

  function setDcMetadata(opfDoc, name, value) {
    if (!value) return;
    let node = firstByLocalName(opfDoc, name);
    if (!node) {
      const metadata = firstByLocalName(opfDoc, 'metadata');
      if (!metadata) return;
      node = opfDoc.createElementNS('http://purl.org/dc/elements/1.1/', `dc:${name}`);
      metadata.appendChild(node);
    }
    node.textContent = value;
  }

  function makeUniquePath(candidate, occupied, original) {
    let path = normalizeZipPath(candidate);
    if (!occupied.has(path) || path === original) { occupied.add(path); return path; }
    const ext = extension(path);
    const base = ext ? path.slice(0, -(ext.length + 1)) : path;
    let counter = 1;
    do { path = `${base}_epubkit${counter > 1 ? counter : ''}${ext ? '.' + ext : ''}`; counter++; } while (occupied.has(path));
    occupied.add(path);
    return path;
  }

  function isExternalReference(value) {
    return !value || value.startsWith('#') || /^(?:data|https?|mailto|tel|javascript):/i.test(value);
  }

  function rewriteReference(value, fromPath, renames) {
    if (isExternalReference(value)) return null;
    const hashIndex = value.indexOf('#');
    const queryIndex = value.indexOf('?');
    let suffixAt = value.length;
    if (hashIndex >= 0) suffixAt = Math.min(suffixAt, hashIndex);
    if (queryIndex >= 0) suffixAt = Math.min(suffixAt, queryIndex);
    const suffix = value.slice(suffixAt);
    let resolved;
    try { resolved = resolveZipPath(fromPath, value.slice(0, suffixAt)); } catch (_) { return null; }
    const targets = renames.get(resolved);
    if (!targets || !targets.length) return null;
    return {targets, values: targets.map(path => relativeZipPath(fromPath, path) + suffix)};
  }

  function rewriteCssUrls(css, cssPath, renames) {
    return String(css || '').replace(/url\(\s*(["']?)([^)'"\s]+)\1\s*\)/gi, (full, quote, url) => {
      const rewritten = rewriteReference(url, cssPath, renames);
      return rewritten ? `url(${quote}${rewritten.values[0]}${quote})` : full;
    });
  }

  function fixSvgCovers(doc) {
    let count = 0;
    for (const svg of elementsByLocalName(doc, 'svg')) {
      const images = Array.from(svg.children || []).filter(node => localName(node) === 'image');
      if (images.length !== 1 || !svg.parentNode) continue;
      const source = images[0].getAttribute('href') || images[0].getAttribute('xlink:href') || images[0].getAttributeNS('http://www.w3.org/1999/xlink', 'href');
      if (!source) continue;
      const image = doc.createElementNS(doc.documentElement.namespaceURI || 'http://www.w3.org/1999/xhtml', 'img');
      image.setAttribute('src', source); image.setAttribute('alt', 'Cover');
      image.setAttribute('style', 'max-width:100%;max-height:100%;display:block;margin:auto');
      svg.parentNode.replaceChild(image, svg); count++;
    }
    return count;
  }

  function updateImageReferences(doc, xhtmlPath, renames) {
    for (const image of Array.from(elementsByLocalName(doc, 'img'))) {
      const source = image.getAttribute('src') || '';
      const rewritten = rewriteReference(source, xhtmlPath, renames);
      if (!rewritten) continue;
      image.setAttribute('src', rewritten.values[0]);
      let insertionPoint = image;
      for (let index = 1; index < rewritten.values.length; index++) {
        const clone = image.cloneNode(true);
        clone.setAttribute('src', rewritten.values[index]);
        if (insertionPoint.parentNode) insertionPoint.parentNode.insertBefore(clone, insertionPoint.nextSibling);
        insertionPoint = clone;
      }
    }
    for (const image of elementsByLocalName(doc, 'image')) {
      const attribute = image.hasAttribute('href') ? 'href' : 'xlink:href';
      const source = image.getAttribute(attribute) || image.getAttributeNS('http://www.w3.org/1999/xlink', 'href') || '';
      const rewritten = rewriteReference(source, xhtmlPath, renames);
      if (rewritten) image.setAttribute(attribute, rewritten.values[0]);
    }
    for (const node of Array.from(doc.getElementsByTagName('*'))) {
      const style = node.getAttribute && node.getAttribute('style');
      if (style && /url\(/i.test(style)) node.setAttribute('style', rewriteCssUrls(style, xhtmlPath, renames));
    }
  }

  function stripAttributes(doc) {
    let removed = 0;
    const discard = new Set(['role', 'tabindex', 'accesskey', 'draggable', 'contenteditable', 'spellcheck', 'autocorrect', 'autocapitalize', 'autofocus', 'dir', 'translate', 'inputmode', 'enterkeyhint', 'hidden', 'inert', 'popover']);
    for (const node of Array.from(doc.getElementsByTagName('*'))) {
      for (const attribute of Array.from(node.attributes || [])) {
        const name = (attribute.localName || attribute.name).toLowerCase();
        if (KEEP_ATTRS.has(name) || name === 'href' || name === 'src' || name === 'type' || name === 'lang') continue;
        if (name.startsWith('data-') || name.startsWith('aria-') || attribute.name.toLowerCase().startsWith('epub:') || discard.has(name)) {
          node.removeAttributeNode(attribute); removed++;
        }
      }
    }
    return removed;
  }

  function normalizeEmptyBlocks(doc) {
    let removed = 0;
    for (const parent of Array.from(doc.getElementsByTagName('*'))) {
      let emptySeen = false;
      for (const child of Array.from(parent.children || [])) {
        const tag = localName(child);
        const empty = (tag === 'p' || tag === 'div') && !child.children.length && !(child.textContent || '').trim();
        if (!empty) { emptySeen = false; continue; }
        if (emptySeen) { child.remove(); removed++; } else emptySeen = true;
      }
    }
    return removed;
  }

  function ensureChapterBreakStyle(doc) {
    const head = firstByLocalName(doc, 'head');
    if (!head) return;
    const exists = elementsByLocalName(head, 'style').some(style => /page-break-before/i.test(style.textContent || ''));
    if (exists) return;
    const style = doc.createElementNS(doc.documentElement.namespaceURI || 'http://www.w3.org/1999/xhtml', 'style');
    style.setAttribute('type', 'text/css'); style.textContent = '\nh1, h2 { page-break-before: always; }\n';
    head.appendChild(style);
  }

  function cleanTextNodes(doc) {
    const counts = {spaces: 0, ligatures: 0, quotes: 0, encoding: 0, punctuation: 0, unicode: 0, total: 0};
    const ligatures = {'\ufb00':'ff','\ufb01':'fi','\ufb02':'fl','\ufb03':'ffi','\ufb04':'ffl'};
    const quotes = {'\u2018':"'",'\u2019':"'",'\u201c':'"','\u201d':'"','\u2014':'--','\u2013':'-','\u2026':'...','\u00a0':' ','\u201a':','};
    const mojibake = {'Ã©':'é','Ã¨':'è','Ã«':'ë','Ã ':'à','Ã¼':'ü','Ã±':'ñ','Ã§':'ç','Ã¶':'ö','Ã¤':'ä','Â£':'£','Â»':'»','Â«':'«','Â°':'°'};
    const walker = doc.createTreeWalker(doc, NodeFilter.SHOW_TEXT);
    const nodes = [];
    while (walker.nextNode()) nodes.push(walker.currentNode);
    for (const node of nodes) {
      if (!node.parentElement || TEXT_SKIP_TAGS.has(localName(node.parentElement))) continue;
      let text = node.nodeValue || '';
      let matches = text.match(/[ \t]{2,}/g); counts.spaces += matches ? matches.length : 0;
      text = text.replace(/[ \t]{2,}/g, ' ');
      matches = text.match(/\s+([.,;:!?])/g); counts.spaces += matches ? matches.length : 0;
      text = text.replace(/\s+([.,;:!?])/g, '$1');
      for (const [from, to] of Object.entries(ligatures)) { const n = text.split(from).length - 1; if (n) { text = text.split(from).join(to); counts.ligatures += n; } }
      for (const [from, to] of Object.entries(quotes)) { const n = text.split(from).length - 1; if (n) { text = text.split(from).join(to); counts.quotes += n; } }
      for (const [from, to] of Object.entries(mojibake)) { const n = text.split(from).length - 1; if (n) { text = text.split(from).join(to); counts.encoding += n; } }
      matches = text.match(/\.{4,}/g); counts.punctuation += matches ? matches.length : 0; text = text.replace(/\.{4,}/g, '...');
      matches = text.match(/([.!?])([A-Z])/g); counts.punctuation += matches ? matches.length : 0; text = text.replace(/([.!?])([A-Z])/g, '$1 $2');
      matches = text.match(/,{2,}/g); counts.punctuation += matches ? matches.length : 0; text = text.replace(/,{2,}/g, ',');
      matches = text.match(/([!?])\1{3,}/g); counts.punctuation += matches ? matches.length : 0; text = text.replace(/([!?])\1{3,}/g, '$1$1$1');
      const normalized = text.normalize('NFC'); if (normalized !== text) counts.unicode++;
      node.nodeValue = normalized;
    }
    counts.total = counts.spaces + counts.ligatures + counts.quotes + counts.encoding + counts.punctuation + counts.unicode;
    return counts;
  }

  function collectSelectors(doc, selectors) {
    for (const node of Array.from(doc.getElementsByTagName('*'))) {
      selectors.elements.add(localName(node));
      for (const name of (node.getAttribute('class') || '').split(/\s+/).filter(Boolean)) selectors.classes.add(name);
      if (node.id) selectors.ids.add(node.id);
    }
  }

  function selectorMayMatch(selector, selectors) {
    if (!selector || selector === '*' || selector === 'html' || selector === 'body') return true;
    if (/::?|\[/.test(selector)) return true;
    const classes = Array.from(selector.matchAll(/\.([a-zA-Z_][\w-]*)/g), match => match[1]);
    if (classes.some(name => selectors.classes.has(name))) return true;
    const ids = Array.from(selector.matchAll(/#([a-zA-Z_][\w-]*)/g), match => match[1]);
    if (ids.some(name => selectors.ids.has(name))) return true;
    const elements = Array.from(selector.matchAll(/(?:^|[\s>+~])([a-zA-Z][\w-]*)/g), match => match[1].toLowerCase());
    if (elements.some(name => selectors.elements.has(name))) return true;
    return !classes.length && !ids.length && !elements.length;
  }

  function cleanUnusedCss(css, selectors) {
    let removed = 0;
    const result = String(css || '').replace(/(^|})(\s*)([^@{}][^{}]*)\{([^{}]*)\}/g, (full, prefix, spacing, selectorText, declarations) => {
      const keep = selectorText.split(',').some(selector => selectorMayMatch(selector.trim(), selectors));
      if (keep) return full;
      removed++; return prefix;
    });
    return {css: result, removed};
  }

  function removeFontFaces(css) {
    let removed = 0;
    const output = String(css || '').replace(/@font-face\s*\{[^{}]*\}/gi, () => { removed++; return ''; });
    return {css: output, removed};
  }

  function cleanStoreMetadata(opfDoc) {
    let removed = 0;
    for (const meta of elementsByLocalName(opfDoc, 'meta')) {
      const name = meta.getAttribute('name') || '';
      const property = meta.getAttribute('property') || '';
      if (STORE_META_NAMES.has(name) || STORE_META_PREFIXES.some(prefix => name.startsWith(prefix) || property.startsWith(prefix))) {
        meta.remove(); removed++;
      }
    }
    return removed;
  }

  function actualDrm(encryptionText) {
    if (!encryptionText || !/EncryptedData|xmlenc/i.test(encryptionText)) return false;
    const references = Array.from(encryptionText.matchAll(/CipherReference[^>]+URI=["']([^"']+)["']/gi), match => match[1]);
    if (!references.length) return !/idpf\.org\/2008\/embedding|ns\.adobe\.com\/pdf\/enc/i.test(encryptionText);
    return references.some(reference => !FONT_EXTENSIONS.has(extension(reference)));
  }

  function isArtifact(path) {
    const parts = normalizeZipPath(path).split('/');
    return parts.some(part => part === '__MACOSX' || part === '.git' || part === '.svn') || OS_ARTIFACTS.has(parts[parts.length - 1]);
  }

  async function readEffectiveText(zip, updates, path) {
    const updated = updates.get(path);
    if (!updated) return zip.readText(path);
    const value = updated.value;
    if (typeof value === 'string') return value;
    if (ArrayBuffer.isView(value)) return decoder.decode(new Uint8Array(value.buffer, value.byteOffset, value.byteLength));
    return decoder.decode(new Uint8Array(await value.arrayBuffer()));
  }

  async function ensureToc(zip, opfDoc, opfPath, updates, removed, xhtmlTexts) {
    const manifest = firstByLocalName(opfDoc, 'manifest');
    const spine = firstByLocalName(opfDoc, 'spine');
    if (!manifest || !spine) return 'No manifest or spine found';
    const items = manifestInfo(opfDoc, opfPath);
    const byId = new Map(items.map(item => [item.id, item]));
    const spineItems = Array.from(spine.children).filter(item => localName(item) === 'itemref').map(item => byId.get(item.getAttribute('idref') || '')).filter(Boolean);
    if (!spineItems.length) return 'Empty spine';
    let ncx = items.find(item => item.mediaType === 'application/x-dtbncx+xml');
    if (ncx && (zip.has(ncx.path) || updates.has(ncx.path)) && !removed.has(ncx.path)) {
      try {
        const doc = parseXml(await readEffectiveText(zip, updates, ncx.path), 'NCX');
        const points = elementsByLocalName(doc, 'navpoint');
        const valid = points.length && points.every(point => {
          const content = firstByLocalName(point, 'content');
          if (!content) return false;
          try {
            const target = resolveZipPath(ncx.path, content.getAttribute('src') || '');
            return zip.has(target) || updates.has(target);
          } catch (_) { return false; }
        });
        if (valid) return 'TOC is valid';
      } catch (_) { /* regenerate below */ }
    }
    const chapters = [];
    for (let index = 0; index < spineItems.length; index++) {
      const item = spineItems[index];
      let title = `Chapter ${index + 1}`;
      try {
        const text = xhtmlTexts.get(item.path) || await readEffectiveText(zip, updates, item.path);
        const parsed = parseRecoverableHtml(text).doc;
        const titleNode = firstByLocalName(parsed, 'title') || firstByLocalName(parsed, 'h1') || firstByLocalName(parsed, 'h2') || firstByLocalName(parsed, 'h3');
        if (titleNode && (titleNode.textContent || '').trim()) title = titleNode.textContent.trim();
      } catch (_) { /* default title */ }
      chapters.push({title, href: item.href});
    }
    let ncxPath;
    if (ncx) ncxPath = ncx.path;
    else {
      ncxPath = normalizeZipPath(`${dirname(opfPath) ? dirname(opfPath) + '/' : ''}toc.ncx`);
      const item = appendOpfElement(opfDoc, manifest, 'item');
      item.setAttribute('id', 'ncx'); item.setAttribute('href', relativeZipPath(opfPath, ncxPath)); item.setAttribute('media-type', 'application/x-dtbncx+xml');
      spine.setAttribute('toc', 'ncx');
    }
    const escapeXml = value => String(value || '').replaceAll('&', '&amp;').replaceAll('<', '&lt;').replaceAll('>', '&gt;').replaceAll('"', '&quot;');
    const points = chapters.map((chapter, index) => `<navPoint id="navPoint-${index + 1}" playOrder="${index + 1}"><navLabel><text>${escapeXml(chapter.title)}</text></navLabel><content src="${escapeXml(chapter.href)}"/></navPoint>`).join('');
    const title = chapters.length ? chapters[0].title : 'Unknown';
    const content = `<?xml version="1.0" encoding="utf-8"?><ncx xmlns="http://www.daisy.org/z3986/2005/ncx/" version="2005-1"><head><meta name="dtb:depth" content="1"/></head><docTitle><text>${escapeXml(title)}</text></docTitle><navMap>${points}</navMap></ncx>`;
    updates.set(ncxPath, {value: content});
    return `Generated TOC with ${chapters.length} entries`;
  }

  async function packageEpub(zip, updates, removed, report, progress) {
    const builder = new EpubZipBuilder();
    const written = new Set();
    await builder.addData('mimetype', encoder.encode('application/epub+zip'), false);
    written.add('mimetype');

    const addPath = async path => {
      path = normalizeZipPath(path);
      if (!path || written.has(path) || removed.has(path) || isArtifact(path)) return;
      const update = updates.get(path);
      if (update) {
        await builder.addData(path, update.value, true);
        written.add(path);
        return;
      }
      const entry = zip.get(path);
      if (!entry || entry.dir) return;
      if ((entry.flags & 1) !== 0) throw new Error(tr('epubkit.encrypted', 'The EPUB contains encrypted ZIP entries.'));
      if (entry.method !== 0 && entry.method !== 8) {
        await builder.addData(path, await zip.readBytes(entry), true);
      } else {
        await builder.addCopied(entry, await zip.compressedBlob(entry));
      }
      written.add(path);
    };

    if (zip.has('META-INF/container.xml') || updates.has('META-INF/container.xml')) await addPath('META-INF/container.xml');
    let done = 0;
    const candidates = zip.entries.filter(entry => !entry.dir && entry.name !== 'mimetype' && entry.name !== 'META-INF/container.xml');
    for (const entry of candidates) {
      if (state.cancelled) throw new Error(tr('epubkit.cancelled', 'Cancelled.'));
      await addPath(entry.name);
      done++;
      if (done % 25 === 0) {
        progress(95 + Math.min(3, done / Math.max(1, candidates.length) * 3), tr('epubkit.stage_package_files', 'Repackaging EPUB…'), `${done}/${candidates.length}`);
        await new Promise(resolve => setTimeout(resolve, 0));
      }
    }
    for (const path of Array.from(updates.keys()).sort()) await addPath(path);
    report.osArtifactsRemoved = zip.entries.filter(entry => isArtifact(entry.name)).length;
    return builder.finish();
  }

  async function processBook(book, options, progress) {
    const started = performance.now();
    const report = {
      success: false, originalSize: book.file.size, optimizedSize: 0, imagesTotal: 0,
      imagesConverted: 0, fontsRemoved: 0, cssRulesRemoved: 0, svgCoversFixed: 0,
      metadataItemsStripped: 0, whitespaceCleaned: 0, attrsStripped: 0,
      textFixesTotal: 0, osArtifactsRemoved: 0, coverGenerated: false, tocStatus: '',
      outputName: '', elapsed: 0, log: []
    };
    const updates = new Map();
    const removed = new Set();
    const renames = new Map();
    const occupied = new Set();
    let zip;
    let opfPath;
    let opfDoc;

    const stage = (number, key, fallback, detail) => {
      const percent = Math.min(100, number * 5);
      const message = tr(key, fallback);
      report.log.push(`${number}/20 ${message}${detail ? ` — ${detail}` : ''}`);
      progress(percent, message, detail || '');
    };

    stage(1, 'epubkit.stage_drm', 'Checking for DRM…');
    zip = await LazyEpubZip.open(book.file);
    zip.entries.forEach(entry => occupied.add(entry.name));
    if (zip.has('META-INF/encryption.xml') && actualDrm(await zip.readText('META-INF/encryption.xml'))) {
      throw new Error(tr('epubkit.drm_error', 'This EPUB is DRM-protected and cannot be processed.'));
    }
    if (state.cancelled) throw new Error(tr('epubkit.cancelled', 'Cancelled.'));

    stage(2, 'epubkit.stage_extract', 'Reading EPUB archive…');
    const container = zip.has('META-INF/container.xml') ? await zip.readText('META-INF/container.xml') : '';
    stage(3, 'epubkit.stage_structure', 'Parsing book structure…');
    opfPath = findOpfPath(zip, container);
    opfDoc = parseXml(await zip.readText(opfPath), 'OPF');

    stage(4, 'epubkit.stage_metadata', 'Reading metadata…');
    let metadata = metadataFromOpf(opfDoc);
    stage(5, 'epubkit.stage_metadata_edit', 'Applying metadata edits…');
    setDcMetadata(opfDoc, 'title', options.title || metadata.title);
    setDcMetadata(opfDoc, 'creator', options.author || metadata.author);
    metadata = metadataFromOpf(opfDoc);

    stage(6, 'epubkit.stage_catalog', 'Cataloguing EPUB content…');
    let items = manifestInfo(opfDoc, opfPath);
    const xhtmlItems = items.filter(item => item.mediaType === 'application/xhtml+xml' || item.mediaType === 'text/html');
    const itemById = new Map(items.map(item => [item.id, item]));
    const spine = firstByLocalName(opfDoc, 'spine');
    const svgCoverPaths = new Set(Array.from(spine?.children || [])
      .filter(node => localName(node) === 'itemref')
      .slice(0, 3)
      .map(node => itemById.get(node.getAttribute('idref') || '')?.path)
      .filter(Boolean));
    const cssItems = items.filter(item => item.mediaType === 'text/css');
    const imageItems = items.filter(item => item.mediaType.startsWith('image/') && IMAGE_EXTENSIONS.has(extension(item.path)) && zip.has(item.path));
    const fontItems = items.filter(item => FONT_EXTENSIONS.has(extension(item.path)) || /font|opentype/i.test(item.mediaType));
    report.imagesTotal = imageItems.length;

    stage(7, 'epubkit.stage_images', 'Processing images…', `0/${imageItems.length}`);
    for (let index = 0; index < imageItems.length; index++) {
      if (state.cancelled) throw new Error(tr('epubkit.cancelled', 'Cancelled.'));
      const item = imageItems[index];
      progress(15 + ((index + 1) / Math.max(1, imageItems.length)) * 45, tr('epubkit.stage_images', 'Processing images…'), `${index + 1}/${imageItems.length} · ${basename(item.path)}`);
      try {
        const blob = await zip.readBlob(item.path, item.mediaType);
        const outputs = await processImage(blob, item.path, options);
        const targetPaths = [];
        for (let partIndex = 0; partIndex < outputs.length; partIndex++) {
          const output = outputs[partIndex];
          const path = makeUniquePath(output.path, occupied, partIndex === 0 ? item.path : '');
          updates.set(path, {value: output.blob});
          targetPaths.push(path);
        }
        renames.set(item.path, targetPaths);
        if (!targetPaths.includes(item.path)) removed.add(item.path);
        item.element.setAttribute('href', relativeZipPath(opfPath, targetPaths[0]));
        item.element.setAttribute('media-type', 'image/jpeg');
        if (targetPaths.length > 1) {
          const manifest = item.element.parentNode;
          for (let partIndex = 1; partIndex < targetPaths.length; partIndex++) {
            const extra = appendOpfElement(opfDoc, manifest, 'item');
            extra.setAttribute('id', `${item.id || 'image'}-part${partIndex + 1}`);
            extra.setAttribute('href', relativeZipPath(opfPath, targetPaths[partIndex]));
            extra.setAttribute('media-type', 'image/jpeg');
          }
        }
        report.imagesConverted += outputs.length;
      } catch (error) {
        report.log.push(`Image skipped: ${item.path} — ${error.message}`);
      }
      await new Promise(resolve => setTimeout(resolve, 0));
    }

    stage(8, 'epubkit.stage_svg', 'Fixing SVG covers…');
    stage(9, 'epubkit.stage_cover', 'Checking cover…');
    let coverItem = findCoverItem(opfDoc);
    if (!coverItem && options.generateCover) {
      const opfFolder = dirname(opfPath);
      const coverPath = makeUniquePath(`${opfFolder ? opfFolder + '/' : ''}images/cover_generated.jpg`, occupied, '');
      updates.set(coverPath, {value: await generateCover(options.title || metadata.title, options.author || metadata.author, options)});
      const manifest = firstByLocalName(opfDoc, 'manifest');
      if (manifest) {
        coverItem = appendOpfElement(opfDoc, manifest, 'item');
        coverItem.setAttribute('id', 'cover-image-generated'); coverItem.setAttribute('href', relativeZipPath(opfPath, coverPath));
        coverItem.setAttribute('media-type', 'image/jpeg'); coverItem.setAttribute('properties', 'cover-image');
        const metadataNode = firstByLocalName(opfDoc, 'metadata');
        if (metadataNode) {
          const coverMeta = appendOpfElement(opfDoc, metadataNode, 'meta');
          coverMeta.setAttribute('name', 'cover'); coverMeta.setAttribute('content', 'cover-image-generated');
        }
        report.coverGenerated = true;
      }
    }

    stage(10, 'epubkit.stage_references', 'Updating image references…');
    stage(11, 'epubkit.stage_html', 'Repairing XHTML…');
    const selectors = {classes: new Set(), ids: new Set(), elements: new Set()};
    const xhtmlTexts = new Map();
    for (let index = 0; index < xhtmlItems.length; index++) {
      const item = xhtmlItems[index];
      if (!item.path || !zip.has(item.path)) continue;
      if (state.cancelled) throw new Error(tr('epubkit.cancelled', 'Cancelled.'));
      try {
        const source = await readEffectiveText(zip, updates, item.path);
        const parsed = parseRecoverableHtml(source);
        if (parsed.recovered) report.log.push(`Recovered malformed XHTML: ${item.path}`);
        if (svgCoverPaths.has(item.path)) report.svgCoversFixed += fixSvgCovers(parsed.doc);
        updateImageReferences(parsed.doc, item.path, renames);
        report.attrsStripped += stripAttributes(parsed.doc);
        report.whitespaceCleaned += normalizeEmptyBlocks(parsed.doc);
        ensureChapterBreakStyle(parsed.doc);
        if (options.textCleanup) report.textFixesTotal += cleanTextNodes(parsed.doc).total;
        collectSelectors(parsed.doc, selectors);
        const output = serializeDocument(parsed.doc);
        xhtmlTexts.set(item.path, output);
        updates.set(item.path, {value: output});
      } catch (error) {
        report.log.push(`XHTML left unchanged: ${item.path} — ${error.message}`);
      }
      progress(70 + ((index + 1) / Math.max(1, xhtmlItems.length)) * 4, tr('epubkit.stage_html', 'Repairing XHTML…'), `${index + 1}/${xhtmlItems.length}`);
      await new Promise(resolve => setTimeout(resolve, 0));
    }

    stage(12, 'epubkit.stage_css', 'Cleaning CSS…');
    for (const item of cssItems) {
      if (!item.path || !zip.has(item.path)) continue;
      try {
        const original = await readEffectiveText(zip, updates, item.path);
        let css = rewriteCssUrls(original, item.path, renames);
        if (options.removeCss) {
          const cleaned = cleanUnusedCss(css, selectors); css = cleaned.css; report.cssRulesRemoved += cleaned.removed;
        }
        if (options.removeFonts) {
          const fonts = removeFontFaces(css); css = fonts.css; report.fontsRemoved += fonts.removed;
        }
        if (css !== original) updates.set(item.path, {value: css});
      } catch (error) {
        report.log.push(`CSS left unchanged: ${item.path} — ${error.message}`);
      }
    }

    stage(13, 'epubkit.stage_fonts', 'Removing embedded fonts…');
    if (options.removeFonts) {
      for (const item of fontItems) {
        if (item.path) removed.add(item.path);
        if (item.element.parentNode) item.element.remove();
        report.fontsRemoved++;
      }
    }

    stage(14, 'epubkit.stage_normalize', 'Normalizing content…');
    stage(15, 'epubkit.stage_text', 'Cleaning text content…');
    stage(16, 'epubkit.stage_clean_metadata', 'Cleaning metadata…');
    if (options.cleanMetadata) report.metadataItemsStripped = cleanStoreMetadata(opfDoc);

    stage(17, 'epubkit.stage_toc', 'Checking table of contents…');
    report.tocStatus = await ensureToc(zip, opfDoc, opfPath, updates, removed, xhtmlTexts);
    updates.set(opfPath, {value: serializeDocument(opfDoc)});

    stage(18, 'epubkit.stage_cleanup', 'Removing OS artifacts…');
    for (const entry of zip.entries) if (isArtifact(entry.name)) removed.add(entry.name);
    stage(19, 'epubkit.stage_package', 'Repackaging EPUB…');
    const blob = await packageEpub(zip, updates, removed, report, progress);

    metadata = metadataFromOpf(opfDoc);
    report.outputName = safeFilename(options.title || metadata.title, options.author || metadata.author, book.file.name);
    report.optimizedSize = blob.size;
    report.elapsed = (performance.now() - started) / 1000;
    report.success = true;
    stage(20, 'epubkit.stage_complete', 'Complete');
    return {blob, report};
  }

  function normalizeRemotePath(value) {
    const parts = String(value || '/Books').replace(/\\/g, '/').split('/').filter(Boolean);
    if (parts.some(part => part === '.' || part === '..' || part.startsWith('.'))) throw new Error(tr('epubkit.bad_path', 'Invalid destination folder.'));
    return '/' + parts.join('/');
  }

  async function ensureRemoteFolder(path) {
    const segments = normalizeRemotePath(path).split('/').filter(Boolean);
    let parent = '/';
    for (const segment of segments) {
      const form = new FormData(); form.append('name', segment); form.append('path', parent);
      try { await fetch('/mkdir', {method: 'POST', body: form}); } catch (_) { /* upload reports the real failure */ }
      parent = parent === '/' ? `/${segment}` : `${parent}/${segment}`;
    }
  }

  function uploadViaWebSocket(file, targetPath, onProgress) {
    return new Promise((resolve, reject) => {
      const socket = new WebSocket(`ws://${window.location.hostname}:81/`);
      state.activeSocket = socket;
      socket.binaryType = 'arraybuffer';
      let complete = false;
      socket.onopen = () => socket.send(`START:${file.name}:${file.size}:${targetPath}`);
      socket.onmessage = async event => {
        const message = String(event.data || '');
        if (message === 'READY') {
          try {
            const chunkSize = 32768;
            for (let offset = 0; offset < file.size; offset += chunkSize) {
              if (state.cancelled) throw new Error(tr('epubkit.cancelled', 'Cancelled.'));
              while (socket.bufferedAmount > chunkSize * 2 && socket.readyState === WebSocket.OPEN) await new Promise(done => setTimeout(done, 5));
              if (socket.readyState !== WebSocket.OPEN) throw new Error('WebSocket closed during upload');
              const end = Math.min(file.size, offset + chunkSize);
              socket.send(await file.slice(offset, end).arrayBuffer());
              onProgress(end, file.size);
            }
          } catch (error) { socket.close(); reject(error); }
        } else if (message === 'DONE') {
          complete = true; state.activeSocket = null; socket.close(); onProgress(file.size, file.size); resolve();
        } else if (message.startsWith('ERROR:')) {
          state.activeSocket = null; socket.close(); reject(new Error(message.slice(6)));
        }
      };
      socket.onerror = () => { state.activeSocket = null; reject(new Error('WebSocket connection failed')); };
      socket.onclose = () => { state.activeSocket = null; if (!complete && !state.cancelled) reject(new Error('WebSocket closed during upload')); };
    });
  }

  function uploadViaHttp(file, targetPath, onProgress) {
    return new Promise((resolve, reject) => {
      const xhr = new XMLHttpRequest();
      const form = new FormData(); form.append('file', file);
      xhr.open('POST', `/upload?path=${encodeURIComponent(targetPath)}`, true);
      xhr.upload.onprogress = event => { if (event.lengthComputable) onProgress(event.loaded, event.total); };
      xhr.onload = () => xhr.status === 200 ? resolve() : reject(new Error(xhr.responseText || `HTTP ${xhr.status}`));
      xhr.onerror = () => reject(new Error('Network error'));
      xhr.send(form);
    });
  }

  async function uploadResult(blob, name, targetPath, onProgress) {
    const file = new File([blob], name, {type: 'application/epub+zip'});
    await ensureRemoteFolder(targetPath);
    try { await uploadViaWebSocket(file, targetPath, onProgress); }
    catch (error) {
      if (state.cancelled) throw error;
      await uploadViaHttp(file, targetPath, onProgress);
    }
  }

  function createProgressRows(books) {
    ui.progressItems.innerHTML = books.map(book => `<article class="epubkit-progress-item" id="progress-${book.id}">
      <div class="epubkit-progress-head"><span>${escapeHtml(book.file.name)}</span><span data-percent>0%</span></div>
      <div class="epubkit-progress-track"><div class="epubkit-progress-bar"></div></div>
      <div class="epubkit-progress-message">${escapeHtml(tr('epubkit.waiting', 'Waiting…'))}</div>
      <div class="epubkit-progress-log"></div>
    </article>`).join('');
  }

  function updateProgress(book, percent, message, detail) {
    const row = el(`progress-${book.id}`);
    if (!row) return;
    const value = Math.max(0, Math.min(100, percent));
    row.querySelector('.epubkit-progress-bar').style.width = `${value}%`;
    row.querySelector('[data-percent]').textContent = `${Math.round(value)}%`;
    row.querySelector('.epubkit-progress-message').textContent = detail ? `${message} — ${detail}` : message;
    if (message) {
      const log = row.querySelector('.epubkit-progress-log');
      const line = detail ? `${message} — ${detail}` : message;
      if (!log.textContent.endsWith(`${line}\n`)) {
        log.textContent += `${line}\n`;
        log.scrollTop = log.scrollHeight;
      }
    }
  }

  function reportSummary(report) {
    const parts = [];
    if (report.imagesConverted) parts.push(tr('epubkit.summary_images', '{count} images converted', {count: report.imagesConverted}));
    if (report.fontsRemoved) parts.push(tr('epubkit.summary_fonts', '{count} fonts/rules removed', {count: report.fontsRemoved}));
    if (report.cssRulesRemoved) parts.push(tr('epubkit.summary_css', '{count} CSS rules removed', {count: report.cssRulesRemoved}));
    if (report.textFixesTotal) parts.push(tr('epubkit.summary_text', '{count} text fixes', {count: report.textFixesTotal}));
    if (report.coverGenerated) parts.push(tr('epubkit.summary_cover', 'cover generated'));
    if (report.tocStatus) parts.push(report.tocStatus);
    return parts.join(' · ') || tr('epubkit.summary_no_changes', 'No structural changes were needed.');
  }

  function renderResults() {
    for (const result of state.results) {
      if (result.downloadUrl) URL.revokeObjectURL(result.downloadUrl);
      result.downloadUrl = result.blob ? URL.createObjectURL(result.blob) : '';
    }
    ui.resultItems.innerHTML = state.results.map((result, index) => {
      if (!result.success) {
        return `<article class="epubkit-result-item error"><div class="epubkit-result-title"><span>✕ ${escapeHtml(result.book.file.name)}</span></div><div class="epubkit-result-summary">${escapeHtml(result.error)}</div></article>`;
      }
      const report = result.report;
      const reduction = report.originalSize ? (1 - report.optimizedSize / report.originalSize) * 100 : 0;
      const action = result.uploaded
        ? `<button class="uploaded" type="button" disabled>✓ ${escapeHtml(tr('epubkit.uploaded', 'Uploaded to reader'))}</button>`
        : `${result.downloadUrl ? `<a href="${result.downloadUrl}" download="${escapeHtml(report.outputName)}">↓ ${escapeHtml(tr('epubkit.download', 'Download EPUB'))}</a>` : ''}
           ${result.uploadError && result.blob ? `<button type="button" data-retry-upload="${index}">↻ ${escapeHtml(tr('epubkit.retry_upload', 'Retry upload'))}</button>` : ''}`;
      return `<article class="epubkit-result-item ${result.uploadError ? 'error' : 'success'}">
        <div class="epubkit-result-title"><span>✓ ${escapeHtml(report.outputName)}</span><span>${report.elapsed.toFixed(1)} s</span></div>
        <div class="epubkit-result-summary">${escapeHtml(reportSummary(report))}${result.uploadError ? `<br>${escapeHtml(tr('epubkit.upload_failed', 'Upload failed: {msg}', {msg: result.uploadError}))}` : ''}</div>
        <div class="epubkit-result-stats">
          <span>${escapeHtml(formatBytes(report.originalSize))} → ${escapeHtml(formatBytes(report.optimizedSize))}</span>
          <span>${reduction >= 0 ? '−' : '+'}${Math.abs(reduction).toFixed(1)}%</span>
          <span>${report.imagesConverted}/${report.imagesTotal} ${escapeHtml(tr('epubkit.images_short', 'images'))}</span>
        </div>
        <div class="epubkit-result-actions">${action}</div>
        <details><summary>${escapeHtml(tr('epubkit.report_log', 'Processing report'))}</summary><pre class="epubkit-progress-log">${escapeHtml(report.log.join('\n'))}</pre></details>
      </article>`;
    }).join('');
    ui.results.hidden = state.results.length === 0;
  }

  async function retryUpload(index) {
    const result = state.results[index];
    if (!result || !result.blob || state.processing) return;
    state.processing = true;
    try {
      const target = normalizeRemotePath(ui.path.value);
      await uploadResult(result.blob, result.report.outputName, target, () => {});
      result.uploaded = true; result.uploadError = ''; result.blob = null;
    } catch (error) {
      result.uploadError = error.message || String(error);
    } finally {
      state.processing = false; renderResults();
    }
  }

  async function startProcessing() {
    const books = state.books.filter(book => !book.error);
    if (!books.length || state.processing) return;
    for (const result of state.results) if (result.downloadUrl) URL.revokeObjectURL(result.downloadUrl);
    state.results = [];
    state.cancelled = false;
    state.processing = true;
    ui.process.disabled = true; ui.cancel.hidden = false; ui.progress.hidden = false; ui.results.hidden = true;
    createProgressRows(books);
    const directUpload = ui.upload.checked;
    let targetPath = '/Books';
    try { if (directUpload) targetPath = normalizeRemotePath(ui.path.value); }
    catch (error) { alert(error.message); state.processing = false; ui.process.disabled = false; ui.cancel.hidden = true; return; }

    for (let index = 0; index < books.length; index++) {
      const book = books[index];
      ui.stageCounter.textContent = `${index + 1}/${books.length}`;
      try {
        const options = optionsFromUi(book);
        const converted = await processBook(book, options, (percent, message, detail) => {
          updateProgress(book, directUpload ? percent * .85 : percent, message, detail);
        });
        const result = {book, success: true, blob: converted.blob, report: converted.report, uploaded: false, uploadError: '', downloadUrl: ''};
        if (directUpload && !state.cancelled) {
          updateProgress(book, 85, tr('epubkit.uploading', 'Uploading to reader…'), converted.report.outputName);
          try {
            await uploadResult(converted.blob, converted.report.outputName, targetPath, (sent, total) => {
              updateProgress(book, 85 + (sent / Math.max(1, total)) * 15, tr('epubkit.uploading', 'Uploading to reader…'), `${Math.round(sent / Math.max(1, total) * 100)}%`);
            });
            result.uploaded = true;
            result.blob = null;
            updateProgress(book, 100, tr('epubkit.upload_complete', 'Optimized and uploaded.'), targetPath);
          } catch (error) {
            result.uploadError = error.message || String(error);
            updateProgress(book, 100, tr('epubkit.upload_failed_short', 'Optimized, but upload failed.'), result.uploadError);
          }
        }
        state.results.push(result);
      } catch (error) {
        const message = error.message || String(error);
        state.results.push({book, success: false, error: message});
        updateProgress(book, 100, state.cancelled ? tr('epubkit.cancelled', 'Cancelled.') : tr('epubkit.failed', 'Processing failed.'), message);
      }
      renderResults();
      if (state.cancelled) break;
      await new Promise(resolve => setTimeout(resolve, 0));
    }
    state.processing = false;
    state.activeSocket = null;
    ui.process.disabled = false; ui.cancel.hidden = true;
  }

  ui.process.addEventListener('click', startProcessing);
  ui.resultItems.addEventListener('click', event => {
    const button = event.target.closest('[data-retry-upload]');
    if (button) retryUpload(Number(button.dataset.retryUpload));
  });

  try {
    new DecompressionStream('deflate-raw');
    new CompressionStream('deflate-raw');
  } catch (_) {
    el('epubkitMemoryMode').textContent = tr('epubkit.compatibility_mode', 'Compatibility ZIP mode');
  }
  setPreset('full');
  setQuality(70, false);
})();
