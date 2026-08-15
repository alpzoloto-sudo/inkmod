/* Browser-side EPUB preparation worker. Heavy ZIP directory parsing and
 * decompression happen off the UI thread. The File object is structured-
 * cloned by reference by modern browsers, so posting it does not duplicate
 * the complete book in the page heap. */
importScripts('/js/jszip.min.js');

const normalise = path => {
  const out = [];
  String(path || '').replace(/\\/g, '/').split('/').forEach(part => {
    if (!part || part === '.') return;
    if (part === '..') out.pop(); else out.push(part);
  });
  return out.join('/');
};

const attr = (tag, name) => {
  const match = tag.match(new RegExp('(?:^|\\s)' + name.replace(':', '\\:') + '\\s*=\\s*["\\\']([^"\\\']*)["\\\']', 'i'));
  return match ? match[1] : '';
};

self.onmessage = async event => {
  const { id, file } = event.data || {};
  try {
    self.postMessage({ id, progress: 5 });
    const zip = await JSZip.loadAsync(file);
    const containerEntry = zip.file('META-INF/container.xml');
    if (!containerEntry) throw new Error('EPUB container.xml is missing');
    const containerXml = await containerEntry.async('string');
    const rootTag = containerXml.match(/<(?:(?:\w+):)?rootfile\b[^>]*>/i);
    const opfPath = normalise(rootTag ? attr(rootTag[0], 'full-path') : '');
    if (!opfPath || !zip.file(opfPath)) throw new Error('EPUB OPF is missing');

    self.postMessage({ id, progress: 25 });
    const opfXml = await zip.file(opfPath).async('string');
    const opfBase = opfPath.includes('/') ? opfPath.slice(0, opfPath.lastIndexOf('/') + 1) : '';
    const itemTags = opfXml.match(/<(?:(?:\w+):)?item\b[^>]*>/gi) || [];
    let navPath = '';
    let ncxPath = '';
    for (const tag of itemTags) {
      const path = normalise(opfBase + attr(tag, 'href'));
      const properties = attr(tag, 'properties').split(/\s+/);
      const mediaType = attr(tag, 'media-type').toLowerCase();
      if (!navPath && properties.includes('nav')) navPath = path;
      if (!ncxPath && mediaType === 'application/x-dtbncx+xml') ncxPath = path;
    }
    const navXml = navPath && zip.file(navPath) ? await zip.file(navPath).async('string') : '';
    const ncxXml = ncxPath && zip.file(ncxPath) ? await zip.file(ncxPath).async('string') : '';

    const entrySizes = Object.create(null);
    Object.keys(zip.files).forEach(path => {
      const entry = zip.files[path];
      if (!entry.dir) entrySizes[normalise(path)] = entry._data && Number(entry._data.uncompressedSize) || 0;
    });
    self.postMessage({ id, progress: 90 });
    self.postMessage({ id, result: { containerXml, opfPath, opfXml, navPath, navXml, ncxPath, ncxXml, entrySizes } });
  } catch (error) {
    self.postMessage({ id, error: error && error.message ? error.message : String(error) });
  }
};
