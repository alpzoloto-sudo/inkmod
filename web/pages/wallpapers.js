
(function () {
  'use strict';

  var state = {
    image: null,
    sourceUrl: null,
    sourceName: '',
    device: 'x4',
    orientation: 'portrait',
    fit: 'contain',
    alignX: 0.5,
    alignY: 0.5,
    pickingBackground: false,
    renderTicket: 0
  };

  var DEVICE = {
    x4: { w: 480, h: 800 },
    x3: { w: 528, h: 792 }
  };

  var $ = function (id) { return document.getElementById(id); };
  var drop = $('wallpaperDropZone');
  var fileInput = $('wallpaperFile');
  var editor = $('wallpaperEditor');
  var canvas = $('wallpaperCanvas');
  var ctx = canvas.getContext('2d', { willReadFrequently: true });
  var downloadBtn = $('downloadWallpaper');
  var applyBtn = $('applyWallpaper');
  var statusBox = $('wallpaperStatus');

  var staging = document.createElement('canvas');
  var stagingCtx = staging.getContext('2d', { willReadFrequently: true });

  function localized(key, fallback) {
    try { return typeof t === 'function' ? t(key) : fallback; } catch (_) { return fallback; }
  }

  function clamp(n, min, max) { return Math.max(min, Math.min(max, n)); }

  function parseHexColor(value) {
    var hex = String(value || '#ffffff').replace('#', '');
    if (hex.length === 3) hex = hex.split('').map(function (c) { return c + c; }).join('');
    var n = parseInt(hex, 16);
    return [(n >> 16) & 255, (n >> 8) & 255, n & 255];
  }

  function rgbHex(r, g, b) {
    return '#' + [r, g, b].map(function (n) {
      return clamp(Math.round(n), 0, 255).toString(16).padStart(2, '0');
    }).join('');
  }

  function setSegmentActive(containerId, dataName, value) {
    var buttons = $(containerId).querySelectorAll('button');
    buttons.forEach(function (btn) { btn.classList.toggle('active', btn.dataset[dataName] === value); });
  }

  function deviceSize() {
    var base = DEVICE[state.device];
    return state.orientation === 'landscape' ? { w: base.h, h: base.w } : { w: base.w, h: base.h };
  }

  function applyDeviceSize() {
    var s = deviceSize();
    $('wallpaperWidth').value = s.w;
    $('wallpaperHeight').value = s.h;
    scheduleRender();
  }

  function outputSize() {
    return {
      w: clamp(parseInt($('wallpaperWidth').value, 10) || 480, 64, 1600),
      h: clamp(parseInt($('wallpaperHeight').value, 10) || 800, 64, 1600)
    };
  }

  function sourceRectFor(img, outW, outH) {
    var iw = img.naturalWidth || img.width;
    var ih = img.naturalHeight || img.height;
    var zoom = (parseInt($('wallpaperZoom').value, 10) || 100) / 100;

    if (state.fit === 'stretch') {
      return { x: 0, y: 0, w: outW, h: outH };
    }

    var baseScale = state.fit === 'cover' ? Math.max(outW / iw, outH / ih) : Math.min(outW / iw, outH / ih);
    var scale = baseScale * zoom;
    var dw = iw * scale;
    var dh = ih * scale;
    return {
      x: (outW - dw) * state.alignX,
      y: (outH - dh) * state.alignY,
      w: dw,
      h: dh
    };
  }

  function colorDistance(r, g, b, bg) {
    var dr = r - bg[0], dg = g - bg[1], db = b - bg[2];
    return Math.sqrt(dr * dr + dg * dg + db * db);
  }

  // Removes only background reachable from the OUTER CANVAS EDGE. Enclosed
  // whites inside a subject are never visited, so socks/eyes/highlights remain.
  function removeEdgeConnectedBackground(imageData, width, height, bg, tolerance, softness) {
    var data = imageData.data;
    var limit = tolerance + softness;
    var count = width * height;
    var visited = new Uint8Array(count);
    var queue = new Int32Array(count);
    var head = 0, tail = 0;

    function eligible(idx) {
      var off = idx * 4;
      if (data[off + 3] === 0) return true;
      return colorDistance(data[off], data[off + 1], data[off + 2], bg) <= limit;
    }

    function push(idx) {
      if (idx < 0 || idx >= count || visited[idx]) return;
      if (!eligible(idx)) return;
      visited[idx] = 1;
      queue[tail++] = idx;
    }

    var x, y;
    for (x = 0; x < width; x++) {
      push(x);
      push((height - 1) * width + x);
    }
    for (y = 1; y < height - 1; y++) {
      push(y * width);
      push(y * width + width - 1);
    }

    while (head < tail) {
      var idx = queue[head++];
      var px = idx % width;
      var py = (idx / width) | 0;
      if (px > 0) push(idx - 1);
      if (px + 1 < width) push(idx + 1);
      if (py > 0) push(idx - width);
      if (py + 1 < height) push(idx + width);
    }

    for (var i = 0; i < count; i++) {
      if (!visited[i]) continue;
      var o = i * 4;
      if (data[o + 3] === 0) continue;
      var d = colorDistance(data[o], data[o + 1], data[o + 2], bg);
      if (d <= tolerance || softness <= 0) {
        data[o + 3] = 0;
      } else {
        var alpha = Math.round(255 * ((d - tolerance) / Math.max(1, softness)));
        data[o + 3] = Math.min(data[o + 3], clamp(alpha, 0, 255));
      }
    }
  }

  function applyEffects(imageData) {
    var data = imageData.data;
    var grayscale = $('wallpaperGrayscale').checked;
    var contrast = (parseInt($('wallpaperContrast').value, 10) || 100) / 100;
    var brightness = (parseInt($('wallpaperBrightness').value, 10) || 100) / 100;

    for (var i = 0; i < data.length; i += 4) {
      if (data[i + 3] === 0) continue;
      var r = data[i], g = data[i + 1], b = data[i + 2];

      if (grayscale) {
        var gray = 0.299 * r + 0.587 * g + 0.114 * b;
        r = g = b = gray;
      }

      r = ((r - 128) * contrast + 128) * brightness;
      g = ((g - 128) * contrast + 128) * brightness;
      b = ((b - 128) * contrast + 128) * brightness;

      data[i] = clamp(Math.round(r), 0, 255);
      data[i + 1] = clamp(Math.round(g), 0, 255);
      data[i + 2] = clamp(Math.round(b), 0, 255);
    }
  }

  function renderNow() {
    if (!state.image) return;

    var size = outputSize();
    canvas.width = size.w;
    canvas.height = size.h;
    staging.width = size.w;
    staging.height = size.h;

    stagingCtx.clearRect(0, 0, size.w, size.h);
    stagingCtx.imageSmoothingEnabled = true;
    stagingCtx.imageSmoothingQuality = 'high';

    var r = sourceRectFor(state.image, size.w, size.h);
    stagingCtx.drawImage(state.image, r.x, r.y, r.w, r.h);

    var imageData = stagingCtx.getImageData(0, 0, size.w, size.h);

    // Background removal must run BEFORE grayscale/contrast; otherwise the
    // chosen source background color can be distorted by effects.
    if ($('removeBackground').checked) {
      removeEdgeConnectedBackground(
        imageData, size.w, size.h,
        parseHexColor($('backgroundColor').value),
        parseInt($('backgroundTolerance').value, 10) || 0,
        parseInt($('backgroundSoftness').value, 10) || 0
      );
    }

    applyEffects(imageData);

    ctx.clearRect(0, 0, size.w, size.h);
    ctx.putImageData(imageData, 0, 0);
    downloadBtn.disabled = false;
    applyBtn.disabled = false;
  }

  function scheduleRender() {
    if (!state.image) return;
    var ticket = ++state.renderTicket;
    requestAnimationFrame(function () {
      if (ticket !== state.renderTicket) return;
      renderNow();
    });
  }

  function loadFile(file) {
    if (!file || !file.type || file.type.indexOf('image/') !== 0) {
      alert(localized('wallpaper.bad_file', 'Please choose an image file.'));
      return;
    }

    if (state.sourceUrl) URL.revokeObjectURL(state.sourceUrl);
    state.sourceUrl = URL.createObjectURL(file);
    state.sourceName = file.name || 'image';

    var img = new Image();
    img.onload = function () {
      state.image = img;
      $('wallpaperSourceInfo').textContent =
        state.sourceName + ' · ' + (img.naturalWidth || img.width) + ' × ' + (img.naturalHeight || img.height);
      var stem = state.sourceName.replace(/\.[^.]+$/, '').replace(/[^\w\-а-яА-ЯёЁіІїЇєЄ ]+/g, '-').trim();
      $('wallpaperFilename').value = (stem || 'sleep-overlay') + '.png';
      editor.classList.remove('hidden');
      scheduleRender();
    };
    img.onerror = function () {
      alert(localized('wallpaper.decode_error', 'The browser could not decode this image.'));
    };
    img.src = state.sourceUrl;
  }

  drop.addEventListener('click', function () { fileInput.click(); });
  drop.addEventListener('keydown', function (e) {
    if (e.key === 'Enter' || e.key === ' ') { e.preventDefault(); fileInput.click(); }
  });
  fileInput.addEventListener('change', function () { loadFile(fileInput.files && fileInput.files[0]); });

  ['dragenter', 'dragover'].forEach(function (name) {
    drop.addEventListener(name, function (e) { e.preventDefault(); drop.classList.add('dragover'); });
  });
  ['dragleave', 'drop'].forEach(function (name) {
    drop.addEventListener(name, function (e) { e.preventDefault(); drop.classList.remove('dragover'); });
  });
  drop.addEventListener('drop', function (e) {
    if (e.dataTransfer && e.dataTransfer.files && e.dataTransfer.files[0]) loadFile(e.dataTransfer.files[0]);
  });

  $('deviceButtons').addEventListener('click', function (e) {
    var btn = e.target.closest('button[data-device]');
    if (!btn) return;
    state.device = btn.dataset.device;
    setSegmentActive('deviceButtons', 'device', state.device);
    applyDeviceSize();
  });

  $('orientationButtons').addEventListener('click', function (e) {
    var btn = e.target.closest('button[data-orientation]');
    if (!btn) return;
    state.orientation = btn.dataset.orientation;
    setSegmentActive('orientationButtons', 'orientation', state.orientation);
    applyDeviceSize();
  });

  $('fitButtons').addEventListener('click', function (e) {
    var btn = e.target.closest('button[data-fit]');
    if (!btn) return;
    state.fit = btn.dataset.fit;
    setSegmentActive('fitButtons', 'fit', state.fit);
    scheduleRender();
  });

  $('alignGrid').addEventListener('click', function (e) {
    var btn = e.target.closest('button[data-align]');
    if (!btn) return;
    var p = btn.dataset.align.split(',').map(Number);
    state.alignX = p[0]; state.alignY = p[1];
    $('alignGrid').querySelectorAll('button').forEach(function (b) { b.classList.toggle('active', b === btn); });
    scheduleRender();
  });

  function bindRange(id, outputId, suffix) {
    $(id).addEventListener('input', function () {
      $(outputId).textContent = this.value + (suffix || '');
      scheduleRender();
    });
  }
  bindRange('wallpaperZoom', 'zoomValue', '%');
  bindRange('backgroundTolerance', 'toleranceValue', '');
  bindRange('backgroundSoftness', 'softnessValue', '');
  bindRange('wallpaperContrast', 'contrastValue', '%');
  bindRange('wallpaperBrightness', 'brightnessValue', '%');

  ['wallpaperWidth', 'wallpaperHeight', 'backgroundColor', 'removeBackground', 'wallpaperGrayscale'].forEach(function (id) {
    $(id).addEventListener('input', scheduleRender);
    $(id).addEventListener('change', scheduleRender);
  });

  $('wallpaperResetView').addEventListener('click', function () {
    state.fit = 'contain';
    state.alignX = 0.5; state.alignY = 0.5;
    $('wallpaperZoom').value = 100;
    $('zoomValue').textContent = '100%';
    setSegmentActive('fitButtons', 'fit', 'contain');
    $('alignGrid').querySelectorAll('button').forEach(function (b) {
      b.classList.toggle('active', b.dataset.align === '0.5,0.5');
    });
    scheduleRender();
  });

  $('pickBackground').addEventListener('click', function () {
    if (!state.image) return;
    state.pickingBackground = !state.pickingBackground;
    $('pickHint').classList.toggle('hidden', !state.pickingBackground);
    canvas.style.cursor = state.pickingBackground ? 'crosshair' : 'default';
  });

  canvas.addEventListener('click', function (e) {
    if (!state.pickingBackground) return;
    var rect = canvas.getBoundingClientRect();
    var x = clamp(Math.floor((e.clientX - rect.left) * canvas.width / rect.width), 0, canvas.width - 1);
    var y = clamp(Math.floor((e.clientY - rect.top) * canvas.height / rect.height), 0, canvas.height - 1);

    // Pick from the original staged image before effects/background removal.
    var px = stagingCtx.getImageData(x, y, 1, 1).data;
    $('backgroundColor').value = rgbHex(px[0], px[1], px[2]);
    state.pickingBackground = false;
    $('pickHint').classList.add('hidden');
    canvas.style.cursor = 'default';
    scheduleRender();
  });


  function setStatus(message, kind) {
    statusBox.textContent = message || '';
    statusBox.classList.toggle('hidden', !message);
    statusBox.classList.remove('error', 'success');
    if (kind) statusBox.classList.add(kind);
  }

  function normalizedPngName() {
    var name = $('wallpaperFilename').value.trim() || 'sleep-overlay.png';
    if (!/\.png$/i.test(name)) name += '.png';
    return name.replace(/[\\/:*?"<>|]+/g, '-');
  }

  function canvasPngBlob() {
    renderNow();
    return new Promise(function (resolve, reject) {
      canvas.toBlob(function (blob) {
        if (blob) resolve(blob);
        else reject(new Error(localized('wallpaper.export_error', 'Could not create PNG.')));
      }, 'image/png');
    });
  }

  async function ensureSleepFolder() {
    var form = new URLSearchParams();
    form.set('path', '/');
    form.set('name', 'sleep');
    try {
      // 400 "already exists" is harmless; upload below is the real check.
      await fetch('/mkdir', {
        method: 'POST',
        headers: {'Content-Type': 'application/x-www-form-urlencoded;charset=UTF-8'},
        body: form.toString()
      });
    } catch (_) {}
  }

  function uploadPngToReader(blob, name) {
    return new Promise(function (resolve, reject) {
      var file = new File([blob], name, {type:'image/png'});
      var data = new FormData();
      data.append('file', file, name);

      var xhr = new XMLHttpRequest();
      xhr.open('POST', '/upload?path=' + encodeURIComponent('/sleep'), true);
      xhr.onload = function () {
        if (xhr.status >= 200 && xhr.status < 300) resolve();
        else reject(new Error(xhr.responseText || ('HTTP ' + xhr.status)));
      };
      xhr.onerror = function () { reject(new Error('Upload connection failed')); };
      xhr.send(data);
    });
  }

  async function activateWallpaper(path) {
    var form = new URLSearchParams();
    form.set('path', path);
    var response = await fetch('/api/wallpapers/apply', {
      method:'POST',
      headers:{'Content-Type':'application/x-www-form-urlencoded;charset=UTF-8'},
      body:form.toString()
    });
    if (!response.ok) throw new Error(await response.text() || ('HTTP ' + response.status));
  }

  applyBtn.addEventListener('click', async function () {
    if (!state.image) return;

    applyBtn.disabled = true;
    downloadBtn.disabled = true;
    setStatus(localized('wallpaper.applying', 'Готовлю PNG и отправляю на устройство…'));

    try {
      var blob = await canvasPngBlob();
      var name = normalizedPngName();
      await ensureSleepFolder();

      setStatus(localized('wallpaper.uploading', 'Загружаю PNG на устройство…'));
      await uploadPngToReader(blob, name);

      var path = '/sleep/' + name;
      setStatus(localized('wallpaper.activating', 'Назначаю заставку…'));
      await activateWallpaper(path);

      setStatus(localized('wallpaper.applied', 'Готово. Теперь можно красиво уснуть.'), 'success');
    } catch (error) {
      setStatus(localized('wallpaper.apply_failed', 'Не удалось установить заставку: ') +
                (error && error.message ? error.message : String(error)), 'error');
    } finally {
      applyBtn.disabled = false;
      downloadBtn.disabled = false;
    }
  });

  downloadBtn.addEventListener('click', async function () {
    if (!state.image) return;
    try {
      var blob = await canvasPngBlob();
      var name = normalizedPngName();
      var url = URL.createObjectURL(blob);
      var a = document.createElement('a');
      a.href = url;
      a.download = name;
      document.body.appendChild(a);
      a.click();
      a.remove();
      setTimeout(function () { URL.revokeObjectURL(url); }, 1500);
    } catch (error) {
      alert(error && error.message ? error.message : String(error));
    }
  });

  applyDeviceSize();
})();
