// get current path from query parameter
  const currentPath = decodeURIComponent(new URLSearchParams(window.location.search).get('path') || '/');

  if (currentPath !== '/') {
    const leaf = currentPath.split('/').filter(Boolean).pop();
    if (leaf) document.title = leaf + ' - Files - inkMOD Reader';
  }

  // Network status monitoring
  let isNetworkOnline = navigator.onLine;

  // Add network status listeners
  window.addEventListener('online', () => {
    console.log('[Network] Online event fired');
    isNetworkOnline = true;
    showNotification(t('files.network_restored'), 'success');
  });

  window.addEventListener('offline', () => {
    console.log('[Network] Offline event fired');
    isNetworkOnline = false;
    showNotification(t('files.network_lost'), 'warning');
  });

  // Initialize network status
  console.log('[Network] Initial status:', isNetworkOnline ? 'online' : 'offline');

  function escapeHtml(unsafe) {
    return unsafe
      .replaceAll("&", "&amp;")
      .replaceAll("<", "&lt;")
      .replaceAll(">", "&gt;")
      .replaceAll('"', "&quot;")
      .replaceAll("'", "&#039;");
  }

  function showNotification(message, type = 'info') {
    // Create notification element if it doesn't exist
    let notification = document.getElementById('notification');
    if (!notification) {
      notification = document.createElement('div');
      notification.id = 'notification';
      notification.style.cssText = `
        position: fixed;
        top: 20px;
        right: 20px;
        padding: 12px 20px;
        border-radius: 4px;
        color: white;
        font-weight: 500;
        z-index: 10000;
        max-width: 300px;
        box-shadow: 0 2px 8px rgba(0,0,0,0.2);
        transition: all 0.3s ease;
      `;
      document.body.appendChild(notification);
    }

    // Set styles based on type
    const styles = {
      'success': 'background-color: #27ae60;',
      'error': 'background-color: #e74c3c;',
      'warning': 'background-color: #f39c12;',
      'info': 'background-color: #3498db;'
    };
    notification.style.cssText += styles[type] || styles['info'];
    notification.textContent = message;

    // Show notification
    notification.style.opacity = '1';
    notification.style.transform = 'translateX(0)';

    // Auto-hide after 5 seconds
    setTimeout(() => {
      notification.style.opacity = '0';
      notification.style.transform = 'translateX(100%)';
      setTimeout(() => {
        if (notification.parentNode) {
          notification.parentNode.removeChild(notification);
        }
      }, 300);
    }, 5000);
  }

  function formatFileSize(bytes) {
    if (bytes === 0) return '0 B';
    const k = 1024;
    const sizes = ['B', 'KB', 'MB', 'GB', 'TB', 'PB', 'EB', 'ZB', 'YB'];
    const i = Math.floor(Math.log(bytes) / Math.log(k));
    return parseFloat((bytes / Math.pow(k, i)).toFixed(2)).toLocaleString() + ' ' + sizes[i];
  }

  async function hydrate() {
    // Fetch inkMOD version
    fetchVersion();

    // Close modals when clicking overlay - call proper cleanup functions
    document.querySelectorAll('.modal-overlay').forEach(function(overlay) {
      overlay.addEventListener('click', function(e) {
        if (e.target === overlay) {
          // Call the appropriate close function for each modal to ensure cleanup
          if (overlay.id === 'uploadModal') return closeUploadModal();
          if (overlay.id === 'folderModal') return closeFolderModal();
          if (overlay.id === 'deleteModal') return closeDeleteModal();
          if (overlay.id === 'renameModal') return closeRenameModal();
          if (overlay.id === 'moveModal') return closeMoveModal();
          if (overlay.id === 'dictionaryModal') return closeDictionaryModal();
          overlay.classList.remove('open');
        }
      });
    });

    const breadcrumbs = document.getElementById('directory-breadcrumbs');
    const fileTable = document.getElementById('file-table');

    let breadcrumbContent = '<span class="sep">/</span>';
    if (currentPath === '/') {
      breadcrumbContent += '<span class="current">🏠</span>';
    } else {
      breadcrumbContent += '<a href="/files">🏠</a>';
      const pathSegments = currentPath.split('/');
      pathSegments.slice(1, pathSegments.length - 1).forEach(function(segment, index) {
        breadcrumbContent += '<span class="sep">/</span><a href="/files?path=' + encodeURIComponent(pathSegments.slice(0, index + 2).join('/')) + '">' + escapeHtml(segment) + '</a>';
      });
      breadcrumbContent += '<span class="sep">/</span>';
      breadcrumbContent += '<span class="current">' + escapeHtml(pathSegments[pathSegments.length - 1]) + '</span>';
    }
    breadcrumbs.innerHTML = breadcrumbContent;

    let files = [];
    try {
      const pageSize = 16;
      let offset = 0;
      for (;;) {
        const controller = new AbortController();
        const timeoutId = setTimeout(() => controller.abort(), 12000);
        let response;
        try {
          response = await fetch(
            '/api/files?paged=1&path=' + encodeURIComponent(currentPath) +
            '&offset=' + offset + '&limit=' + pageSize + '&_=' + Date.now(),
            { signal: controller.signal, cache: 'no-store' }
          );
        } finally {
          clearTimeout(timeoutId);
        }

        if (!response.ok) {
          throw new Error(t('files.load_files_failed', {msg: response.status + ' ' + response.statusText}));
        }

        const page = await response.json();
        if (!Array.isArray(page)) throw new Error('Invalid file-list response');
        files.push(...page);

        const hasMore = response.headers.get('X-InkMOD-Has-More') === '1';
        if (!hasMore || page.length === 0) break;
        offset += page.length;

        // Safety guard against a corrupt server cursor.
        if (offset > 4096) throw new Error('Too many file-list entries');
      }
    } catch (e) {
      console.error('[Files] load failed:', e);
      const message = e && e.name === 'AbortError'
        ? 'File list request timed out'
        : 'An error occurred while loading the files';
      fileTable.innerHTML = '<div class="no-files">' + message + '</div>';
      return;
    }

    let folderCount = 0;
    let totalSize = 0;
    files.forEach(file => {
      if (file.isDirectory) folderCount++;
      totalSize += file.size;
    });

    const fileCount = files.length - folderCount;
    const folderLabel = folderCount === 1 ? 'folder' : 'folders';
    const fileLabel = fileCount === 1 ? 'file' : 'files';
    document.getElementById('folder-summary').innerHTML = `${folderCount} ${folderLabel}, ${fileCount} ${fileLabel}, ${formatFileSize(totalSize)}`;

    if (files.length === 0) {
      fileTable.innerHTML = '<div class="no-files">This folder is empty</div>';
    } else {
      let fileTableContent = '<table class="file-table">';

				// Add select-all checkbox column
      fileTableContent += '<tr><th style="width:40px"><input type="checkbox" id="selectAllCheckbox" onchange="toggleSelectAll(this)"></th><th>Name</th><th>Type</th><th>Size</th><th class="actions-col">Actions</th></tr>';


      const sortedFiles = files.sort((a, b) => {
        // Directories first, then epub files, then other files, alphabetically within each group
        if (a.isDirectory && !b.isDirectory) return -1;
        if (!a.isDirectory && b.isDirectory) return 1;
        if (a.isEpub && !b.isEpub) return -1;
        if (!a.isEpub && b.isEpub) return 1;
        return a.name.localeCompare(b.name);
      });

      sortedFiles.forEach(file => {
        const lowerFileNameForBadge = file.isDirectory ? '' : file.name.toLowerCase();
        const isFb2File = isFb2Name(lowerFileNameForBadge);
        if (file.isDirectory) {
          let folderPath = currentPath;
          if (!folderPath.endsWith("/")) folderPath += "/";
          folderPath += file.name;

         // Checkbox cell + folder row
          fileTableContent += `<tr class="folder-row">`;
          fileTableContent += `<td><input type="checkbox" class="select-item" data-path="${encodeURIComponent(folderPath)}" data-name="${escapeHtml(file.name)}" data-type="folder"></td>`;
          fileTableContent += `<td><span class="file-icon">📁</span><a href="/files?path=${encodeURIComponent(folderPath)}" class="folder-link">${escapeHtml(file.name)}</a></td>`;
          fileTableContent += '<td><span class="folder-badge">FOLDER</span></td>';
          fileTableContent += '<td>-</td>';
          fileTableContent += `<td class="actions-col"><div class="action-icon-group">`;
          fileTableContent += `<button class="rename-btn" onclick="openRenameModal('${file.name.replaceAll("'", "\\'")}', '${folderPath.replaceAll("'", "\\'")}', true)" title="${escapeHtml(t('files.rename_btn'))}">✏️</button>`;
          fileTableContent += `<button class="delete-btn" onclick="openDeleteModal('${file.name.replaceAll("'", "\\'")}', '${folderPath.replaceAll("'", "\\'")}', true)" title="${escapeHtml(t('files.title_delete_folder'))}">🗑️</button>`;
          fileTableContent += `</div></td>`;
          fileTableContent += '</tr>';
        } else {
          let filePath = currentPath;
          if (!filePath.endsWith("/")) filePath += "/";
          filePath += file.name;

          // Checkbox cell + file row
          fileTableContent += `<tr class="${file.isEpub ? 'epub-file' : (isFb2File ? 'fb2-file' : '')}">`;
          fileTableContent += `<td><input type="checkbox" class="select-item" data-path="${encodeURIComponent(filePath)}" data-name="${escapeHtml(file.name)}" data-type="file"></td>`;
          fileTableContent += `<td><span class="file-icon">${file.isEpub ? '📗' : (isFb2File ? '📘' : '📄')}</span>`;
          fileTableContent += `<a rel="noopener noreferrer" target="_blank" href="/download?path=${encodeURIComponent(filePath)}" class="file-link">${escapeHtml(file.name)}</a>`;
          fileTableContent += '</td>';
          fileTableContent += file.isEpub
            ? '<td><span class="epub-badge">EPUB</span></td>'
            : (isFb2File
                ? `<td><span class="epub-badge fb2-badge">${lowerFileNameForBadge.endsWith('.zip') ? 'FB2.ZIP' : 'FB2'}</span></td>`
                : `<td>${escapeHtml(file.name.split('.').pop().toUpperCase())}</td>`);
          fileTableContent += `<td>${formatFileSize(file.size)}</td>`;
          fileTableContent += `<td class="actions-col"><div class="action-icon-group">`;
          fileTableContent += `<button class="move-btn" onclick="openMoveModal('${file.name.replaceAll("'", "\\'")}', '${filePath.replaceAll("'", "\\'")}' )" title="${escapeHtml(t('files.title_move_file'))}">📂</button>`;
          fileTableContent += `<button class="rename-btn" onclick="openRenameModal('${file.name.replaceAll("'", "\\'")}', '${filePath.replaceAll("'", "\\'")}', false)" title="${escapeHtml(t('files.title_rename_file'))}">✏️</button>`;
          fileTableContent += `<button class="delete-btn" onclick="openDeleteModal('${file.name.replaceAll("'", "\\'")}', '${filePath.replaceAll("'", "\\'")}', false)" title="${escapeHtml(t('files.title_delete_file'))}">🗑️</button>`;
          fileTableContent += `</div></td>`;
          fileTableContent += '</tr>';
        }
      });

      fileTableContent += '</table>';
      fileTable.innerHTML = fileTableContent;
    }
  }

  // Modal functions
  function openUploadModal() {
    // Reset converter variables to defaults
    ENABLE_GRAYSCALE = true;
    JPEG_QUALITY = 85;
    HANDEDNESS = 'right';
    OVERLAP_PERCENT = 5;
    imageStates = {};

    // Hide convert options when opening modal (no files selected initially)
    const convertOptions = document.getElementById('convertOptions');
    if (convertOptions) {
      convertOptions.style.display = 'none';
    }

    // Reset rotation and overlap UI
    setHandedness('right');
    setOverlap(5);

    // Hide log section from previous session
    const logSection = document.getElementById('log-section');
    if (logSection) logSection.classList.remove('visible');
    const logContainer = document.getElementById('log-container');
    if (logContainer) logContainer.innerHTML = '';

    document.getElementById('uploadPathDisplay').textContent = currentPath === '/' ? '/ 🏠' : currentPath;
    document.getElementById('uploadModal').classList.add('open');
  }

  function handleCancelUploadModal() {
    if (!isUploadInProgress) {
      // No process running: close the modal normally
      closeUploadModal();
      return;
    }
    // Process running: stop it, keep modal open for retry
    operationCancelled = true;
    if (currentUploadWs) { currentUploadWs.close(); currentUploadWs = null; }
    if (currentUploadXhr) { currentUploadXhr.abort(); currentUploadXhr = null; }
    // isUploadInProgress and UI are restored by restoreAfterCancel() from the async handlers
  }

  function restoreAfterCancel() {
    operationCancelled = false;
    isUploadInProgress = false;
    document.getElementById('uploadModalClose').classList.remove('disabled');
    const progressFill = document.getElementById('progress-fill');
    const progressText = document.getElementById('progress-text');
    progressFill.style.width = '0%';
    progressFill.style.backgroundColor = '#e74c3c';
    progressText.style.color = '#e74c3c';
    progressText.textContent = t('files.upload_cancelled');
    // Re-enable the action button (uploadBtn is always visible at this point;
    // its text is already "Upload" or "Optimize & Upload" depending on checkbox state)
    document.getElementById('uploadBtn').disabled = false;
  }

  function closeUploadModal() {
    // Prevent closing during upload/conversion
    if (isUploadInProgress) {
      return;
    }
    document.getElementById('uploadModal').classList.remove('open');
    const fileInput = document.getElementById('fileInput');
    fileInput.value = '';
    fileInput.classList.remove('has-files');
    document.getElementById('folderPickerInput').value = '';
    folderPickedFiles = null;
    document.getElementById('folderModeInfo').style.display = 'none';
    const uploadBtn = document.getElementById('uploadBtn');
    uploadBtn.disabled = true;
    uploadBtn.style.display = 'block';
    uploadBtn.textContent = t('files.upload_btn_plain');
    uploadBtn.classList.remove('optimize');
    document.getElementById('startConversionBtn').style.display = 'none';
    document.getElementById('progress-container').style.display = 'none';
    document.getElementById('progress-fill').style.width = '0%';
    document.getElementById('progress-fill').style.backgroundColor = '#27ae60';
    document.getElementById('convertBeforeUpload').checked = false;
    document.getElementById('prepareBookBeforeUpload').checked = false;
    document.getElementById('convertInfo').style.display = 'none';
    document.getElementById('convertWarning').style.display = 'none';
    document.getElementById('fb2ToEpubCheckbox').checked = false;
    document.getElementById('fb2ToEpubRow').style.display = 'none';
    // Clear image picker cache and reset layout
    epubImagesCache = [];
    imageStates = {};
    document.getElementById('imagePickerSection').style.display = 'none';
    const imageGrid = document.getElementById('imageGrid');
    if (imageGrid) imageGrid.innerHTML = '';
    document.querySelector('#uploadModal .modal').classList.remove('picker-mode');
    document.getElementById('pickerColumns').classList.remove('picker-active');
    // Hide log section
    if (logSection) logSection.classList.remove('visible');
    // Reset advanced options
    const advancedSettingsContent = document.getElementById('advancedSettingsContent');
    if (advancedSettingsContent) advancedSettingsContent.classList.remove('visible');
    document.getElementById('advancedOptionsArrow').classList.remove('expanded');
    const advancedOptionsToggle = document.getElementById('advancedOptionsToggle');
    if (advancedOptionsToggle) {
      advancedOptionsToggle.style.opacity = '0.5';
      advancedOptionsToggle.style.pointerEvents = 'none';
    }
    // Reset to defaults
    document.getElementById('qualitySlider').value = 85;
    document.getElementById('qualityInput').value = 85;
    setHandedness('right');
    setOverlap(5);
    // Update converter variables
    updateQualitySettings();
  }

  function updateBatchModeUI(isBatch) {
    const rotationRow = document.getElementById('rotationSettingRow');
    const overlapRow = document.getElementById('overlapSettingRow');
    if (rotationRow) rotationRow.style.display = isBatch ? 'none' : '';
    if (overlapRow) overlapRow.style.display = isBatch ? 'none' : '';
  }

  function updateUploadBtnLabel() {
    const optimizeChecked = document.getElementById('convertBeforeUpload').checked;
    const prepareEl = document.getElementById('prepareBookBeforeUpload');
    const prepareRow = document.getElementById('prepareBookRow');
    const prepareChecked = !!(prepareEl && prepareEl.checked && prepareRow && prepareRow.style.display !== 'none');
    const fb2El = document.getElementById('fb2ToEpubCheckbox');
    const fb2Checked = !!(fb2El && fb2El.checked);
    const uploadBtn = document.getElementById('uploadBtn');
    if (optimizeChecked) {
      uploadBtn.textContent = 'Optimize & Upload';
      uploadBtn.classList.add('optimize');
    } else if (prepareChecked) {
      uploadBtn.textContent = 'Prepare & Upload';
      uploadBtn.classList.add('optimize');
    } else if (fb2Checked) {
      uploadBtn.textContent = 'Convert & Upload';
      uploadBtn.classList.add('optimize');
    } else {
      uploadBtn.textContent = t('files.upload_btn_plain');
      uploadBtn.classList.remove('optimize');
    }
  }

  function toggleConvertOptions() {
    const checked = document.getElementById('convertBeforeUpload').checked;
    document.getElementById('convertWarning').style.display = checked ? 'block' : 'none';
    document.getElementById('convertInfo').style.display = checked ? 'block' : 'none';
    // Update button text and style
    updateUploadBtnLabel();
    if (!checked) {
      // Clear image picker when unchecking
      clearImagePicker();
    }
    // Reset advanced options when toggling off
    if (!checked) {
      const advancedSettingsContent = document.getElementById('advancedSettingsContent');
      if (advancedSettingsContent) advancedSettingsContent.classList.remove('visible');
      document.getElementById('advancedOptionsArrow').classList.remove('expanded');
    }
    // Disable/enable advanced mode toggle
    const advancedOptionsToggle = document.getElementById('advancedOptionsToggle');
    if (advancedOptionsToggle) {
      advancedOptionsToggle.style.opacity = checked ? '1' : '0.5';
      advancedOptionsToggle.style.pointerEvents = checked ? 'auto' : 'none';
    }
  }

  function toggleAdvancedOptions() {
    // Check if advanced options toggle is enabled (optimize EPUB must be checked)
    const convertEnabled = document.getElementById('convertBeforeUpload').checked;
    if (!convertEnabled) {
      return; // Don't toggle if optimization is disabled
    }

    const content = document.getElementById('advancedSettingsContent');
    const arrow = document.getElementById('advancedOptionsArrow');
    const isExpanding = !content.classList.contains('visible');

    content.classList.toggle('visible');
    arrow.classList.toggle('expanded');

    // When expanding, show image picker if an EPUB is selected
    if (isExpanding) {
      const fileInput = document.getElementById('fileInput');
      const files = fileInput.files;
      if (files.length === 1 && files[0].name.toLowerCase().endsWith('.epub')) {
        showImagePicker(files[0]).catch(err => console.error('Image picker error:', err));
      }
    } else {
      // When collapsing, hide the picker and startConversionBtn
      document.getElementById('imagePickerSection').style.display = 'none';
      document.getElementById('startConversionBtn').style.display = 'none';
      document.getElementById('uploadBtn').style.display = 'block';
      document.getElementById('uploadBtn').disabled = false;
      document.querySelector('#uploadModal .modal').classList.remove('picker-mode');
      document.getElementById('pickerColumns').classList.remove('picker-active');
    }
  }

  function setQualityPreset(value) {
    document.getElementById('qualitySlider').value = value;
    document.getElementById('qualityInput').value = value;
    // Update active preset
    document.querySelectorAll('.quality-preset').forEach(btn => {
      btn.classList.remove('active');
      if (parseInt(btn.dataset.value, 10) === value) {
        btn.classList.add('active');
      }
    });
    updateQualitySettings();
  }

  function updateQualitySettings() {
    const quality = document.getElementById('qualitySlider').value;
    // Check if grayscaleToggle exists (may be hidden for compatibility with other devices)
    const grayscaleToggle = document.getElementById('grayscaleToggle');
    const grayscale = grayscaleToggle ? grayscaleToggle.checked : true; // Default to true for e-ink

    // Update displays (if element exists)
    const qualityDisplay = document.getElementById('qualityDisplaySimple');
    if (qualityDisplay) {
      qualityDisplay.textContent = '📦 ' + quality + '% JPEG';
    }

    // Update converter variables (used by processImage and applyGrayscale)
    JPEG_QUALITY = parseInt(quality, 10);
    ENABLE_GRAYSCALE = true; // Always grayscale for e-ink
  }

  function setHandedness(value) {
    HANDEDNESS = value;
    // Update UI
    document.getElementById('rotationCW').classList.remove('active');
    document.getElementById('rotationCCW').classList.remove('active');
    document.getElementById(value === 'right' ? 'rotationCW' : 'rotationCCW').classList.add('active');
    // Re-render grid to update rotation arrows
    if (document.getElementById('imagePickerSection').style.display !== 'none') {
      renderImageGrid();
    }
  }

  function setOverlap(value) {
    OVERLAP_PERCENT = value;
    // Update UI
    document.querySelectorAll('.overlap-btn').forEach(btn => {
      btn.classList.toggle('active', parseInt(btn.dataset.value) === value);
    });
  }

  // ============================================================================
  // Image Picker Functions
  // ============================================================================

  /**
   * Extract images from EPUB for preview
   * Returns array of {path, name, dataUrl, width, height}
   */
  async function extractImagesForPreview(file) {
    const zip = await JSZip.loadAsync(file);
    const imageExtensions = ['.png', '.gif', '.webp', '.bmp', '.jpg', '.jpeg'];

    // Collect all image paths first
    const allImages = [];
    for (const [path, fileObj] of Object.entries(zip.files)) {
      if (fileObj.dir) continue;
      const ext = path.substring(path.lastIndexOf('.')).toLowerCase();
      if (imageExtensions.includes(ext)) {
        allImages.push(path);
      }
    }

    // Try to get images in reading order from OPF spine
    let orderedImages = [];
    let coverImagePath = null; // Track cover image
    try {
      // Find OPF file
      let opfPath = null;
      zip.forEach(p => { if (p.toLowerCase().endsWith('.opf')) opfPath = p; });

      if (opfPath) {
        const opfContent = await zip.files[opfPath].async('string');
        const opfDir = opfPath.includes('/') ? opfPath.substring(0, opfPath.lastIndexOf('/')) : '';

        // Detect cover image from OPF
        let coverId = null;
        let m;
        // Try 1: properties="cover-image"
        if (m = opfContent.match(/<item[^>]+id=["']([^"']+)["'][^>]+properties="[^"]*cover-image[^"]*"/)) coverId = m[1];
        if (!coverId && (m = opfContent.match(/<item[^>]+properties="[^"]*cover-image[^"]*"[^>]+id=["']([^"']+)["']/))) coverId = m[1];
        // Try 2: meta name="cover" content="id"
        if (!coverId && (m = opfContent.match(/<meta\s+name=["']cover["']\s+content=["']([^"']+)["']/))) coverId = m[1];
        if (!coverId && (m = opfContent.match(/<meta\s+content=["']([^"']+)["']\s+name=["']cover["']/))) coverId = m[1];

        // Parse manifest to get id -> href mapping
        const manifest = {};
        const manifestRegex = /<item[^>]+id=["']([^"']+)["'][^>]+href=["']([^"']+)["'][^>]*>/gi;
        let match;
        while ((match = manifestRegex.exec(opfContent)) !== null) {
          const id = match[1];
          const href = match[2];
          const fullPath = opfDir ? opfDir + '/' + href : href;
          manifest[id] = fullPath;
          if (id === coverId) coverImagePath = fullPath;
        }
        // Also check reversed attribute order
        const manifestRegex2 = /<item[^>]+href=["']([^"']+)["'][^>]+id=["']([^"']+)["'][^>]*>/gi;
        while ((match = manifestRegex2.exec(opfContent)) !== null) {
          const href = match[1];
          const id = match[2];
          const fullPath = opfDir ? opfDir + '/' + href : href;
          manifest[id] = fullPath;
          if (id === coverId) coverImagePath = fullPath;
        }

        // Cover-page reconciliation — if the cover XHTML references a different
        // but byte-identical image, prefer the one actually displayed on the page.
        if (coverImagePath) {
          try {
            let coverXhtmlPath = null;
            const guideM = opfContent.match(/<(?:\w+:)?reference[^>]+type=["']cover["'][^>]+href=["']([^"']+)["']/i) ||
                            opfContent.match(/<(?:\w+:)?reference[^>]+href=["']([^"']+)["'][^>]+type=["']cover["']/i);
            if (guideM) {
              coverXhtmlPath = opfDir ? opfDir + '/' + decodeHref(guideM[1]) : decodeHref(guideM[1]);
            }
            if (!coverXhtmlPath) {
              const spineM = opfContent.match(/<(?:\w+:)?itemref[^>]+idref=["']([^"']+)["']/i);
              if (spineM && manifest[spineM[1]]) coverXhtmlPath = manifest[spineM[1]];
            }
            if (coverXhtmlPath && zip.files[coverXhtmlPath]) {
              const coverXhtml = await zip.files[coverXhtmlPath].async('string');
              const imgM = coverXhtml.match(/(?:src|xlink:href)=["']([^"']+)["']/i);
              if (imgM) {
                const href = imgM[1];
                const xDir = coverXhtmlPath.includes('/') ? coverXhtmlPath.substring(0, coverXhtmlPath.lastIndexOf('/')) : '';
                let pageImgPath = href.startsWith('../') ? xDir.split('/').slice(0, -1).join('/') + '/' + href.substring(3)
                                : href.startsWith('/') ? href.substring(1)
                                : xDir ? xDir + '/' + href : href;
                pageImgPath = pageImgPath.replace(/\/+/g, '/');
                for (const realPath of allImages) {
                  if (realPath === pageImgPath || realPath.endsWith('/' + href) || realPath.endsWith(href)) {
                    pageImgPath = realPath; break;
                  }
                }
                if (pageImgPath !== coverImagePath && allImages.includes(pageImgPath) && zip.files[pageImgPath]) {
                  const coverData = await zip.files[coverImagePath].async('arraybuffer');
                  const pageData = await zip.files[pageImgPath].async('arraybuffer');
                  if (coverData.byteLength === pageData.byteLength) {
                    const a = new Uint8Array(coverData);
                    const b = new Uint8Array(pageData);
                    let identical = true;
                    for (let i = 0; i < a.length; i++) { if (a[i] !== b[i]) { identical = false; break; } }
                    if (identical) coverImagePath = pageImgPath;
                  }
                }
              }
            }
          } catch (e) { /* non-critical */ }
        }

        // Parse spine to get reading order
        const spineOrder = [];
        const spineRegex = /<itemref[^>]+idref=["']([^"']+)["'][^>]*>/gi;
        while ((match = spineRegex.exec(opfContent)) !== null) {
          const idref = match[1];
          if (manifest[idref]) spineOrder.push(manifest[idref]);
        }

        // For each spine item (XHTML), extract images in order
        const seenImages = new Set();
        for (const xhtmlPath of spineOrder) {
          if (!zip.files[xhtmlPath]) continue;
          const xhtmlContent = await zip.files[xhtmlPath].async('string');
          const xhtmlDir = xhtmlPath.includes('/') ? xhtmlPath.substring(0, xhtmlPath.lastIndexOf('/')) : '';

          // Find all image references
          const imgRegex = /(?:src|xlink:href)=["']([^"']+)["']/gi;
          while ((match = imgRegex.exec(xhtmlContent)) !== null) {
            let imgHref = match[1];
            // Skip non-image references
            if (!imageExtensions.some(ext => imgHref.toLowerCase().endsWith(ext))) continue;

            // Resolve relative path
            let imgPath;
            if (imgHref.startsWith('../')) {
              // Go up from xhtmlDir
              const parts = xhtmlDir.split('/');
              parts.pop();
              imgPath = parts.join('/') + '/' + imgHref.substring(3);
            } else if (imgHref.startsWith('/')) {
              imgPath = imgHref.substring(1);
            } else {
              imgPath = xhtmlDir ? xhtmlDir + '/' + imgHref : imgHref;
            }
            // Normalize path
            imgPath = imgPath.replace(/\/+/g, '/');

            // Check if this is actually an image in our list
            for (const realPath of allImages) {
              if (realPath === imgPath || realPath.endsWith('/' + imgHref) || realPath.endsWith(imgHref)) {
                if (!seenImages.has(realPath)) {
                  seenImages.add(realPath);
                  orderedImages.push(realPath);
                }
                break;
              }
            }
          }
        }

        // Add any remaining images that weren't in XHTML files (e.g., unused images)
        for (const imgPath of allImages) {
          if (!seenImages.has(imgPath)) {
            orderedImages.push(imgPath);
          }
        }
      }
    } catch (e) {
      console.warn('Failed to parse reading order, using default:', e);
    }

    // Fallback to alphabetical if parsing failed
    if (orderedImages.length === 0) {
      orderedImages = [...allImages].sort();
    }

    // Load image data in order
    const images = [];
    for (const path of orderedImages) {
      const data = await zip.files[path].async('arraybuffer');
      const blob = new Blob([data]);
      const dataUrl = URL.createObjectURL(blob);

      // Get dimensions
      const dims = await getImageDimensions(data);

      // Check if this is the cover image
      const isCover = (path === coverImagePath) ||
                      path.toLowerCase().includes('cover') && images.length === 0;

      // Check if this is a separator/ornament (skip cover check)
      const filename = path.split('/').pop();
      let isSeparator = false;
      if (!isCover) {
        try {
          isSeparator = await isSeparatorImage(dataUrl, dims.width, dims.height, filename);
        } catch (e) {
          console.warn('Separator check failed for', filename, e);
        }
      }

      // Tiny images (<200x200) are locked like separators
      const isTiny = (dims.width < 200 && dims.height < 200);

      // Images that fit screen can only rotate, not split
      const fitsScreen = (dims.width <= MAX_WIDTH && dims.height <= MAX_HEIGHT);

      // Split capability - no upscaling allowed
      // H-Split scales width to MAX_HEIGHT (long edge), so needs width >= MAX_HEIGHT
      // V-Split scales height to MAX_HEIGHT, so needs height >= MAX_HEIGHT
      const canHSplit = dims.width >= MAX_HEIGHT;
      const canVSplit = dims.height >= MAX_HEIGHT;

      images.push({
        path: path,
        name: filename,
        dataUrl: dataUrl,
        width: dims.width,
        height: dims.height,
        isCover: isCover,
        isSeparator: isSeparator || isTiny,
        fitsScreen: fitsScreen,
        canHSplit: canHSplit,
        canVSplit: canVSplit
      });
    }

    return images;
  }

  /**
   * Get image dimensions from array buffer
   */
  function getImageDimensions(data) {
    return new Promise((resolve, reject) => {
      const url = URL.createObjectURL(new Blob([data]));
      const img = new Image();
      img.onload = () => {
        URL.revokeObjectURL(url);
        resolve({ width: img.width, height: img.height });
      };
      img.onerror = () => {
        URL.revokeObjectURL(url);
        resolve({ width: 0, height: 0 });
      };
      img.src = url;
    });
  }

  /**
   * Check if image is a separator/ornament
   * Criteria: small size AND (filename match OR symmetric OR extreme aspect ratio)
   */
  async function isSeparatorImage(dataUrl, width, height, filename) {
    const MAX_DIMENSION = 150;
    const SYMMETRY_THRESHOLD = 0.85;

    // First check: must be small in at least one dimension
    const isSmall = (height < MAX_DIMENSION || width < MAX_DIMENSION);
    if (!isSmall) return false;

    // Filename hints (instant match if small + named correctly)
    const separatorNames = ['separator', 'divider', 'ornament', 'break', 'flourish', 'scene', 'divid', 'decor'];
    const lowerName = filename.toLowerCase();
    if (separatorNames.some(n => lowerName.includes(n))) return true;

    // Extreme aspect ratio check (>10:1 or <1:10) - these are definitely separators/lines
    const aspectRatio = width / height;
    if (aspectRatio > 10 || aspectRatio < 0.1) return true;

    // Symmetry check (skip for very thin images - too few pixels)
    if (width < 10 || height < 10) return true; // Very small = separator

    try {
      const isSymmetric = await checkHorizontalSymmetry(dataUrl, width, height, SYMMETRY_THRESHOLD);
      return isSymmetric;
    } catch (e) {
      console.warn('Symmetry check failed:', e);
      return false;
    }
  }

  /**
   * Check horizontal symmetry by comparing left and right halves
   */
  function checkHorizontalSymmetry(dataUrl, width, height, threshold) {
    return new Promise((resolve) => {
      const img = new Image();
      img.onload = () => {
        // Use small canvas for performance (max 100px wide)
        const scale = Math.min(1, 100 / width);
        const w = Math.max(2, Math.floor(width * scale));  // Minimum 2px
        const h = Math.max(1, Math.floor(height * scale)); // Minimum 1px

        const canvas = document.createElement('canvas');
        canvas.width = w;
        canvas.height = h;
        const ctx = canvas.getContext('2d');

        // Draw scaled image
        ctx.drawImage(img, 0, 0, w, h);
        const imageData = ctx.getImageData(0, 0, w, h);
        const pixels = imageData.data;

        // Compare left half with flipped right half
        const halfW = Math.floor(w / 2);
        let matchingPixels = 0;
        let totalPixels = 0;

        for (let y = 0; y < h; y++) {
          for (let x = 0; x < halfW; x++) {
            const leftIdx = (y * w + x) * 4;
            const rightIdx = (y * w + (w - 1 - x)) * 4;

            // Compare RGB (ignore alpha)
            const rDiff = Math.abs(pixels[leftIdx] - pixels[rightIdx]);
            const gDiff = Math.abs(pixels[leftIdx + 1] - pixels[rightIdx + 1]);
            const bDiff = Math.abs(pixels[leftIdx + 2] - pixels[rightIdx + 2]);

            // Allow some tolerance for JPEG artifacts (threshold of 30)
            if (rDiff < 30 && gDiff < 30 && bDiff < 30) {
              matchingPixels++;
            }
            totalPixels++;
          }
        }

        const symmetryScore = matchingPixels / totalPixels;
        resolve(symmetryScore >= threshold);
      };
      img.onerror = () => resolve(false);
      img.src = dataUrl;
    });
  }

  /**
   * Show image picker after EPUB file selection
   */
  async function showImagePicker(file) {
    // Check if JSZip is available
    if (typeof JSZip === 'undefined') {
      console.error('JSZip not loaded');
      alert(t('files.jszip_unavailable'));
      startConversionWithImageStates();
      return;
    }

    const pickerSection = document.getElementById('imagePickerSection');
    const imageGrid = document.getElementById('imageGrid');
    const countDisplay = document.getElementById('imagePickerCount');

    // Reset state
    imageStates = {};
    epubImagesCache = [];
    pendingConversionFile = file;

    // Extract images
    try {
      const images = await extractImagesForPreview(file);
      epubImagesCache = images;

      // Initialize all states to 0 (Normal)
      images.forEach(img => {
        imageStates[img.path] = 0;
      });

      // Build UI
      renderImageGrid();

      // Count images - covers and separators are locked
      const coverCount = images.filter(img => img.isCover).length;
      const separatorCount = images.filter(img => img.isSeparator).length;
      const lockedCount = coverCount + separatorCount;
      const selectableCount = images.length - lockedCount;

      if (lockedCount > 0) {
        const lockedParts = [];
        if (coverCount > 0) lockedParts.push(`${coverCount} cover`);
        if (separatorCount > 0) lockedParts.push(`${separatorCount} separator${separatorCount !== 1 ? 's' : ''}`);
        countDisplay.textContent = `${images.length} images (${selectableCount} selectable, ${lockedParts.join(', ')})`;
      } else {
        countDisplay.textContent = `${images.length} image${images.length !== 1 ? 's' : ''} (all selectable)`;
      }

      // Show picker, hide upload button, show start conversion button
      pickerSection.style.display = 'block';
      document.getElementById('uploadBtn').style.display = 'none';
      document.getElementById('startConversionBtn').style.display = 'block';
      // Enable two-column layout
      document.querySelector('#uploadModal .modal').classList.add('picker-mode');
      document.getElementById('pickerColumns').classList.add('picker-active');

    } catch (error) {
      console.error('Failed to extract images:', error);
      alert(t('files.preview_failed', {msg: error.message}));
      // Fallback: start conversion directly
      startConversionWithImageStates();
    }
  }

  /**
   * Render the image grid with current states
   */
  function renderImageGrid() {
    const imageGrid = document.getElementById('imageGrid');
    imageGrid.innerHTML = '';

    const stateLabels = ['', 'H-Split', 'V-Split', 'Rotate'];
    const stateClasses = ['state-0', 'state-1', 'state-2', 'state-3'];

    epubImagesCache.forEach(img => {
      // Cover images and separators are always locked (no splitting/rotation)
      const isCover = img.isCover;
      const isSeparator = img.isSeparator;
      const state = imageStates[img.path] || 0;

      const item = document.createElement('div');

      if (isCover) {
        // Cover image - locked, cannot be split
        item.className = 'image-item cover-locked';
        item.title = `${img.width}×${img.height} - Cover image (locked)`;
        item.innerHTML = `
          <span class="image-state-badge cover-badge">🔒</span>
          <img src="${img.dataUrl}" alt="${img.name}" loading="lazy">
          <div class="image-name">${img.name}</div>
        `;
      } else if (isSeparator) {
        // Separator/ornament - locked, cannot be split
        item.className = 'image-item separator-locked';
        item.title = `${img.width}×${img.height} - Separator (locked)`;
        item.innerHTML = `
          <span class="image-state-badge separator-badge">✦</span>
          <img src="${img.dataUrl}" alt="${img.name}" loading="lazy">
          <div class="image-name">${img.name}</div>
        `;
      } else {
        // All other images are selectable - all modes allowed

        // Determine which overlay to show based on state
        const showRotation = (state === 1 || state === 3); // H-Split or Rotate
        const showSplitLines = (state === 1 || state === 2); // H-Split or V-Split
        const rotateClass = showRotation ? (HANDEDNESS === 'right' ? 'rotate-cw' : 'rotate-ccw') : '';

        // Calculate actual number of parts for split preview
        let numParts = 1;
        if (showSplitLines) {
          let finalWidth;
          if (state === 1) {
            // H-Split: scale width to MAX_HEIGHT, rotate, then check width
            const scaledH = Math.round(img.height * (MAX_HEIGHT / img.width));
            finalWidth = scaledH; // After rotation, height becomes width
          } else {
            // V-Split: scale height to MAX_HEIGHT, then check width
            finalWidth = Math.round(img.width * (MAX_HEIGHT / img.height));
          }
          if (finalWidth > MAX_WIDTH) {
            const minOverlapPx = Math.round(MAX_WIDTH * (OVERLAP_PERCENT / 100));
            const maxStep = MAX_WIDTH - minOverlapPx;
            numParts = Math.ceil((finalWidth - minOverlapPx) / maxStep);
            if (numParts < 2) numParts = 2;
          }
        }

        // Generate split line elements (numParts - 1 lines at evenly distributed positions)
        let splitLinesHtml = '';
        if (showSplitLines && numParts > 1) {
          const lines = [];
          const splitClass = state === 1 ? 'split-h' : 'split-v';
          for (let i = 1; i < numParts; i++) {
            const pos = (i / numParts) * 100;
            lines.push(`<div class="split-line ${splitClass}" style="left:${pos}%"></div>`);
          }
          splitLinesHtml = `<div class="split-lines">${lines.join('')}</div>`;
        }

        // Build tooltip
        const stateText = stateLabels[state] || 'Normal';
        const partsText = numParts > 1 ? ` (${numParts} parts)` : '';

        item.className = `image-item ${stateClasses[state]} ${rotateClass}`.trim();
        item.onclick = () => cycleImageState(img.path);
        item.title = `${img.width}×${img.height} - ${stateText}${partsText}`;
        item.innerHTML = `
          <span class="image-state-badge">${stateLabels[state] || '•'}</span>
          <div class="image-preview-overlay">
            ${splitLinesHtml}
          </div>
          <img src="${img.dataUrl}" alt="${img.name}" loading="lazy">
          <div class="image-name">${img.name}</div>
        `;
      }

      imageGrid.appendChild(item);
    });
  }

  /**
   * Cycle state for a single image
   * All non-locked images: 0 -> 1 -> 2 -> 3 -> 0
   */
  function cycleImageState(imagePath) {
    const currentState = imageStates[imagePath] || 0;
    imageStates[imagePath] = (currentState + 1) % 4;
    renderImageGrid();
  }

  /**
   * Apply state to all eligible images based on smart rules
   * 0 = Normal (all selectable images)
   * 1 = H-Split (landscapes that canHSplit)
   * 2 = V-Split (all images that canVSplit - portraits AND landscapes)
   * 3 = Rotate (landscapes that don't fit screen)
   */
  function applyStateToAll(state) {
    epubImagesCache.forEach(img => {
      // Skip locked images
      if (img.isCover || img.isSeparator) return;

      const canHSplit = img.canHSplit && !img.fitsScreen;
      const canVSplit = img.canVSplit && !img.fitsScreen;
      const isLandscape = img.width > img.height;

      if (state === 0) {
        // Normal - applies to all selectable
        imageStates[img.path] = 0;
      } else if (state === 1) {
        // H-Split - all landscapes that can H-Split
        if (isLandscape && canHSplit) {
          imageStates[img.path] = 1;
        }
      } else if (state === 2) {
        // V-Split - all images that can V-Split (portrait and landscape)
        if (canVSplit) {
          imageStates[img.path] = 2;
        }
      } else if (state === 3) {
        // Rotate - landscapes that exceed screen
        if (isLandscape && !img.fitsScreen) {
          imageStates[img.path] = 3;
        }
      }
    });
    renderImageGrid();
  }

  /**
   * Start conversion with configured image states
   */
  function startConversionWithImageStates() {
    const pickerSection = document.getElementById('imagePickerSection');
    const uploadBtn = document.getElementById('uploadBtn');
    const startConversionBtn = document.getElementById('startConversionBtn');

    // Hide picker and start conversion button, remove two-column layout
    pickerSection.style.display = 'none';
    startConversionBtn.style.display = 'none';
    document.querySelector('#uploadModal .modal').classList.remove('picker-mode');
    document.getElementById('pickerColumns').classList.remove('picker-active');

    // Show upload button and trigger upload
    uploadBtn.style.display = 'block';
    uploadBtn.disabled = false;
    uploadFile();
  }

  /**
   * Get processing state for an image path
   * Returns 0 (Normal), 1 (H-Split), 2 (V-Split), or 3 (Rotate)
   */
  function getImageState(imagePath) {
    return imageStates[imagePath] || 0;
  }

  /**
   * Get state label for logging
   */
  function getStateLabel(state) {
    const labels = ['Normal', 'H-Split', 'V-Split', 'Rotate'];
    return labels[state] || 'Normal';
  }

  // Initialize quality settings handlers
  document.addEventListener('DOMContentLoaded', function() {
    const qualitySlider = document.getElementById('qualitySlider');
    const qualityInput = document.getElementById('qualityInput');

    // Initialize advanced mode toggle state based on Optimize EPUB checkbox
    const convertCheckbox = document.getElementById('convertBeforeUpload');
    const advancedToggle = document.getElementById('advancedOptionsToggle');
    if (convertCheckbox && advancedToggle) {
      const isOptimizeEnabled = convertCheckbox.checked;
      advancedToggle.style.opacity = isOptimizeEnabled ? '1' : '0.5';
      advancedToggle.style.pointerEvents = isOptimizeEnabled ? 'auto' : 'none';
    }

    if (qualitySlider && qualityInput) {
      // Initialize converter variables with UI default values
      updateQualitySettings();

      // Deselect all presets when slider is manually changed
      const deselectPresets = function() {
        document.querySelectorAll('.quality-preset').forEach(btn => {
          btn.classList.remove('active');
        });
      };

      qualitySlider.oninput = function() {
        qualityInput.value = this.value;
        deselectPresets();
        updateQualitySettings();
      };
      qualityInput.onchange = function() {
        let v = Math.max(1, Math.min(95, parseInt(this.value, 10) || 85));
        this.value = v;
        qualitySlider.value = v;
        deselectPresets();
        updateQualitySettings();
      };
      qualityInput.onblur = function() {
        this.value = qualitySlider.value;
      };
    }
  });

  function openFolderModal() {
    document.getElementById('folderPathDisplay').textContent = currentPath === '/' ? '/ 🏠' : currentPath;
    document.getElementById('folderModal').classList.add('open');
    document.getElementById('folderName').value = '';
    setTimeout(() => document.getElementById('folderName').focus(), 50);
    document.getElementById('folderName').onkeydown = (e) => { if (e.key === 'Enter' && e.target.value.trim()) createFolder(); };
  }

  function closeFolderModal() {
    document.getElementById('folderModal').classList.remove('open');
  }

  // Toggle select-all checkbox
  function toggleSelectAll(master) {
    const checked = master.checked;
    document.querySelectorAll('.select-item').forEach(cb => {
      cb.checked = checked;
    });
  }

  function getSelectedItems() {
    const items = [];
    document.querySelectorAll('.select-item:checked').forEach(cb => {
      items.push({
        name: cb.dataset.name || decodeURIComponent(cb.dataset.path).split('/').pop(),
        path: decodeURIComponent(cb.dataset.path),
        isFolder: cb.dataset.type === 'folder'
      });
    });
    return items;
  }

  // Open delete modal for currently selected checkboxes
  function openDeleteSelectedModal() {
    const items = getSelectedItems();
    if (items.length === 0) {
      alert(t('files.please_select_delete_item'));
      return;
    }
    openDeleteModalForItems(items);
  }

  // Open delete modal for a single item (keeps backwards compatibility with per-row delete button)
  function openDeleteModal(name, path, isFolder) {
    openDeleteModalForItems([{ name: name, path: path, isFolder: !!isFolder }]);
  }

  let deleteItemsGlobal = [];

  function openDeleteModalForItems(items) {
    deleteItemsGlobal = items;
    const listEl = document.getElementById('deleteItemList');
    listEl.innerHTML = '';
    items.forEach(it => {
      const div = document.createElement('div');
      div.style.marginBottom = '6px';
      div.textContent = (it.isFolder ? '📁 ' : '📄 ') + it.path;
      listEl.appendChild(div);
    });
    document.getElementById('deleteModal').classList.add('open');
  }

  function closeDeleteModal() {
    document.getElementById('deleteModal').classList.remove('open');
  }

  function confirmDelete() {
    if (!deleteItemsGlobal || deleteItemsGlobal.length === 0) {
      closeDeleteModal();
      return;
    }

    const paths = deleteItemsGlobal.map(it => {
      // Ensure path starts with /
      let p = it.path;
      if (!p.startsWith('/')) p = '/' + p;
      return p;
    });

    const body = 'paths=' + encodeURIComponent(JSON.stringify(paths));
    fetch('/delete', {
      method: 'POST',
      headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
      body: body
    }).then(async res => {
      if (res.ok) {
        window.location.reload();
      } else {
        const text = await res.text();
        alert(t('files.delete_failed', {msg: text}));
        closeDeleteModal();
      }
    }).catch(() => {
      alert(t('files.delete_net_error'));
      closeDeleteModal();
    });
  }

  // Helper to clear image picker state
  function clearImagePicker() {
    epubImagesCache = [];
    imageStates = {};
    const imageGrid = document.getElementById('imageGrid');
    if (imageGrid) imageGrid.innerHTML = '';
    const pickerSection = document.getElementById('imagePickerSection');
    if (pickerSection) pickerSection.style.display = 'none';
    // Reset two-column layout
    document.querySelector('#uploadModal .modal')?.classList.remove('picker-mode');
    document.getElementById('pickerColumns')?.classList.remove('picker-active');
    // Hide start conversion, show upload
    const startBtn = document.getElementById('startConversionBtn');
    if (startBtn) startBtn.style.display = 'none';
    const uploadBtn = document.getElementById('uploadBtn');
    if (uploadBtn) uploadBtn.style.display = 'block';
  }

  // Set up file input click listener once
  (function setupFileInputListener() {
    const fileInput = document.getElementById('fileInput');
    if (!fileInput) return;

    fileInput.addEventListener('click', function() {
      // Small delay to allow browser to process click before checking files
      setTimeout(() => {
        if (fileInput.files.length === 0) {
          clearImagePicker();
          document.getElementById('uploadBtn').disabled = true;
        }
      }, 10);
    });
  })();

  // Folder upload: reuse the exact same modal/conversion/upload pipeline as a
  // normal multi-file pick by transplanting the picked FileList (which
  // carries webkitRelativePath per file) onto the regular #fileInput via
  // DataTransfer — the same trick the failed-upload retry buttons already use.
  function handleFolderPicked() {
    const folderInput = document.getElementById('folderPickerInput');
    if (!folderInput.files || folderInput.files.length === 0) return;

    // IMPORTANT (Safari/macOS): keep the original File objects. Rebuilding a
    // FileList through DataTransfer may drop webkitRelativePath, which destroys
    // the directory structure and makes large folder retries unreliable.
    folderPickedFiles = Array.from(folderInput.files);

    // Mirror the selection into the regular input only as a best-effort UI aid.
    // The actual upload pipeline below uses folderPickedFiles directly.
    try {
      const dt = new DataTransfer();
      folderPickedFiles.forEach(f => dt.items.add(f));
      document.getElementById('fileInput').files = dt.files;
    } catch (_) { /* Safari may not allow programmatic FileList assignment */ }
    folderInput.value = '';

    openUploadModal();
    validateFile(true);
  }

  function validateFile(fromFolderPicker = false) {
    const fileInput = document.getElementById('fileInput');
    const uploadBtn = document.getElementById('uploadBtn');
    if (!fromFolderPicker) folderPickedFiles = null;
    const files = folderPickedFiles || fileInput.files;
    const convertOptions = document.getElementById('convertOptions');
    fileInput.classList.toggle('has-files', files.length > 0);

    // Folder uploads carry a webkitRelativePath ("TopFolder/sub/book.fb2") on
    // every file; show what will be recreated on the device so it's not a surprise.
    const folderModeInfo = document.getElementById('folderModeInfo');
    const relPaths = Array.from(files).map(f => f.webkitRelativePath || '').filter(Boolean);
    if (relPaths.length > 0) {
      const subfolders = new Set(relPaths.map(p => p.slice(0, p.length - p.split('/').pop().length).replace(/\/$/, '')).filter(Boolean));
      folderModeInfo.textContent = `📁 Folder upload: ${files.length} file(s) across ${subfolders.size} subfolder(s) — structure will be recreated here.`;
      folderModeInfo.style.display = 'block';
    } else if (fromFolderPicker && files.length > 0) {
      // Folder was picked but no file reported a webkitRelativePath — this browser/
      // webview doesn't expose folder structure, so every file will land flat in
      // the current directory. Say so instead of silently flattening.
      folderModeInfo.textContent = `⚠️ This browser didn't report the folder structure — all ${files.length} file(s) will be uploaded directly into the current folder (no subfolders).`;
      folderModeInfo.style.display = 'block';
    } else {
      folderModeInfo.style.display = 'none';
    }

    // Show convert options when at least one selected file is an EPUB, FB2, FB2.ZIP,
    // or a standalone image (jpg/png/gif/webp/bmp) — images get resized to fit the
    // device screen the same way EPUB/FB2 covers do.
    const hasConvertible = Array.from(files).some(f => {
      const n = f.name.toLowerCase();
      return n.endsWith('.epub') || isFb2Name(n) || n.endsWith('.zip') || isImageName(n);
    });
    const hasFb2 = Array.from(files).some(f => {
      const n = f.name.toLowerCase();
      return isFb2Name(n);
    });
    const hasBook = Array.from(files).some(f => {
      const n = f.name.toLowerCase();
      // Generic ZIP remains eligible because content detection happens
      // asynchronously later (it may contain either EPUB or FB2).
      return n.endsWith('.epub') || isFb2Name(n) || n.endsWith('.zip');
    });
    const prepareBookRow = document.getElementById('prepareBookRow');
    if (prepareBookRow) prepareBookRow.style.display = hasBook ? 'flex' : 'none';
    const fb2ToEpubRow = document.getElementById('fb2ToEpubRow');
    if (files.length > 0 && hasFb2) {
      fb2ToEpubRow.style.display = 'flex';
    } else {
      fb2ToEpubRow.style.display = 'none';
      const fb2Cb = document.getElementById('fb2ToEpubCheckbox');
      if (fb2Cb) fb2Cb.checked = false;
      updateUploadBtnLabel();
    }
    if (files.length > 0 && hasConvertible) {
      convertOptions.style.display = 'block';
      // Conversion/preparation must always be explicit. A plain upload is a
      // byte-for-byte copy regardless of file size. Do NOT auto-enable either
      // option for large EPUB/FB2/ZIP files.
    } else {
      convertOptions.style.display = 'none';
      // Clear stale checkbox state so the "Optimize & Upload" button doesn't linger
      // when the user re-picks a non-convertible file after having ticked Optimize.
      const cb = document.getElementById('convertBeforeUpload');
      if (cb && cb.checked) {
        cb.checked = false;
        toggleConvertOptions();
      }
      if (files.length === 0) clearImagePicker();
    }

    if (files.length > 0) {
      // If advanced settings is expanded and single EPUB, show image picker.
      // FB2 has no per-image picker — optimization always just resizes the cover.
      const advancedContent = document.getElementById('advancedSettingsContent');
      const convertEnabled = document.getElementById('convertBeforeUpload').checked;
      if (advancedContent.classList.contains('visible') && files.length === 1 && convertEnabled && files[0].name.toLowerCase().endsWith('.epub')) {
        showImagePicker(files[0]).catch(err => console.error('Image picker error:', err));
      } else {
        // New selection no longer matches single-EPUB-with-advanced-expanded —
        // tear down any picker from a previous file so the grid and picker-mode
        // layout don't linger. clearImagePicker is idempotent.
        clearImagePicker();
      }

      // If multiple files with conversion, inform user about batch mode
      if (files.length > 1 && convertEnabled) {
        const convertibleCount = Array.from(files).filter(f => {
          const n = f.name.toLowerCase();
          return n.endsWith('.epub') || isFb2Name(n) || isImageName(n);
        }).length;
        if (convertibleCount > 0) {
          console.log(`Batch mode: ${convertibleCount} EPUB/FB2 file(s) will use auto settings`);
        }
      }

      updateBatchModeUI(files.length > 1);
      uploadBtn.disabled = false;
    } else {
      updateBatchModeUI(false);
      uploadBtn.disabled = true;
    }
  }

let failedUploadsGlobal = [];
let wsConnection = null;
let isUploadInProgress = false; // Prevent modal close during upload/conversion
let operationCancelled = false; // Set by Cancel to stop conversion loops and upload async handlers
let uploadGeneration = 0;       // Incremented each uploadFile() call; guards stale restoreAfterCancel()
let currentUploadWs = null;     // Active WebSocket reference for external abort
let currentUploadXhr = null;    // Active XHR reference for external abort
let folderPickedFiles = null;   // Preserve Safari's original File objects + webkitRelativePath
const WS_PORT = 81;
const WS_CHUNK_SIZE = 4096; // 4KB chunks - smaller for ESP32 stability
const WS_LARGE_FILE_LIMIT = 8 * 1024 * 1024; // large files use HTTP from the start
const IS_SAFARI = /^((?!chrome|chromium|crios|android).)*safari/i.test(navigator.userAgent);

// ============================================================================
// EPUB Image Conversion Functions (from Baseline JPEG Converter)
// ============================================================================

// Device profiles (short edge × long edge in portrait orientation)
const DEVICE_PROFILES = {
  X4: { width: 480, height: 800, label: 'X4' },
  X3: { width: 528, height: 792, label: 'X3' },
};

// Default conversion settings
const DEFAULT_DEVICE = 'X4';
const DEFAULT_MAX_WIDTH = DEVICE_PROFILES[DEFAULT_DEVICE].width;
const DEFAULT_MAX_HEIGHT = DEVICE_PROFILES[DEFAULT_DEVICE].height;
const DEFAULT_JPEG_QUALITY = 85;
const DEFAULT_ENABLE_GRAYSCALE = true;
// Note: Overlap is now always centered distribution (min 5%)

// Dynamic conversion settings (updated by UI)
let DEVICE_TARGET = 'auto';      // 'auto' | 'X4' | 'X3'
let DETECTED_DEVICE = null;      // populated from /api/status
let ACTIVE_DEVICE = DEFAULT_DEVICE;
let MAX_WIDTH = DEFAULT_MAX_WIDTH;
let MAX_HEIGHT = DEFAULT_MAX_HEIGHT;
let JPEG_QUALITY = DEFAULT_JPEG_QUALITY;
let ENABLE_GRAYSCALE = DEFAULT_ENABLE_GRAYSCALE;
let HANDEDNESS = 'right'; // 'right' = clockwise (right-handed), 'left' = counter-clockwise (left-handed)
let OVERLAP_PERCENT = 5; // Minimum overlap percentage for splits (5%, 10%, 15%)

// ============================================================================
// Image Picker State Management
// ============================================================================

let imageStates = {};        // Map: imagePath -> state (0=Normal, 1=H-Split, 2=V-Split, 3=Rotate)
let epubImagesCache = [];    // Cache of extracted images for preview
let pendingConversionFile = null;  // File awaiting conversion after image selection

// ============================================================================
// Enhanced Logging System
// ============================================================================

let logStartTime = null;
let conversionStats = { images: 0, splits: 0, splitParts: 0, fixes: 0, skipped: 0, errors: 0, originalSize: 0, newSize: 0 };
const logSection = document.getElementById('log-section');
const logContainer = document.getElementById('log-container');
const exportLogCheckbox = document.getElementById('export-log-checkbox');

// inkMOD version (fetched from API)
let inkmodVersion = 'Unknown';

// Fetch version from API
async function fetchVersion() {
  try {
    const response = await fetch('/api/status');
    if (response.ok) {
      const data = await response.json();
      inkmodVersion = data.version || 'Unknown';
      if (data.device === 'X3' || data.device === 'X4') {
        DETECTED_DEVICE = data.device;
        applyDeviceTarget();
      }
    }
  } catch (e) {
    console.error('Failed to fetch version:', e);
  }
}

// Resolve DEVICE_TARGET ('auto' | 'X4' | 'X3') to a concrete profile and update UI.
function applyDeviceTarget() {
  const resolved = DEVICE_TARGET === 'auto' ? (DETECTED_DEVICE || DEFAULT_DEVICE) : DEVICE_TARGET;
  const profile = DEVICE_PROFILES[resolved] || DEVICE_PROFILES[DEFAULT_DEVICE];
  ACTIVE_DEVICE = resolved;
  MAX_WIDTH = profile.width;
  MAX_HEIGHT = profile.height;

  const summary = document.getElementById('convertSizeSummary');
  if (summary) {
    summary.textContent = `📏 Max ${profile.width}×${profile.height}px`;
  }
  document.querySelectorAll('.device-btn').forEach(btn => {
    btn.classList.toggle('active', btn.dataset.value === DEVICE_TARGET);
  });
  const autoLabel = document.getElementById('deviceAutoLabel');
  if (autoLabel) {
    autoLabel.textContent = DETECTED_DEVICE ? `Auto (${DETECTED_DEVICE})` : 'Auto';
  }

  // Recompute picker classification with new dimensions, then refresh grid.
  if (Array.isArray(epubImagesCache) && epubImagesCache.length > 0) {
    for (const img of epubImagesCache) {
      img.fitsScreen = (img.width <= MAX_WIDTH && img.height <= MAX_HEIGHT);
      img.canHSplit = img.width >= MAX_HEIGHT;
      img.canVSplit = img.height >= MAX_HEIGHT;
    }
    const pickerSection = document.getElementById('imagePickerSection');
    if (pickerSection && pickerSection.style.display !== 'none' && typeof renderImageGrid === 'function') {
      renderImageGrid();
    }
  }
}

function setDeviceTarget(value) {
  DEVICE_TARGET = value;
  applyDeviceTarget();
}

// Batch logging system for multiple files
let batchLogEntries = [];
let batchStats = { filesProcessed: 0, filesSucceeded: 0, filesFailed: 0, totalImages: 0, totalSplits: 0, totalFixes: 0, totalErrors: 0, totalOriginalSize: 0, totalNewSize: 0 };
let batchStartTime = null;
let isBatchMode = false;

// Format bytes to human-readable size (for logging)
function formatBytes(b) {
  if (!b) return '0 B';
  const k = 1024;
  const s = ['B', 'KB', 'MB', 'GB'];
  const i = Math.floor(Math.log(b) / Math.log(k));
  return (b / Math.pow(k, i)).toFixed(1) + ' ' + s[i];
}

// Get elapsed timestamp since log start
function getTimestamp() {
  if (!logStartTime) return '[00:00.0]';
  const elapsed = (Date.now() - logStartTime) / 1000;
  const mins = Math.floor(elapsed / 60).toString().padStart(2, '0');
  const secs = (elapsed % 60).toFixed(1).padStart(4, '0');
  return `[${mins}:${secs}]`;
}

// Main logging function
function log(message, type = '', tag = '') {
  const entry = document.createElement('div');
  entry.className = 'log-entry ' + type;

  // Timestamp
  const timestamp = document.createElement('span');
  timestamp.className = 'log-timestamp';
  timestamp.textContent = getTimestamp();
  entry.appendChild(timestamp);

  // Tag (if provided)
  if (tag) {
    const tagEl = document.createElement('span');
    tagEl.className = 'log-tag ' + tag.toLowerCase();
    tagEl.textContent = tag;
    entry.appendChild(tagEl);
  }

  // Message
  const msg = document.createElement('span');
  msg.className = 'log-message';
  msg.innerHTML = message;
  entry.appendChild(msg);

  logContainer.appendChild(entry);
  logContainer.scrollTop = logContainer.scrollHeight;
}

// Log image processing details
function logImage(name, origW, origH, origFormat, origSize, newW, newH, newSize, wasSplit = false, splitCount = 0, partsInfo = null, imageState = 0) {
  const saved = origSize - newSize;
  const savedPct = ((saved / origSize) * 100).toFixed(0);
  const dims = `${origW}×${origH}`;
  const newDims = `${newW}×${newH}`;

  // Get state label and color
  const stateLabels = ['', 'H-Split', 'V-Split', 'Rotate'];
  const stateColors = ['', '#3498db', '#e74c3c', '#9b59b6'];
  const stateLabel = stateLabels[imageState] || '';
  const stateColor = stateColors[imageState] || '';

  if (wasSplit) {
    conversionStats.splits++;
    conversionStats.splitParts += splitCount;
    // Build parts detail string
    let partsDetail = '';
    if (partsInfo && partsInfo.length > 0) {
      const baseName = name.replace(/\.[^.]+$/, '');
      partsDetail = partsInfo.map(p =>
        `${baseName}${p.suffix}.jpg (${p.width}×${p.height}, ${formatBytes(p.size)})`
      ).join(', ');
    }
    const savedInfo = saved > 0 ? `, <span style="color:#27ae60">-${savedPct}%</span>` : '';
    const stateIndicator = imageState > 0 ? ` <span style="color:${stateColor};font-weight:600;">[${stateLabel}]</span>` : '';
    log(`<strong>${escapeHtml(name)}</strong>${stateIndicator} <span class="log-detail">(${dims} ${origFormat.toUpperCase()}, ${formatBytes(origSize)}) → ${splitCount} parts${savedInfo}</span>`, '', 'SPLIT');
    if (partsDetail) {
      log(`<span class="log-detail" style="margin-left: 20px;">↳ ${partsDetail}</span>`, '', '');
    }
  } else {
    conversionStats.images++;
    const stateIndicator = imageState > 0 ? ` <span style="color:${stateColor};font-weight:600;">[${stateLabel}]</span>` : '';
    const detail = saved > 0
      ? `<span class="log-detail">(${dims} → ${newDims}, ${formatBytes(origSize)} → ${formatBytes(newSize)}, <span style="color:#27ae60">-${savedPct}%</span>)</span>`
      : `<span class="log-detail">(${dims} → ${newDims}, ${formatBytes(newSize)})</span>`;
    log(`<strong>${escapeHtml(name)}</strong>${stateIndicator} ${detail}`, '', 'CONVERT');
  }
}

// Log fix applied
function logFix(type, detail) {
  conversionStats.fixes++;
  log(`${type}: <span class="log-detail">${detail}</span>`, 'success', 'FIX');
}

// Log skipped item
function logSkip(name, reason) {
  conversionStats.skipped++;
  log(`${escapeHtml(name)} <span class="log-detail">(${reason})</span>`, '', 'SKIP');
}

// Log error
function logError(message) {
  conversionStats.errors++;
  log(message, 'error', 'ERROR');
}

// Log summary table
function logSummary(originalSize, newSize, timeElapsed) {
  const saved = originalSize - newSize;
  const savedPct = ((saved / originalSize) * 100).toFixed(1);
  const totalImages = conversionStats.images + conversionStats.splits;
  const totalOutput = conversionStats.images + conversionStats.splitParts;

  const summaryHtml = `
    <div class="log-summary">
      <div class="log-summary-title">📊 Conversion Summary</div>
      <table class="log-summary-table">
        <tr><td>Images found</td><td class="highlight">${totalImages}</td></tr>
        <tr><td>Images processed</td><td>${totalOutput}${conversionStats.splitParts > conversionStats.splits ? ` (+${conversionStats.splitParts - conversionStats.splits} from splits)` : ''}</td></tr>
        <tr><td>EPUB repairs</td><td>${conversionStats.fixes > 0 ? conversionStats.fixes + ' fixes applied' : 'None needed'}</td></tr>
        ${conversionStats.errors > 0 ? `<tr><td>Errors</td><td style="color:#e74c3c">${conversionStats.errors}</td></tr>` : ''}
        <tr><td>Original size</td><td>${formatBytes(originalSize)}</td></tr>
        <tr><td>Optimized size</td><td>${formatBytes(newSize)}</td></tr>
        <tr><td>Saved</td><td class="${saved > 0 ? 'saved' : 'increased'}">${saved > 0 ? formatBytes(saved) + ' (' + savedPct + '%)' : '+' + formatBytes(-saved)}</td></tr>
        <tr><td>Time</td><td>${timeElapsed.toFixed(1)}s</td></tr>
      </table>
    </div>
  `;
  logContainer.insertAdjacentHTML('beforeend', summaryHtml);
  logContainer.scrollTop = logContainer.scrollHeight;
}

// Clear log
function clearLog() {
  logContainer.innerHTML = '';
  logStartTime = Date.now();
  conversionStats = { images: 0, splits: 0, splitParts: 0, fixes: 0, skipped: 0, errors: 0, originalSize: 0, newSize: 0 };
}

// Start batch logging mode
function startBatchLog(fileCount) {
  isBatchMode = true;
  batchStartTime = Date.now();
  batchLogEntries = [];
  batchStats = { filesProcessed: 0, filesSucceeded: 0, filesFailed: 0, totalImages: 0, totalSplits: 0, totalFixes: 0, totalErrors: 0, totalOriginalSize: 0, totalNewSize: 0 };
  clearLog();
  logContainer.innerHTML = ''; // Clear display
  log(`Starting batch conversion: ${fileCount} file(s)`, '', 'INFO');
}

// Save current file's log to batch entries
function saveToFileBatchLog(fileName, succeeded, originalSize = 0, newSize = 0) {
  if (!isBatchMode) return;

  const entries = Array.from(logContainer.querySelectorAll('.log-entry'));
  batchLogEntries.push({
    fileName: fileName,
    succeeded: succeeded,
    entries: entries,
    stats: { ...conversionStats }
  });

  // Update batch stats
  batchStats.filesProcessed++;
  if (succeeded) {
    batchStats.filesSucceeded++;
  } else {
    batchStats.filesFailed++;
  }
  batchStats.totalImages += conversionStats.images;
  batchStats.totalSplits += conversionStats.splits;
  batchStats.totalFixes += conversionStats.fixes;
  batchStats.totalErrors += conversionStats.errors;
  // Defaults of 0 keep failure-path callers safe — files that never
  // produced a converted blob contribute nothing to the totals.
  batchStats.totalOriginalSize += originalSize;
  batchStats.totalNewSize += newSize;

  // Clear for next file
  logContainer.innerHTML = '';
  conversionStats = { images: 0, splits: 0, splitParts: 0, fixes: 0, skipped: 0, errors: 0, originalSize: 0, newSize: 0 };
}

// Finalize batch log and export
function finalizeBatchLog() {
  if (!isBatchMode) return;

  const batchTime = (Date.now() - batchStartTime) / 1000;

  // Build consolidated log display
  logContainer.innerHTML = '';
  log(`Starting batch conversion: ${batchStats.filesProcessed} file(s)`, '', 'INFO');

  // Add all file entries
  batchLogEntries.forEach((fileLog, index) => {
    const fileHeader = document.createElement('div');
    fileHeader.className = 'log-entry';
    fileHeader.style.marginTop = index > 0 ? '15px' : '5px';
    fileHeader.style.borderTop = index > 0 ? '1px solid var(--border-color)' : 'none';
    fileHeader.style.paddingTop = index > 0 ? '10px' : '0';
    fileHeader.innerHTML = `<span class="log-timestamp"></span><span class="log-message"><strong>${escapeHtml(fileLog.fileName)}</strong> — ${fileLog.succeeded ? '<span style="color:#27ae60">✓ Success</span>' : '<span style="color:#e74c3c">✗ Failed</span>'}</span>`;
    logContainer.appendChild(fileHeader);

    fileLog.entries.forEach(entry => {
      const clone = entry.cloneNode(true);
      logContainer.appendChild(clone);
    });
  });

  // Aggregate size totals: only emit rows when at least one file was successfully
  // converted (totalOriginalSize stays 0 for batches where conversion was off or
  // every file fell back to original upload).
  const totalSaved = batchStats.totalOriginalSize - batchStats.totalNewSize;
  const totalSavedPct = batchStats.totalOriginalSize > 0
    ? ((totalSaved / batchStats.totalOriginalSize) * 100).toFixed(1)
    : '0.0';
  const sizeRowsHtml = batchStats.totalOriginalSize > 0 ? `
        <tr><td>Total original</td><td>${formatBytes(batchStats.totalOriginalSize)}</td></tr>
        <tr><td>Total optimised</td><td>${formatBytes(batchStats.totalNewSize)}</td></tr>
        <tr><td>Total saved</td><td class="${totalSaved > 0 ? 'saved' : 'increased'}">${
          totalSaved > 0
            ? `${formatBytes(totalSaved)} (${totalSavedPct}%)`
            : `+${formatBytes(-totalSaved)}`
        }</td></tr>` : '';

  // Add batch summary
  const batchSummaryHtml = `
    <div class="log-summary">
      <div class="log-summary-title">📊 Batch Conversion Summary</div>
      <table class="log-summary-table">
        <tr><td>Files processed</td><td class="highlight">${batchStats.filesProcessed}</td></tr>
        <tr><td>Successful</td><td style="color:#27ae60">${batchStats.filesSucceeded}</td></tr>
        <tr><td>Failed</td><td style="${batchStats.filesFailed > 0 ? '#e74c3c' : '#7f8c8d'}">${batchStats.filesFailed}</td></tr>
        <tr><td>Total images processed</td><td>${batchStats.totalImages}</td></tr>
        <tr><td>Total splits</td><td>${batchStats.totalSplits}</td></tr>
        <tr><td>Total fixes applied</td><td>${batchStats.totalFixes}</td></tr>
        ${batchStats.totalErrors > 0 ? `<tr><td>Total errors</td><td style="color:#e74c3c">${batchStats.totalErrors}</td></tr>` : ''}${sizeRowsHtml}
        <tr><td>Total time</td><td>${batchTime.toFixed(1)}s</td></tr>
      </table>
    </div>
  `;
  logContainer.insertAdjacentHTML('beforeend', batchSummaryHtml);
  logContainer.scrollTop = logContainer.scrollHeight;

  // Auto-export if checkbox is checked
  if (exportLogCheckbox && exportLogCheckbox.checked) {
    setTimeout(() => {
      exportLogToFile(null, true); // isBatch = true
    }, 200);
  }

  // Reset batch mode
  isBatchMode = false;
  batchLogEntries = [];
}

// Show/hide log section
function showLog() {
  logSection.classList.add('visible');
}

function hideLog() {
  logSection.classList.remove('visible');
}

// Generate standardized log filename with date
function generateLogFilename(isBatch = false) {
  const now = new Date();
  const date = now.toISOString().split('T')[0]; // YYYY-MM-DD
  const time = now.toTimeString().split(' ')[0].replace(/:/g, '-'); // HH-MM-SS
  const prefix = isBatch ? 'batch' : 'epub';
  return `${prefix}-conversion-log-${date}_${time}.txt`;
}

// Export log as text file (can be called automatically)
function exportLogToFile(filename = null, isBatch = false) {
  // Use standardized filename if none provided
  if (!filename) {
    filename = generateLogFilename(isBatch);
  }
  // Extract text from log entries
  const entries = logContainer.querySelectorAll('.log-entry');
  let logText = `inkMOD Reader ${inkmodVersion} - EPUB Conversion Log\n`;
  logText += `Generated: ${new Date().toLocaleString()}\n`;
  logText += `${'='.repeat(60)}\n\n`;

  entries.forEach(entry => {
    const timestamp = entry.querySelector('.log-timestamp')?.textContent || '';
    const tag = entry.querySelector('.log-tag')?.textContent || '';
    const message = entry.querySelector('.log-message')?.textContent || entry.textContent;

    if (tag) {
      logText += `${timestamp} [${tag}] ${message}\n`;
    } else {
      logText += `${timestamp} ${message}\n`;
    }
  });

  // Extract summary table if present
  const summary = logContainer.querySelector('.log-summary');
  if (summary) {
    logText += `\n${'='.repeat(60)}\n`;
    const title = summary.querySelector('.log-summary-title')?.textContent || 'Summary';
    logText += `${title}\n`;
    logText += `${'-'.repeat(40)}\n`;

    const rows = summary.querySelectorAll('tr');
    rows.forEach(row => {
      const cells = row.querySelectorAll('td');
      if (cells.length >= 2) {
        logText += `${cells[0].textContent.padEnd(25)} ${cells[1].textContent}\n`;
      }
    });
  }

  // Create download link
  const blob = new Blob([logText], { type: 'text/plain' });
  const url = URL.createObjectURL(blob);
  const a = document.createElement('a');
  a.href = url;
  a.download = filename || `epub-conversion-log-${Date.now()}.txt`;
  document.body.appendChild(a);
  a.click();
  document.body.removeChild(a);
  URL.revokeObjectURL(url);
}

// ═══════════════════════════════════════════════════════════════════
// EPUB Utilities — ported from EPUB Optimizer Pro
// ═══════════════════════════════════════════════════════════════════

/** Defensive CSS injected into every XHTML <head> — prevents e-ink overflow. */
const DEFENSIVE_STYLE = '<style type="text/css">img,svg{max-width:100%;height:auto}body{overflow-wrap:break-word}table{max-width:100%;table-layout:fixed}pre,code{white-space:pre-wrap;word-wrap:break-word}*{box-sizing:border-box}</style>';

/** Escape a string for safe insertion into XML attribute values / text content. */
function xmlEscape(str) {
  return str.replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;').replace(/"/g, '&quot;');
}

function escapeRegex(str) { return str.replace(/[.*+?^${}()|[\]\\]/g, '\\$&'); }

/**
 * Decode a URI-encoded href (e.g., "my%20image.jpg" → "my image.jpg").
 * Handles double-encoding gracefully.
 */
function decodeHref(href) {
  try { return decodeURIComponent(href); }
  catch (e) { return href; }
}

/**
 * Safely read a text file from the zip, handling BOM and encoding.
 * Strips UTF-8 BOM. Detects encoding from XML declaration or meta tag.
 */
async function safeReadText(fileObj) {
  const raw = await fileObj.async('uint8array');

  // Detect and strip UTF-8 BOM (EF BB BF)
  let offset = 0;
  if (raw.length >= 3 && raw[0] === 0xEF && raw[1] === 0xBB && raw[2] === 0xBF) {
    offset = 3;
  }

  // Try UTF-8 first (vast majority of EPUBs)
  const utf8 = new TextDecoder('utf-8', { fatal: true });
  try {
    return utf8.decode(raw.subarray(offset));
  } catch (e) { /* not valid UTF-8 */ }

  // Peek at XML declaration or meta charset for encoding hint
  const ascii = new TextDecoder('ascii', { fatal: false }).decode(raw.subarray(offset, offset + 512));
  const encodingMatch = ascii.match(/encoding=["']([^"']+)["']/i) ||
                        ascii.match(/charset=["']?([^"'\s;]+)/i);
  const encoding = encodingMatch ? encodingMatch[1].toLowerCase() : 'windows-1252';

  try {
    return new TextDecoder(encoding, { fatal: false }).decode(raw.subarray(offset));
  } catch (e) {
    // Last resort: lossy latin1
    return new TextDecoder('iso-8859-1', { fatal: false }).decode(raw.subarray(offset));
  }
}

/**
 * Find the canonical OPF path by parsing META-INF/container.xml.
 * Falls back to scanning for any .opf file.
 */
async function findOPFPath(zip) {
  try {
    const containerPath = Object.keys(zip.files).find(p => p.toLowerCase() === 'meta-inf/container.xml');
    if (containerPath) {
      const containerXml = await zip.files[containerPath].async('string');
      const match = containerXml.match(/<rootfile[^>]+full-path=["']([^"']+)["']/i);
      if (match && zip.files[match[1]]) return match[1];
    }
  } catch (e) { /* fall through */ }
  let fallback = null;
  zip.forEach(p => { if (!fallback && p.toLowerCase().endsWith('.opf')) fallback = p; });
  return fallback;
}

/**
 * Resolve a relative href against a base file path.
 * Handles multiple ../, ./, absolute /, and bare relative paths.
 */
function resolvePath(basePath, href) {
  if (href.startsWith('/')) return href.substring(1);
  href = href.replace(/^\.\//, '');
  const baseDir = basePath.includes('/') ? basePath.substring(0, basePath.lastIndexOf('/')) : '';
  const baseParts = baseDir ? baseDir.split('/') : [];
  const hrefParts = href.split('/');
  while (hrefParts.length > 0 && hrefParts[0] === '..') {
    hrefParts.shift();
    if (baseParts.length > 0) baseParts.pop();
  }
  const resolved = [...baseParts, ...hrefParts].join('/');
  return resolved.replace(/\/+/g, '/');
}

/**
 * Serialize an XML doc back to string, preserving the original <?xml?> declaration
 * and cleaning up XMLSerializer namespace prefix noise (xmlns:ns0 etc).
 */
function safeSerialize(doc, originalContent) {
  let result = new XMLSerializer().serializeToString(doc);

  // Restore <?xml?> declaration if original had one
  if (originalContent && /^\s*<\?xml\b/.test(originalContent) && !/^\s*<\?xml\b/.test(result)) {
    const declMatch = originalContent.match(/^\s*(<\?xml[^?]*\?>)/);
    if (declMatch) result = declMatch[1] + '\n' + result;
  }

  // Clean up XMLSerializer namespace prefix noise (xmlns:ns0="..." ns0:attr="...")
  result = result.replace(/ xmlns:ns\d+="[^"]*"/g, '');
  result = result.replace(/ ns\d+:/g, ' ');

  return result;
}

function protectWhitespaceOnlyTextNodes(content) {
  const preserved = [];
  const tokenPrefix = '__INKMOD_PRESERVE_WS_';
  const protectedContent = content.replace(/>([\s\u00a0]+)</g, (_, whitespace) => {
    const token = `${tokenPrefix}${preserved.length}__`;
    preserved.push(whitespace);
    return `>${token}<`;
  });

  return {
    content: protectedContent,
    restore(serialized) {
      return serialized.replace(new RegExp(`${escapeRegex(tokenPrefix)}(\\d+)__`, 'g'), (match, indexText) => {
        const index = Number(indexText);
        return Number.isInteger(index) && index >= 0 && index < preserved.length ? preserved[index] : match;
      });
    }
  };
}

/**
 * Extract main identifier from OPF for NCX sync. DOMParser with regex fallback.
 */
function extractIdentifier(opfContent) {
  let mainIdentifier = null;
  try {
    const doc = new DOMParser().parseFromString(opfContent, 'application/xml');
    if (!doc.querySelector('parsererror')) {
      const pkg = doc.getElementsByTagNameNS('*', 'package')[0];
      const uid = pkg ? pkg.getAttribute('unique-identifier') : null;
      if (uid) {
        const el = [...doc.getElementsByTagNameNS('*', 'identifier')].find(e => e.getAttribute('id') === uid);
        if (el) mainIdentifier = (el.textContent || '').trim();
      }
      if (!mainIdentifier) {
        const el = doc.getElementsByTagNameNS('*', 'identifier')[0];
        if (el) mainIdentifier = (el.textContent || '').trim();
      }
    }
  } catch (e) { /* fall through to regex */ }
  if (!mainIdentifier) {
    const uniqueIdMatch = opfContent.match(/<(?:\w+:)?package[^>]*unique-identifier=["']([^"']+)["']/i);
    if (uniqueIdMatch) {
      const idRegex = new RegExp(`<dc:identifier[^>]*id=["']${uniqueIdMatch[1]}["'][^>]*>([^<]+)</dc:identifier>`, 'i');
      const idMatch = opfContent.match(idRegex);
      if (idMatch) mainIdentifier = idMatch[1].trim();
    }
    if (!mainIdentifier) {
      const firstIdMatch = opfContent.match(/<dc:identifier[^>]*>([^<]+)</i);
      if (firstIdMatch) mainIdentifier = firstIdMatch[1].trim();
    }
  }
  return mainIdentifier;
}

/**
 * Sync NCX dtb:uid with the given identifier. DOMParser with regex fallback.
 */
function syncNCXIdentifier(ncxText, mainIdentifier) {
  if (!mainIdentifier) return ncxText;
  let t = ncxText;
  try {
    const doc = new DOMParser().parseFromString(t, 'application/xml');
    if (!doc.querySelector('parsererror')) {
      const meta = [...doc.getElementsByTagNameNS('*', 'meta')].find(m => m.getAttribute('name') === 'dtb:uid');
      if (meta) {
        meta.setAttribute('content', mainIdentifier);
        t = safeSerialize(doc, ncxText);
      }
    }
  } catch (e) {
    t = t.replace(/<meta\s+name=["']dtb:uid["']\s+content=["'][^"']*["']\s*\/?>/gi, `<meta name="dtb:uid" content="${xmlEscape(mainIdentifier)}"/>`);
  }
  return t;
}

/**
 * Fix OPF content: fix media-types, strip svg properties,
 * update split image manifest entries, ensure cover meta.
 * DOMParser with regex fallback.
 */
function fixOPF(opfText, opfOriginal, opfDir, splitImages = {}) {
  let t = opfText;

  try {
    const parser = new DOMParser();
    const doc = parser.parseFromString(t, 'application/xml');
    if (doc.querySelector('parsererror')) throw new Error('OPF parse failed');

    const items = [...doc.getElementsByTagNameNS('*', 'item')];
    const manifestEl = doc.getElementsByTagNameNS('*', 'manifest')[0];

    // Fix media-types for converted images
    for (const item of items) {
      const href = item.getAttribute('href') || '';
      const type = item.getAttribute('media-type') || '';
      if (href.endsWith('.jpg') && type.match(/^image\/(png|gif|webp|bmp)$/)) {
        item.setAttribute('media-type', 'image/jpeg');
      }
    }

    // Remove 'svg' from properties
    for (const item of items) {
      const props = item.getAttribute('properties') || '';
      if (props.includes('svg')) {
        const newProps = props.split(/\s+/).filter(p => p !== 'svg').join(' ').trim();
        if (newProps) item.setAttribute('properties', newProps);
        else item.removeAttribute('properties');
      }
    }

    // Update split image hrefs and add manifest entries for parts
    for (const [splitKey, splitInfo] of Object.entries(splitImages)) {
      const parts = splitInfo.parts || splitInfo;
      let origHref = opfDir && splitKey.startsWith(opfDir + '/') ? splitKey.substring(opfDir.length + 1) : splitKey;
      const origHrefJpg = origHref.replace(/\.(png|gif|webp|bmp|jpeg)$/i, '.jpg');
      const part1Href = origHrefJpg.replace(/\.jpg$/i, '_part1.jpg');

      for (const item of items) {
        const h = item.getAttribute('href') || '';
        if (h === origHref || h === origHrefJpg || decodeHref(h) === origHref || decodeHref(h) === origHrefJpg) {
          item.setAttribute('href', part1Href);
          break;
        }
      }

      if (manifestEl) {
        const ns = manifestEl.namespaceURI || 'http://www.idpf.org/2007/opf';
        for (let j = 1; j < parts.length; j++) {
          const p = parts[j];
          const href = opfDir && p.path.startsWith(opfDir + '/') ? p.path.substring(opfDir.length + 1) : p.path;
          const newItem = doc.createElementNS(ns, 'item');
          newItem.setAttribute('id', `img-${p.id}`);
          newItem.setAttribute('href', href);
          newItem.setAttribute('media-type', 'image/jpeg');
          manifestEl.appendChild(newItem);
        }
      }
    }

    t = safeSerialize(doc, opfOriginal);
  } catch (e) {
    // Regex fallback
    t = t.replace(/(<(?:\w+:)?item\b[^>]*href="[^"]+\.jpg"[^>]*)media-type="image\/(png|gif|webp|bmp)"/g, '$1media-type="image/jpeg"');
    t = t.replace(/(<(?:\w+:)?item\b[^>]*)media-type="image\/(png|gif|webp|bmp)"([^>]*href="[^"]+\.jpg")/g, '$1media-type="image/jpeg"$3');
    t = t.replace(/\s+svg(?=["'\s>])/g, '');
    for (const [splitKey, splitInfo] of Object.entries(splitImages)) {
      const parts = splitInfo.parts || splitInfo;
      let origHref = opfDir && splitKey.startsWith(opfDir + '/') ? splitKey.substring(opfDir.length + 1) : splitKey;
      const origHrefJpg = origHref.replace(/\.(png|gif|webp|bmp|jpeg)$/i, '.jpg');
      const part1Href = origHrefJpg.replace(/\.jpg$/i, '_part1.jpg');
      const origImgRegex = new RegExp(`(href=["'])(${escapeRegex(origHref)}|${escapeRegex(origHrefJpg)})(["'])`, 'gi');
      t = t.replace(origImgRegex, `$1${part1Href}$3`);
      let adds = '';
      for (let j = 1; j < parts.length; j++) {
        const p = parts[j];
        const href = opfDir && p.path.startsWith(opfDir + '/') ? p.path.substring(opfDir.length + 1) : p.path;
        adds += `<item id="img-${xmlEscape(p.id)}" href="${xmlEscape(href)}" media-type="image/jpeg"/>\n`;
      }
      if (adds && t.includes('</manifest>')) t = t.replace('</manifest>', adds + '</manifest>');
    }
  }

  // Ensure cover meta
  const cm = ensureCoverMeta(t);
  if (cm.fixed) t = cm.o;

  return t;
}

// Fix SVG cover - converts SVG-wrapped covers to plain HTML img tags
function fixSvgCover(content) {
  const hasSvg = content.includes('<svg') || content.includes('<svg:');
  if (!hasSvg || !content.includes('xlink:href')) return { c: content, fixed: false, count: 0 };
  if (!content.includes('calibre:cover') && !content.includes('name="cover"') && !content.includes('<title>Cover</title>')) return { c: content, fixed: false, count: 0 };

  try {
    const parser = new DOMParser();
    const doc = parser.parseFromString(content, 'application/xhtml+xml');

    if (doc.querySelector('parsererror')) {
      // Fallback to regex
      const m = content.match(/xlink:href=["']([^"']+)["']/);
      if (!m) return { c: content, fixed: false, count: 0 };
      return { c: `<?xml version="1.0" encoding="utf-8"?>
<!DOCTYPE html>
<html xmlns="http://www.w3.org/1999/xhtml" xmlns:epub="http://www.idpf.org/2007/ops" lang="en" xml:lang="en">
<head><meta content="text/html; charset=UTF-8" http-equiv="default-style"/><title>Cover</title></head>
<body><section epub:type="cover"><img style="max-width:100%;height:auto" alt="Cover" src="${m[1]}"/></section></body>
</html>`, fixed: true, count: 1 };
    }

    // Find SVG elements - check both standard and namespaced variants
    let imgHref = null;
    const svgNS = 'http://www.w3.org/2000/svg';
    const xlinkNS = 'http://www.w3.org/1999/xlink';

    // Try to find all SVG elements
    const svgs = [
      ...doc.getElementsByTagName('svg'),
      ...doc.getElementsByTagNameNS(svgNS, 'svg'),
      ...doc.getElementsByTagName('svg:svg')
    ];

    for (const svg of svgs) {
      // Find image element inside - try all variants
      const imageEl = svg.getElementsByTagName('image')[0] ||
                      svg.getElementsByTagNameNS(svgNS, 'image')[0] ||
                      svg.getElementsByTagName('svg:image')[0];

      if (imageEl) {
        imgHref = imageEl.getAttributeNS(xlinkNS, 'href') ||
                  imageEl.getAttribute('xlink:href') ||
                  imageEl.getAttribute('href');
        if (imgHref) break;
      }
    }

    if (!imgHref) {
      // Fallback to regex
      const m = content.match(/xlink:href=["']([^"']+)["']/);
      if (!m) return { c: content, fixed: false, count: 0 };
      imgHref = m[1];
    }

    return {
      c: `<?xml version="1.0" encoding="utf-8"?>
<!DOCTYPE html>
<html xmlns="http://www.w3.org/1999/xhtml" xmlns:epub="http://www.idpf.org/2007/ops" lang="en" xml:lang="en">
<head><meta content="text/html; charset=UTF-8" http-equiv="default-style"/><title>Cover</title></head>
<body><section epub:type="cover"><img style="max-width:100%;height:auto" alt="Cover" src="${imgHref}"/></section></body>
</html>`,
      fixed: true,
      count: 1
    };
  } catch (e) {
    // Fallback to regex
    const m = content.match(/xlink:href=["']([^"']+)["']/);
    if (!m) return { c: content, fixed: false, count: 0 };
    return { c: `<?xml version="1.0" encoding="utf-8"?>
<!DOCTYPE html>
<html xmlns="http://www.w3.org/1999/xhtml" xmlns:epub="http://www.idpf.org/2007/ops" lang="en" xml:lang="en">
<head><meta content="text/html; charset=UTF-8" http-equiv="default-style"/><title>Cover</title></head>
<body><section epub:type="cover"><img style="max-width:100%;height:auto" alt="Cover" src="${m[1]}"/></section></body>
</html>`, fixed: true, count: 1 };
  }
}

// Fix SVG-wrapped images - unwrap SVG and replace with plain img
function fixSvgWrappedImages(content) {
  const hasSvg = content.includes('<svg') || content.includes('<svg:');
  if (!hasSvg || !content.includes('xlink:href')) return { c: content, fixed: false, count: 0 };

  try {
    const whitespaceGuard = protectWhitespaceOnlyTextNodes(content);
    const parser = new DOMParser();
    const doc = parser.parseFromString(whitespaceGuard.content, 'application/xhtml+xml');

    if (doc.querySelector('parsererror')) {
      // Fallback to regex
      let fixedCount = 0;
      const svgImageRegex = /<(?:svg:)?svg\b[^>]*>[\s\S]*?<(?:svg:)?image\b[^>]*xlink:href=["']([^"']+)["'][^>]*\/?>\s*<\/(?:svg:)?svg>/gi;
      const newContent = content.replace(svgImageRegex, (match, href) => { fixedCount++; return `<img style="max-width:100%;height:auto" src="${href}" alt="" />`; });
      return { c: newContent, fixed: fixedCount > 0, count: fixedCount };
    }

    const svgNS = 'http://www.w3.org/2000/svg';
    const xlinkNS = 'http://www.w3.org/1999/xlink';

    const svgElements = [...doc.querySelectorAll('svg'), ...doc.getElementsByTagNameNS(svgNS, 'svg')];
    const uniqueSvgs = [...new Set(svgElements)];
    let fixedCount = 0;

    for (const svg of uniqueSvgs) {
      const imageEl = svg.querySelector('image[*|href]') || svg.getElementsByTagNameNS(svgNS, 'image')[0] || svg.getElementsByTagNameNS('*', 'image')[0];
      if (!imageEl) continue;
      const href = imageEl.getAttributeNS(xlinkNS, 'href') || imageEl.getAttribute('xlink:href') || imageEl.getAttribute('href');
      if (!href) continue;
      const width = imageEl.getAttribute('width') || svg.getAttribute('width');
      const height = imageEl.getAttribute('height') || svg.getAttribute('height');
      const img = doc.createElementNS('http://www.w3.org/1999/xhtml', 'img');
      img.setAttribute('src', href);
      img.setAttribute('alt', '');
      img.setAttribute('style', 'max-width:100%;height:auto');
      if (width) img.setAttribute('width', width);
      if (height) img.setAttribute('height', height);
      svg.parentNode.replaceChild(img, svg);
      fixedCount++;
    }

    if (fixedCount === 0) return { c: content, fixed: false, count: 0 };
    return { c: whitespaceGuard.restore(safeSerialize(doc, whitespaceGuard.content)), fixed: true, count: fixedCount };

  } catch (e) {
    // Fallback to regex
    let fixedCount = 0;
    const svgImageRegex = /<(?:svg:)?svg\b[^>]*>[\s\S]*?<(?:svg:)?image\b[^>]*xlink:href=["']([^"']+)["'][^>]*\/?>\s*<\/(?:svg:)?svg>/gi;
    const newContent = content.replace(svgImageRegex, (match, href) => { fixedCount++; return `<img style="max-width:100%;height:auto" src="${href}" alt="" />`; });
    return { c: newContent, fixed: fixedCount > 0, count: fixedCount };
  }
}

// Ensure cover meta tag exists in OPF — DOMParser with regex fallback
function ensureCoverMeta(opfString) {
  try {
    const parser = new DOMParser();
    const doc = parser.parseFromString(opfString, 'application/xml');
    if (doc.querySelector('parsererror')) throw new Error('Parse failed');

    // Find cover image id: properties="cover-image", or id/href containing "cover"
    let coverId = null;
    const items = [...doc.getElementsByTagNameNS('*', 'item')];
    for (const item of items) {
      const props = item.getAttribute('properties') || '';
      const id = item.getAttribute('id') || '';
      const type = item.getAttribute('media-type') || '';
      if (!type.startsWith('image/')) continue;
      if (props.includes('cover-image')) { coverId = id; break; }
    }
    if (!coverId) {
      for (const item of items) {
        const id = item.getAttribute('id') || '';
        const href = item.getAttribute('href') || '';
        const type = item.getAttribute('media-type') || '';
        if (!type.startsWith('image/')) continue;
        if (id.toLowerCase().includes('cover') || href.toLowerCase().includes('cover')) { coverId = id; break; }
      }
    }
    if (!coverId) return { o: opfString, fixed: false };

    // Find or create <meta name="cover" content="..."/>
    const metas = [...doc.getElementsByTagNameNS('*', 'meta')];
    const coverMeta = metas.find(m => m.getAttribute('name') === 'cover');
    if (coverMeta) {
      if (coverMeta.getAttribute('content') === coverId) return { o: opfString, fixed: false };
      coverMeta.setAttribute('content', coverId);
    } else {
      const metadata = doc.getElementsByTagNameNS('*', 'metadata')[0];
      if (!metadata) return { o: opfString, fixed: false };
      const ns = metadata.namespaceURI || 'http://www.idpf.org/2007/opf';
      const newMeta = doc.createElementNS(ns, 'meta');
      newMeta.setAttribute('name', 'cover');
      newMeta.setAttribute('content', coverId);
      metadata.appendChild(newMeta);
    }
    return { o: safeSerialize(doc, opfString), fixed: true };
  } catch (e) {
    // Regex fallback
    return ensureCoverMetaRegex(opfString);
  }
}

function ensureCoverMetaRegex(o) {
  let coverId = null, m;
  if (!coverId && (m = o.match(/<\w+:?item[^>]+id="([^"]+)"[^>]+properties="[^"]*cover-image[^"]*"/i))) coverId = m[1];
  if (!coverId && (m = o.match(/<\w+:?item[^>]+properties="[^"]*cover-image[^"]*"[^>]+id="([^"]+)"/i))) coverId = m[1];
  if (!coverId && (m = o.match(/<\w+:?item[^>]*id="([^"]+)"[^>]*href="[^"]*cover[^"]*"[^>]*media-type="image\//i))) coverId = m[1];
  if (!coverId && (m = o.match(/<\w+:?item[^>]*href="[^"]*cover[^"]*"[^>]*id="([^"]+)"[^>]*media-type="image\//i))) coverId = m[1];
  if (!coverId && (m = o.match(/<\w+:?item[^>]*id="([^"]*cover[^"]*)"[^>]*media-type="image\//i))) coverId = m[1];
  if (!coverId && (m = o.match(/<\w+:?item[^>]*media-type="image\/[^"]*"[^>]*id="([^"]*cover[^"]*)"/i))) coverId = m[1];
  if (!coverId) return { o, fixed: false };
  const metaMatch = o.match(/<\w+:?meta\s+name=["']cover["']\s+content=["']([^"']+)["']/i) || o.match(/<\w+:?meta\s+content=["']([^"']+)["']\s+name=["']cover["']/i);
  if (metaMatch) {
    if (metaMatch[1] === coverId && !metaMatch[1].includes('/')) return { o, fixed: false };
    const esc = xmlEscape(coverId);
    o = o.replace(/<\w+:?meta\s+name=["']cover["']\s+content=["'][^"']+["']\s*\/?>/gi, `<meta name="cover" content="${esc}" />`);
    o = o.replace(/<\w+:?meta\s+content=["'][^"']+["']\s+name=["']cover["']\s*\/?>/gi, `<meta name="cover" content="${esc}" />`);
    return { o, fixed: true };
  }
  const idx = o.indexOf('</metadata>');
  if (idx !== -1) return { o: o.substring(0, idx) + `    <meta name="cover" content="${xmlEscape(coverId)}"/>\n  </metadata>` + o.substring(idx + 11), fixed: true };
  return { o, fixed: false };
}

// Apply grayscale to canvas image data
function applyGrayscale(ctx, width, height) {
  if (!ENABLE_GRAYSCALE) return;
  const imageData = ctx.getImageData(0, 0, width, height);
  const data = imageData.data;
  for (let i = 0; i < data.length; i += 4) {
    // Alpha-blend against white background before grayscaling (handles transparent PNGs)
    const a = data[i + 3] / 255;
    const blendedR = data[i] * a + 255 * (1 - a);
    const blendedG = data[i + 1] * a + 255 * (1 - a);
    const blendedB = data[i + 2] * a + 255 * (1 - a);
    const gray = Math.round(blendedR * 0.299 + blendedG * 0.587 + blendedB * 0.114);
    data[i] = gray; data[i + 1] = gray; data[i + 2] = gray; data[i + 3] = 255;
  }
  ctx.putImageData(imageData, 0, 0);
}

// Process single image - returns array of {data, suffix} objects
const IMAGE_LOAD_TIMEOUT_MS = 30000; // 30 second timeout for image loading
async function processImage(data, imageState = 0, imagePath = '') {
  return new Promise((resolve, reject) => {
    const url = URL.createObjectURL(new Blob([data]));
    const img = new Image();
    const origSize = data.byteLength;
    // Set up timeout to handle cases where image never loads
    const timeoutId = setTimeout(() => {
      URL.revokeObjectURL(url);
      reject(new Error('Image load timeout'));
    }, IMAGE_LOAD_TIMEOUT_MS);

    img.onload = async () => {
      clearTimeout(timeoutId);
      URL.revokeObjectURL(url);
      const origW = img.width, origH = img.height;

      // imageState: 0=Normal, 1=H-Split (CW/CCW), 2=V-Split, 3=Rotate & Fit
      // ========================================================================
      // STATE 1: H-Split (Rotate + Split) - EXACT COPY FROM index.html
      // Step 1: Scale WIDTH to 800px (keep aspect ratio)
      // Step 2: Rotate 90° CW or CCW based on HANDEDNESS
      // Step 3: If WIDTH > 480, split vertically with overlap
      // ========================================================================
      if (imageState === 1) {
        // Step 1: Scale WIDTH to 800 (this is the key difference!)
        const scale = MAX_HEIGHT / origW;  // 800 / origW
        const scaledW = MAX_HEIGHT;  // 800
        const scaledH = Math.round(origH * scale);

        const scaledCanvas = document.createElement('canvas');
        scaledCanvas.width = scaledW;
        scaledCanvas.height = scaledH;
        const scaledCtx = scaledCanvas.getContext('2d');
        scaledCtx.imageSmoothingEnabled = true;
        scaledCtx.imageSmoothingQuality = 'high';
        scaledCtx.fillStyle = '#FFF';
        scaledCtx.fillRect(0, 0, scaledW, scaledH);
        scaledCtx.drawImage(img, 0, 0, origW, origH, 0, 0, scaledW, scaledH);

        // Step 2: Rotate 90° CW or CCW
        const rotW = scaledH;
        const rotH = scaledW;  // 800

        const rotCanvas = document.createElement('canvas');
        rotCanvas.width = rotW;
        rotCanvas.height = rotH;
        const rotCtx = rotCanvas.getContext('2d');
        rotCtx.fillStyle = '#FFF';
        rotCtx.fillRect(0, 0, rotW, rotH);

        const isClockwise = HANDEDNESS === 'right';
        if (isClockwise) {
          // Rotate 90° CW
          rotCtx.translate(rotW, 0);
          rotCtx.rotate(Math.PI / 2);
        } else {
          // Rotate 90° CCW
          rotCtx.translate(0, rotH);
          rotCtx.rotate(-Math.PI / 2);
        }
        rotCtx.drawImage(scaledCanvas, 0, 0);
        rotCtx.setTransform(1, 0, 0, 1, 0, 0); // Reset transform
        applyGrayscale(rotCtx, rotW, rotH);

        // Step 3: If WIDTH > 480, split vertically
        if (rotW <= MAX_WIDTH) {
          const blob = await new Promise(res => rotCanvas.toBlob(res, 'image/jpeg', JPEG_QUALITY / 100));
          const arrBuf = await blob.arrayBuffer();
          resolve({
            parts: [{ data: arrBuf, suffix: '', width: rotW, height: rotH, size: arrBuf.byteLength }],
            meta: { origW, origH, origSize, wasSplit: false, rotated: true, finalW: rotW, finalH: rotH, finalSize: arrBuf.byteLength, imageState: 1 }
          });
        } else {
          // Split by WIDTH (vertical cuts) - from RIGHT to LEFT for CW, LEFT to RIGHT for CCW
          const parts = [];
          const maxW = MAX_WIDTH;  // 480

          // Centered distribution: calculate numParts first, then distribute evenly
          let overlapPx, step, numParts;
          const minOverlapPx = Math.round(maxW * (OVERLAP_PERCENT / 100));  // Configurable overlap
          const maxStep = maxW - minOverlapPx;
          numParts = Math.ceil((rotW - minOverlapPx) / maxStep);
          if (numParts < 2) numParts = 2;
          // Now calculate step to distribute evenly
          step = Math.round((rotW - maxW) / (numParts - 1));
          overlapPx = maxW - step;
          // Ensure minimum overlap
          if (overlapPx < minOverlapPx) {
            overlapPx = minOverlapPx;
            step = maxW - overlapPx;
          }

          // Calculate all x positions first to ensure consistency
          const positions = [];
          for (let i = 0; i < numParts; i++) {
            let x;
            if (isClockwise) {
              // CW: right to left - start from right edge
              x = rotW - maxW - (i * step);
            } else {
              // CCW: left to right - start from left edge
              x = i * step;
            }
            // Clamp to valid range
            x = Math.max(0, Math.min(x, rotW - maxW));
            positions.push(x);
          }

          // Ensure first and last positions are at edges
          if (isClockwise) {
            positions[0] = rotW - maxW; // First part at right edge
            positions[numParts - 1] = 0; // Last part at left edge
          } else {
            positions[0] = 0; // First part at left edge
            positions[numParts - 1] = rotW - maxW; // Last part at right edge
          }

          for (let i = 0; i < numParts; i++) {
            const x = positions[i];
            const partW = maxW; // Always full width for consistency

            const partCanvas = document.createElement('canvas');
            partCanvas.width = partW;
            partCanvas.height = rotH;
            const partCtx = partCanvas.getContext('2d');
            // Clear canvas first
            partCtx.fillStyle = '#FFFFFF';
            partCtx.fillRect(0, 0, partW, rotH);
            // Draw the slice
            partCtx.drawImage(rotCanvas, x, 0, partW, rotH, 0, 0, partW, rotH);

            const blob = await new Promise(res => partCanvas.toBlob(res, 'image/jpeg', JPEG_QUALITY / 100));
            const arrBuf = await blob.arrayBuffer();
            parts.push({ data: arrBuf, suffix: `_part${i + 1}`, width: partW, height: rotH, size: arrBuf.byteLength });
          }

          const totalSize = parts.reduce((sum, p) => sum + p.size, 0);
          resolve({
            parts,
            meta: { origW, origH, origSize, wasSplit: true, splitCount: numParts, rotated: true, finalW: parts[0].width, finalH: parts[0].height, finalSize: totalSize, imageState: 1 }
          });
        }
      }
      // ========================================================================
      // STATE 2: V-Split (Vertical Split, no rotation)
      // Step 1: Scale HEIGHT to 800px (up or down)
      // Step 2: If WIDTH > 480, split vertically with overlap
      // ========================================================================
      else if (imageState === 2) {
        // ALWAYS scale height to 800 (up or down)
        const scale = MAX_HEIGHT / origH;  // 800 / origH
        const scaledW = Math.round(origW * scale);
        const scaledH = MAX_HEIGHT;  // Always 800

        const scaledCanvas = document.createElement('canvas');
        scaledCanvas.width = scaledW;
        scaledCanvas.height = scaledH;
        const scaledCtx = scaledCanvas.getContext('2d');
        scaledCtx.imageSmoothingEnabled = true;
        scaledCtx.imageSmoothingQuality = 'high';
        scaledCtx.fillStyle = '#FFF';
        scaledCtx.fillRect(0, 0, scaledW, scaledH);
        scaledCtx.drawImage(img, 0, 0, origW, origH, 0, 0, scaledW, scaledH);
        applyGrayscale(scaledCtx, scaledW, scaledH);

        // Check if split needed
        if (scaledW <= MAX_WIDTH) {
          const blob = await new Promise(res => scaledCanvas.toBlob(res, 'image/jpeg', JPEG_QUALITY / 100));
          const arrBuf = await blob.arrayBuffer();
          resolve({
            parts: [{ data: arrBuf, suffix: '', width: scaledW, height: scaledH, size: arrBuf.byteLength }],
            meta: { origW, origH, origSize, wasSplit: false, rotated: false, finalW: scaledW, finalH: scaledH, finalSize: arrBuf.byteLength, imageState: 2 }
          });
        } else {
          // Split by WIDTH (vertical cuts) - LEFT to RIGHT (natural reading order)
          const parts = [];
          const maxW = MAX_WIDTH;

          // Centered distribution: calculate numParts first, then distribute evenly
          let overlapPx, step, numParts;
          const minOverlapPx = Math.round(maxW * (OVERLAP_PERCENT / 100));  // Configurable overlap
          const maxStep = maxW - minOverlapPx;
          numParts = Math.ceil((scaledW - minOverlapPx) / maxStep);
          if (numParts < 2) numParts = 2;
          // Now calculate step to distribute evenly
          step = Math.round((scaledW - maxW) / (numParts - 1));
          overlapPx = maxW - step;
          // Ensure minimum overlap
          if (overlapPx < minOverlapPx) {
            overlapPx = minOverlapPx;
            step = maxW - overlapPx;
          }

          // Calculate all x positions first to ensure consistency
          const positions = [];
          for (let i = 0; i < numParts; i++) {
            let x = i * step;
            // Clamp to valid range
            x = Math.max(0, Math.min(x, scaledW - maxW));
            positions.push(x);
          }
          // Ensure last position is at right edge
          positions[0] = 0;
          positions[numParts - 1] = scaledW - maxW;

          for (let i = 0; i < numParts; i++) {
            const x = positions[i];
            const partW = maxW; // Always full width for consistency

            const partCanvas = document.createElement('canvas');
            partCanvas.width = partW;
            partCanvas.height = scaledH;
            const partCtx = partCanvas.getContext('2d');
            // Clear canvas first
            partCtx.fillStyle = '#FFFFFF';
            partCtx.fillRect(0, 0, partW, scaledH);
            // Draw the slice
            partCtx.drawImage(scaledCanvas, x, 0, partW, scaledH, 0, 0, partW, scaledH);

            const blob = await new Promise(res => partCanvas.toBlob(res, 'image/jpeg', JPEG_QUALITY / 100));
            const arrBuf = await blob.arrayBuffer();
            parts.push({ data: arrBuf, suffix: `_part${i + 1}`, width: partW, height: scaledH, size: arrBuf.byteLength });
          }

          const totalSize = parts.reduce((sum, p) => sum + p.size, 0);
          resolve({
            parts,
            meta: { origW, origH, origSize, wasSplit: true, splitCount: numParts, rotated: false, finalW: parts[0].width, finalH: parts[0].height, finalSize: totalSize, imageState: 2 }
          });
        }
      }
      // ========================================================================
      // STATE 3: Rotate & Fit (Rotate 90°, then scale to fit 480x800, no split)
      // ========================================================================
      else if (imageState === 3) {
        // Step 1: Rotate 90° based on handedness
        const rotW = origH;
        const rotH = origW;

        const rotCanvas = document.createElement('canvas');
        rotCanvas.width = rotW;
        rotCanvas.height = rotH;
        const rotCtx = rotCanvas.getContext('2d');
        rotCtx.fillStyle = '#FFF';
        rotCtx.fillRect(0, 0, rotW, rotH);

        const isClockwise = HANDEDNESS === 'right';
        if (isClockwise) {
          rotCtx.translate(rotW, 0);
          rotCtx.rotate(Math.PI / 2);
        } else {
          rotCtx.translate(0, rotH);
          rotCtx.rotate(-Math.PI / 2);
        }
        rotCtx.drawImage(img, 0, 0);
        rotCtx.setTransform(1, 0, 0, 1, 0, 0);

        // Step 2: Scale to fit 480x800 (if needed)
        const fitsInScreen = rotW <= MAX_WIDTH && rotH <= MAX_HEIGHT;

        if (fitsInScreen) {
          // Already fits after rotation - just apply grayscale
          applyGrayscale(rotCtx, rotW, rotH);
          const blob = await new Promise(res => rotCanvas.toBlob(res, 'image/jpeg', JPEG_QUALITY / 100));
          const arrBuf = await blob.arrayBuffer();
          resolve({
            parts: [{ data: arrBuf, suffix: '', width: rotW, height: rotH, size: arrBuf.byteLength }],
            meta: { origW, origH, origSize, wasSplit: false, rotated: true, finalW: rotW, finalH: rotH, finalSize: arrBuf.byteLength, imageState: 3 }
          });
        } else {
          // Scale to fit 480x800
          const scale = Math.min(MAX_WIDTH / rotW, MAX_HEIGHT / rotH);
          const newW = Math.round(rotW * scale);
          const newH = Math.round(rotH * scale);

          const scaledCanvas = document.createElement('canvas');
          scaledCanvas.width = newW;
          scaledCanvas.height = newH;
          const scaledCtx = scaledCanvas.getContext('2d');
          scaledCtx.imageSmoothingEnabled = true;
          scaledCtx.imageSmoothingQuality = 'high';
          scaledCtx.fillStyle = '#FFF';
          scaledCtx.fillRect(0, 0, newW, newH);
          scaledCtx.drawImage(rotCanvas, 0, 0, newW, newH);
          applyGrayscale(scaledCtx, newW, newH);

          const blob = await new Promise(res => scaledCanvas.toBlob(res, 'image/jpeg', JPEG_QUALITY / 100));
          const arrBuf = await blob.arrayBuffer();
          resolve({
            parts: [{ data: arrBuf, suffix: '', width: newW, height: newH, size: arrBuf.byteLength }],
            meta: { origW, origH, origSize, wasSplit: false, rotated: true, finalW: newW, finalH: newH, finalSize: arrBuf.byteLength, imageState: 3 }
          });
        }
      }
      // ========================================================================
      // STATE 0: Normal processing (scale to fit, no split/rotation)
      // ========================================================================
      else {
        // Normal processing: check if scaling is needed
        const fitsInScreen = origW <= MAX_WIDTH && origH <= MAX_HEIGHT;

        if (fitsInScreen) {
          // Image already fits - just convert to JPEG with grayscale
          const c = document.createElement('canvas');
          c.width = origW;
          c.height = origH;
          const ctx = c.getContext('2d');
          ctx.fillStyle = '#FFF';
          ctx.fillRect(0, 0, origW, origH);
          ctx.drawImage(img, 0, 0);
          applyGrayscale(ctx, origW, origH);

          const blob = await new Promise(res => c.toBlob(res, 'image/jpeg', JPEG_QUALITY / 100));
          const arrBuf = await blob.arrayBuffer();
          resolve({
            parts: [{ data: arrBuf, suffix: '', width: origW, height: origH, size: arrBuf.byteLength }],
            meta: { origW, origH, origSize, wasSplit: false, rotated: false, finalW: origW, finalH: origH, finalSize: arrBuf.byteLength, imageState: 0 }
          });
        } else {
          // Scale to fit 480x800
          const scale = Math.min(MAX_WIDTH / origW, MAX_HEIGHT / origH);
          const newW = Math.round(origW * scale);
          const newH = Math.round(origH * scale);

          const c = document.createElement('canvas');
          c.width = newW;
          c.height = newH;
          const ctx = c.getContext('2d');
          ctx.imageSmoothingEnabled = true;
          ctx.imageSmoothingQuality = 'high';
          ctx.fillStyle = '#FFF';
          ctx.fillRect(0, 0, newW, newH);
          ctx.drawImage(img, 0, 0, newW, newH);
          applyGrayscale(ctx, newW, newH);

          const blob = await new Promise(res => c.toBlob(res, 'image/jpeg', JPEG_QUALITY / 100));
          const arrBuf = await blob.arrayBuffer();
          resolve({
            parts: [{ data: arrBuf, suffix: '', width: newW, height: newH, size: arrBuf.byteLength }],
            meta: { origW, origH, origSize, wasSplit: false, rotated: false, finalW: newW, finalH: newH, finalSize: arrBuf.byteLength, imageState: 0 }
          });
        }
      }
    };
    img.onerror = () => {
      clearTimeout(timeoutId);
      URL.revokeObjectURL(url);
      reject(new Error('Image load failed'));
    };
    img.src = url;
  });
}

// ═══════════════════════════════════════════════════════════════════
// FB2 Cover Optimization — mirrors the EPUB image pipeline (canvas resize +
// JPEG re-encode), but FB2 is plain XML (no zip), so we only need to find
// and rewrite the single cover <binary> block instead of unzipping/rezipping
// a whole package.
// ═══════════════════════════════════════════════════════════════════

/** Detect the FB2 text encoding from its XML declaration and decode to a string. */
/**
 * Real-world FB2 exporters aren't consistent about the separator before the
 * ".zip" — "Book.fb2.zip", "Book_fb2.zip" and "Book-fb2.zip" all show up in
 * the wild. Match any of them instead of requiring a literal dot.
 */
function isFb2ZipName(name) {
  const n = (name || '').toLowerCase().trim();
  if (!n.endsWith('.zip')) return false;

  // Accept normal names:
  //   Book.fb2.zip
  //   Book_fb2.zip
  // and browser/Windows duplicate names:
  //   Book.fb2 (1).zip
  //   Book.fb2 (12).zip
  const stem = n.slice(0, -4).trim();
  return /(?:^|[._ -])fb2(?:\s*\(\d+\))?$/.test(stem);
}

function canonicalFb2ZipUploadName(name) {
  const original = String(name || '');
  const withoutZip = original.replace(/\.zip$/i, '').trim();

  // If the archive name already contains a trailing FB2 marker, including
  // duplicate-download suffixes like ".fb2 (1)", collapse it to ".fb2".
  if (/(?:^|[._ -])fb2(?:\s*\(\d+\))?$/i.test(withoutZip)) {
    return withoutZip
      .replace(/\s*\(\d+\)\s*$/i, '')
      .replace(/([._ -])fb2$/i, '.fb2') + '.zip';
  }

  // Generic Book.zip whose CONTENT was detected as FB2.
  return withoutZip + '.fb2.zip';
}
function isFb2Name(name) {
  const n = (name || '').toLowerCase();
  return n.endsWith('.fb2') || isFb2ZipName(n);
}

/**
 * Upload-time FB2 detection. A ZIP does not have to be named *.fb2.zip:
 * many libraries distribute names such as "Book.zip". Inspect the archive
 * directory and treat it as an FB2 book when it actually contains a .fb2.
 * Ordinary ZIP files stay ordinary files.
 */
async function isFb2UploadFile(file) {
  const name = (file && file.name || '').toLowerCase();
  if (name.endsWith('.fb2')) return true;
  if (!name.endsWith('.zip')) return false;
  if (isFb2ZipName(name)) return true;

  try {
    const zip = await JSZip.loadAsync(file);
    return Object.keys(zip.files).some(path => !zip.files[path].dir && path.toLowerCase().endsWith('.fb2'));
  } catch (_) {
    return false;
  }
}

// Accept both a real EPUB renamed to .zip and an outer archive containing one
// .epub file. Ordinary ZIPs are returned unchanged.
async function extractEpubFromUpload(file) {
  const name = (file && file.name || '').toLowerCase();
  if (name.endsWith('.epub') || !name.endsWith('.zip')) return file;
  try {
    const zip = await JSZip.loadAsync(file);
    if (zip.file('META-INF/container.xml')) {
      return new File([file], file.name.replace(/\.zip$/i, '.epub'), { type: 'application/epub+zip' });
    }
    const nested = Object.keys(zip.files).find(path => !zip.files[path].dir && path.toLowerCase().endsWith('.epub'));
    if (nested) {
      const blob = await zip.file(nested).async('blob');
      return new File([blob], nested.split('/').pop(), { type: 'application/epub+zip' });
    }
  } catch (_) { /* not a readable ZIP */ }
  return file;
}

/** Standalone image files (not embedded in an EPUB/FB2) eligible for the
 * same screen-fit resize/optimize pipeline as EPUB/FB2 covers and images. */
const STANDALONE_IMAGE_EXTENSIONS = ['.jpg', '.jpeg', '.png', '.gif', '.webp', '.bmp'];
function isImageName(name) {
  const n = (name || '').toLowerCase();
  return STANDALONE_IMAGE_EXTENSIONS.some(ext => n.endsWith(ext));
}

function decodeFb2Bytes(buf) {
  let offset = 0;
  if (buf.length >= 3 && buf[0] === 0xEF && buf[1] === 0xBB && buf[2] === 0xBF) offset = 3;

  const utf8 = new TextDecoder('utf-8', { fatal: true });
  try {
    return utf8.decode(buf.subarray(offset));
  } catch (e) { /* not valid UTF-8, sniff the declared encoding */ }

  const ascii = new TextDecoder('ascii', { fatal: false }).decode(buf.subarray(offset, offset + 200));
  const m = ascii.match(/encoding=["']([^"']+)["']/i);
  const encoding = m ? m[1].toLowerCase() : 'windows-1251'; // common FB2 default
  try {
    return new TextDecoder(encoding, { fatal: false }).decode(buf.subarray(offset));
  } catch (e) {
    return new TextDecoder('windows-1251', { fatal: false }).decode(buf.subarray(offset));
  }
}

/** Read a plain (uncompressed) .fb2 File and decode it to a string. */
async function readFb2Text(file) {
  return decodeFb2Bytes(new Uint8Array(await file.arrayBuffer()));
}

/** If `file` is a .fb2.zip, locate the inner .fb2 entry. Returns
 *  { zip, entryPath } so the caller can both read and later rewrite that
 *  entry before re-zipping. Throws if no .fb2 is found inside the archive. */
async function openFb2Zip(file) {
  const zip = await JSZip.loadAsync(file);
  let entryPath = null;
  zip.forEach(p => {
    if (!entryPath && p.toLowerCase().endsWith('.fb2')) entryPath = p;
  });
  if (!entryPath) throw new Error('No .fb2 file found inside archive');
  return { zip, entryPath };
}

/** Find the binary id referenced by <coverpage><image .../></coverpage>. */
function findFb2CoverId(fb2Text) {
  const coverpageMatch = fb2Text.match(/<coverpage>([\s\S]*?)<\/coverpage>/i);
  if (!coverpageMatch) return null;
  const hrefMatch = coverpageMatch[1].match(/(?:xlink:href|l:href|href)\s*=\s*["']#([^"']+)["']/i);
  return hrefMatch ? hrefMatch[1] : null;
}

/** Find a <binary id="..."> element: its content-type, base64 payload, and span in the text. */
function findFb2Binary(fb2Text, id) {
  const re = new RegExp(`<binary\\b[^>]*\\bid=["']${escapeRegex(id)}["'][^>]*>([\\s\\S]*?)</binary>`, 'i');
  const match = re.exec(fb2Text);
  if (!match) return null;
  const ctMatch = match[0].match(/content-type=["']([^"']+)["']/i);
  return {
    fullMatch: match[0],
    index: match.index,
    length: match[0].length,
    contentType: ctMatch ? ctMatch[1] : 'image/jpeg',
    base64: match[1].replace(/\s+/g, ''),
  };
}

/** Convert a base64 string to a Blob. */
function base64ToBlob(base64, mimeType) {
  const binary = atob(base64);
  const bytes = new Uint8Array(binary.length);
  for (let i = 0; i < binary.length; i++) bytes[i] = binary.charCodeAt(i);
  return new Blob([bytes], { type: mimeType });
}

/** Convert a Blob to a base64 string (no "data:...;base64," prefix). */
function blobToBase64(blob) {
  return new Promise((resolve, reject) => {
    const reader = new FileReader();
    reader.onload = () => resolve(reader.result.split(',')[1]);
    reader.onerror = reject;
    reader.readAsDataURL(blob);
  });
}

/** Load a Blob into an <img>, resolving with the element + its natural dimensions. */
function loadImageFromBlob(blob) {
  return new Promise((resolve, reject) => {
    const url = URL.createObjectURL(blob);
    const img = new Image();
    img.onload = () => resolve({ img, width: img.width, height: img.height, url });
    img.onerror = () => { URL.revokeObjectURL(url); reject(new Error('Image decode failed')); };
    img.src = url;
  });
}

/**
 * Optimize an FB2's cover image the same way EPUB images are optimized:
 * scale to fit the active device profile (MAX_WIDTH × MAX_HEIGHT, same as
 * EPUB — auto-detected X3/X4 or whatever the user picked), optionally
 * grayscale, re-encode as JPEG at JPEG_QUALITY. Only the cover's <binary>
 * block is rewritten; the rest of the FB2 XML is left untouched.
 *
 * Returns a new File if the cover was changed, or the original File
 * unchanged if there was nothing to do (no cover found, or it already fits).
 */
async function optimizeRemainingFb2Images(fb2Text, alreadyOptimizedId, progressCallback) {
  const binaryRegex = /<binary\b[^>]*\bid=["']([^"']+)["'][^>]*>([\s\S]*?)<\/binary>/gi;
  const replacements = [];
  let match;
  while ((match = binaryRegex.exec(fb2Text)) !== null) {
    const tag = match[0];
    const id = match[1];
    const contentType = (tag.match(/content-type=["']([^"']+)["']/i) || [])[1] || '';
    if (id === alreadyOptimizedId || !contentType.toLowerCase().startsWith('image/')) continue;

    try {
      const base64 = match[2].replace(/\s+/g, '');
      const loaded = await loadImageFromBlob(base64ToBlob(base64, contentType));
      const { img, width: origW, height: origH, url } = loaded;
      if (origW <= MAX_WIDTH && origH <= MAX_HEIGHT) {
        URL.revokeObjectURL(url);
        continue;
      }
      const scale = Math.min(MAX_WIDTH / origW, MAX_HEIGHT / origH);
      const newW = Math.round(origW * scale);
      const newH = Math.round(origH * scale);
      const canvas = document.createElement('canvas');
      canvas.width = newW;
      canvas.height = newH;
      const ctx = canvas.getContext('2d');
      ctx.imageSmoothingEnabled = true;
      ctx.imageSmoothingQuality = 'high';
      ctx.fillStyle = '#FFF';
      ctx.fillRect(0, 0, newW, newH);
      ctx.drawImage(img, 0, 0, newW, newH);
      applyGrayscale(ctx, newW, newH);
      URL.revokeObjectURL(url);
      const newBlob = await new Promise(resolve => canvas.toBlob(resolve, 'image/jpeg', JPEG_QUALITY / 100));
      if (!newBlob) throw new Error('JPEG encoding failed');
      const newBase64 = await blobToBase64(newBlob);
      replacements.push({
        index: match.index,
        length: tag.length,
        text: tag.replace(/content-type=["'][^"']*["']/i, 'content-type="image/jpeg"')
                 .replace(/>[\s\S]*<\/binary>/i, '>' + newBase64 + '</binary>')
      });
      logImage('image (' + id + ')', origW, origH, contentType.split('/')[1] || 'img',
               Math.round(base64.length * 0.75), newW, newH, newBlob.size, false, 0, null, 0);
    } catch (e) {
      logError('Failed to decode image ' + id + ': ' + e.message);
    }
  }
  if (replacements.length === 0) return fb2Text;

  let cursor = 0;
  let result = '';
  for (const replacement of replacements) {
    result += fb2Text.slice(cursor, replacement.index) + replacement.text;
    cursor = replacement.index + replacement.length;
  }
  return result + fb2Text.slice(cursor);
}

async function convertFb2File(file, progressCallback, optimizeAllImages = true) {
  const startTime = Date.now();
  const originalSize = file.size;
  const isZip = /\.zip$/i.test(file.name);

  clearLog();
  showLog();
  log(`<strong>${file.name}</strong> <span class="log-detail">(${formatBytes(originalSize)})</span>`, '', 'INFO');
  log(`Quality: ${JPEG_QUALITY}% | Grayscale: ${ENABLE_GRAYSCALE ? 'ON' : 'OFF'} | Target: ${MAX_WIDTH}×${MAX_HEIGHT}`, '', 'INFO');

  let zip = null;
  let fb2EntryPath = null;
  let fb2Text;
  if (isZip) {
    try {
      ({ zip, entryPath: fb2EntryPath } = await openFb2Zip(file));
    } catch (e) {
      logSkip(file.name, 'no .fb2 file found inside archive');
      log('Archive has no FB2 — uploading unchanged.', '', 'INFO');
      logSummary(originalSize, originalSize, (Date.now() - startTime) / 1000);
      if (progressCallback) progressCallback(100);
      return file;
    }
    fb2Text = decodeFb2Bytes(await zip.file(fb2EntryPath).async('uint8array'));
  } else {
    fb2Text = await readFb2Text(file);
  }
  if (progressCallback) progressCallback(20);

  const imageBinaryIds = [];
  const imageBinaryRegex = /<binary\b[^>]*\bid=["']([^"']+)["'][^>]*>/gi;
  let imageBinaryMatch;
  while ((imageBinaryMatch = imageBinaryRegex.exec(fb2Text)) !== null) imageBinaryIds.push(imageBinaryMatch[1]);
  const coverId = findFb2CoverId(fb2Text) || imageBinaryIds[0];
  if (!coverId) {
    logSkip(file.name, 'no <coverpage> reference found');
    log('No cover found — uploading unchanged.', '', 'INFO');
    logSummary(originalSize, originalSize, (Date.now() - startTime) / 1000);
    if (progressCallback) progressCallback(100);
    return file;
  }

  const binary = findFb2Binary(fb2Text, coverId);
  if (!binary) {
    logSkip(file.name, `binary "${coverId}" not found`);
    log('Cover binary missing — uploading unchanged.', '', 'INFO');
    logSummary(originalSize, originalSize, (Date.now() - startTime) / 1000);
    if (progressCallback) progressCallback(100);
    return file;
  }

  let loaded;
  try {
    const blob = base64ToBlob(binary.base64, binary.contentType);
    loaded = await loadImageFromBlob(blob);
  } catch (e) {
    logError(`Failed to decode cover image: ${e.message}`);
    logSummary(originalSize, originalSize, (Date.now() - startTime) / 1000);
    if (progressCallback) progressCallback(100);
    return file;
  }
  if (progressCallback) progressCallback(50);

  const { img, width: origW, height: origH, url } = loaded;

  // Always pass the FB2 cover through Canvas, even when it already fits.
  // Besides optional grayscale/quality normalization this converts progressive
  // JPEG covers to a baseline JPEG that the X3/X4 can decode at full detail.
  // This specifically fixes covers such as 470×720 progressive JPEGs which
  // otherwise decode on-device only as a tiny 1/8 DC preview.
  const fitsScreen = origW <= MAX_WIDTH && origH <= MAX_HEIGHT;
  const scale = fitsScreen ? 1 : Math.min(MAX_WIDTH / origW, MAX_HEIGHT / origH);
  const newW = Math.round(origW * scale);
  const newH = Math.round(origH * scale);

  const canvas = document.createElement('canvas');
  canvas.width = newW;
  canvas.height = newH;
  const ctx = canvas.getContext('2d');
  ctx.imageSmoothingEnabled = true;
  ctx.imageSmoothingQuality = 'high';
  ctx.fillStyle = '#FFF';
  ctx.fillRect(0, 0, newW, newH);
  ctx.drawImage(img, 0, 0, newW, newH);
  applyGrayscale(ctx, newW, newH);
  URL.revokeObjectURL(url);

  const newBlob = await new Promise(res => canvas.toBlob(res, 'image/jpeg', JPEG_QUALITY / 100));
  const newBase64 = await blobToBase64(newBlob);
  if (progressCallback) progressCallback(80);

  // Rebuild the <binary> tag with the updated content-type + payload, same id.
  const newBinaryTag = binary.fullMatch
    .replace(/content-type=["'][^"']*["']/i, 'content-type="image/jpeg"')
    .replace(/>[\s\S]*<\/binary>/i, `>${newBase64}</binary>`);

  let newFb2Text = fb2Text.slice(0, binary.index) + newBinaryTag + fb2Text.slice(binary.index + binary.length);

  // The cover is always normalized because progressive JPEG covers are only
  // decodable at reduced detail by the device JPEG path. Other illustrations
  // are still optimized only when the user enabled the normal Optimization
  // checkbox, so automatic cover safety does not unexpectedly rewrite the
  // rest of the book.
  if (optimizeAllImages) {
    newFb2Text = await optimizeRemainingFb2Images(newFb2Text, coverId, progressCallback);
  }

  let newFile;
  if (isZip) {
    // Rewrite only the .fb2 entry inside the archive; any other entries
    // (rare, but be safe) are kept exactly as they were.
    zip.file(fb2EntryPath, newFb2Text);
    const newZipBlob = await zip.generateAsync({ type: 'blob', mimeType: 'application/zip' });
    newFile = new File([newZipBlob], file.name, { type: file.type || 'application/zip' });
  } else {
    newFile = new File([newFb2Text], file.name, { type: file.type || 'application/octet-stream' });
  }

  logImage(`cover (${coverId})`, origW, origH, (binary.contentType.split('/')[1] || 'img'),
           Math.round(binary.base64.length * 0.75), newW, newH, newBlob.size, false, 0, null, 0);
  if (fitsScreen) log(`Cover normalized: ${origW}×${origH} → baseline JPEG`, 'success', 'DONE');
  log(t('files.cover_opt_complete'), 'success', 'DONE');
  logSummary(originalSize, newFile.size, (Date.now() - startTime) / 1000);

  if (progressCallback) progressCallback(100);
  return newFile;
}

// ═══════════════════════════════════════════════════════════════════
// FB2 → EPUB structural conversion — unlike convertFb2File (which only
// re-encodes the cover image in place), this rebuilds the book as a real
// EPUB: each top-level <section> becomes its own XHTML chapter, inline
// formatting/images are translated, and a proper OPF/NCX/nav is generated.
// ═══════════════════════════════════════════════════════════════════

function parseFb2Xml(fb2Text) {
  const doc = new DOMParser().parseFromString(fb2Text, 'application/xml');
  if (doc.querySelector('parsererror')) throw new Error('FB2 is not valid XML');
  return doc;
}

function fb2Children(el, localName) {
  return Array.from(el.children).filter(c => (c.localName || c.tagName) === localName);
}
function fb2Child(el, localName) {
  return fb2Children(el, localName)[0] || null;
}
function fb2TextOf(el) {
  return el ? el.textContent.replace(/\s+/g, ' ').trim() : '';
}
function fb2AuthorName(authorEl) {
  const first = fb2TextOf(fb2Child(authorEl, 'first-name'));
  const middle = fb2TextOf(fb2Child(authorEl, 'middle-name'));
  const last = fb2TextOf(fb2Child(authorEl, 'last-name'));
  const nick = fb2TextOf(fb2Child(authorEl, 'nickname'));
  const full = [first, middle, last].filter(Boolean).join(' ').trim();
  return full || nick || 'Unknown';
}

/** Joined text of a <title> for use as a chapter/TOC label. */
function fb2SectionTitleText(sectionEl, fallback) {
  const titleEl = fb2Child(sectionEl, 'title');
  if (!titleEl) return fallback;
  const lines = fb2Children(titleEl, 'p').map(fb2TextOf).filter(Boolean);
  return lines.join(' ') || fb2TextOf(titleEl) || fallback;
}

function extractFb2Metadata(doc) {
  const root = doc.documentElement;
  const description = fb2Child(root, 'description');
  const titleInfo = description ? fb2Child(description, 'title-info') : null;
  const title = fb2TextOf(titleInfo ? fb2Child(titleInfo, 'book-title') : null) || 'Untitled';
  const authors = titleInfo ? fb2Children(titleInfo, 'author').map(fb2AuthorName) : [];
  const lang = fb2TextOf(titleInfo ? fb2Child(titleInfo, 'lang') : null) || 'en';
  const annotationEl = titleInfo ? fb2Child(titleInfo, 'annotation') : null;
  const annotation = annotationEl ? fb2Children(annotationEl, 'p').map(fb2TextOf).filter(Boolean).join(' ') : '';
  const dateEl = titleInfo ? fb2Child(titleInfo, 'date') : null;
  const date = dateEl ? (dateEl.getAttribute('value') || fb2TextOf(dateEl)) : '';

  let coverId = null;
  const coverpage = titleInfo ? fb2Child(titleInfo, 'coverpage') : null;
  if (coverpage) {
    const img = fb2Child(coverpage, 'image');
    if (img) {
      const href = img.getAttribute('l:href') || img.getAttribute('xlink:href') || img.getAttribute('href');
      if (href) coverId = href.replace(/^#/, '');
    }
  }
  return { title, authors, lang, annotation, annotationEl, date, coverId };
}

function extractFb2Binaries(doc) {
  const map = {};
  const bins = doc.getElementsByTagName('binary');
  for (const b of bins) {
    const id = b.getAttribute('id');
    if (!id) continue;
    map[id] = {
      contentType: b.getAttribute('content-type') || 'image/jpeg',
      base64: (b.textContent || '').replace(/\s+/g, ''),
    };
  }
  return map;
}

/**
 * Resize + re-encode a single embedded FB2 image (base64 + content-type) to
 * fit the active device screen (MAX_WIDTH × MAX_HEIGHT — the same setting
 * the "Optimize EPUB/FB2" cover mode and Advanced Mode device picker use).
 * Returns the original payload unchanged (resized: false) if it already fits.
 */
async function resizeImageToScreen(base64, contentType) {
  const blob = base64ToBlob(base64, contentType);
  const { img, width: origW, height: origH, url } = await loadImageFromBlob(blob);

  // Covers specifically (this function isn't used for the book's other
  // illustrations) always get scaled to fit MAX_WIDTH×MAX_HEIGHT, in both
  // directions - a cover left smaller than the screen gets upscaled by the
  // *device* at display time instead, and that on-device scaling produces
  // a visibly blurrier result than doing it here with proper interpolation
  // (imageSmoothingQuality below). Skip only when it's already exactly the
  // target size, so a cover that's already right doesn't get needlessly
  // re-encoded.
  if (origW === MAX_WIDTH && origH === MAX_HEIGHT) {
    URL.revokeObjectURL(url);
    return { base64, contentType, resized: false, width: origW, height: origH };
  }

  const scale = Math.min(MAX_WIDTH / origW, MAX_HEIGHT / origH);
  const newW = Math.round(origW * scale);
  const newH = Math.round(origH * scale);

  const canvas = document.createElement('canvas');
  canvas.width = newW;
  canvas.height = newH;
  const ctx = canvas.getContext('2d');
  ctx.imageSmoothingEnabled = true;
  ctx.imageSmoothingQuality = 'high';
  ctx.fillStyle = '#FFF';
  ctx.fillRect(0, 0, newW, newH);
  ctx.drawImage(img, 0, 0, newW, newH);
  applyGrayscale(ctx, newW, newH);
  URL.revokeObjectURL(url);

  const newBlob = await new Promise(res => canvas.toBlob(res, 'image/jpeg', JPEG_QUALITY / 100));
  const newBase64 = await blobToBase64(newBlob);
  return {
    base64: newBase64, contentType: 'image/jpeg', resized: true,
    width: newW, height: newH, origWidth: origW, origHeight: origH,
  };
}

/**
 * Resize + re-encode a standalone image file (picked directly, not embedded
 * in an EPUB/FB2) to fit the active device screen (MAX_WIDTH × MAX_HEIGHT —
 * same target the EPUB/FB2 cover and image pipelines use). Keeps aspect
 * ratio (contain, no crop), optionally grayscales, re-encodes as JPEG at
 * JPEG_QUALITY. If the image already fits, it's still re-encoded as JPEG at
 * the chosen quality so "Optimize" always produces a predictable output —
 * pass through untouched only when it's already a same-size JPEG.
 */
async function convertImageFile(file, progressCallback) {
  if (progressCallback) progressCallback(5);
  const { img, width: origW, height: origH, url } = await loadImageFromBlob(file);
  if (progressCallback) progressCallback(35);

  const fitsScreen = origW <= MAX_WIDTH && origH <= MAX_HEIGHT;
  const alreadyRightFormat = fitsScreen && file.type === 'image/jpeg' && !ENABLE_GRAYSCALE;
  if (alreadyRightFormat) {
    URL.revokeObjectURL(url);
    if (progressCallback) progressCallback(100);
    return { blob: file, resized: false, width: origW, height: origH };
  }

  const scale = fitsScreen ? 1 : Math.min(MAX_WIDTH / origW, MAX_HEIGHT / origH);
  const newW = Math.max(1, Math.round(origW * scale));
  const newH = Math.max(1, Math.round(origH * scale));

  const canvas = document.createElement('canvas');
  canvas.width = newW;
  canvas.height = newH;
  const ctx = canvas.getContext('2d');
  ctx.imageSmoothingEnabled = true;
  ctx.imageSmoothingQuality = 'high';
  ctx.fillStyle = '#FFF';
  ctx.fillRect(0, 0, newW, newH);
  ctx.drawImage(img, 0, 0, newW, newH);
  applyGrayscale(ctx, newW, newH);
  URL.revokeObjectURL(url);
  if (progressCallback) progressCallback(75);

  const blob = await new Promise(res => canvas.toBlob(res, 'image/jpeg', JPEG_QUALITY / 100));
  if (progressCallback) progressCallback(100);
  return { blob, resized: true, width: newW, height: newH, origWidth: origW, origHeight: origH };
}

function fb2ExtFromMime(mime) {
  const m = (mime || '').toLowerCase();
  if (m.includes('png')) return 'png';
  if (m.includes('gif')) return 'gif';
  if (m.includes('webp')) return 'webp';
  if (m.includes('bmp')) return 'bmp';
  return 'jpg';
}

function fb2ImageInfo(imgEl, imageMap) {
  const href = imgEl.getAttribute('l:href') || imgEl.getAttribute('xlink:href') || imgEl.getAttribute('href');
  if (!href) return null;
  return imageMap[href.replace(/^#/, '')] || null;
}

function sanitizeXmlId(id) {
  let s = String(id).replace(/[^A-Za-z0-9_-]/g, '_');
  if (!/^[A-Za-z_]/.test(s)) s = 'id_' + s;
  return s;
}

function generateEpubUuid() {
  return 'xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx'.replace(/[xy]/g, c => {
    const r = (Math.random() * 16) | 0;
    const v = c === 'x' ? r : (r & 0x3) | 0x8;
    return v.toString(16);
  });
}

function createXhtmlDoc(titleText) {
  const template = '<html xmlns="http://www.w3.org/1999/xhtml" xmlns:epub="http://www.idpf.org/2007/ops">'
    + '<head><meta charset="utf-8"/><title>' + escapeHtml(titleText || '') + '</title>'
    + '<link rel="stylesheet" type="text/css" href="../style.css"/></head><body></body></html>';
  return new DOMParser().parseFromString(template, 'application/xhtml+xml');
}

function serializeXhtmlDoc(doc) {
  const xml = new XMLSerializer().serializeToString(doc);
  return '<?xml version="1.0" encoding="UTF-8"?>\n<!DOCTYPE html>\n' + xml;
}

const FB2_INLINE_TAG_MAP = { emphasis: 'em', strong: 'strong', strikethrough: 's', sub: 'sub', sup: 'sup', code: 'code' };

/**
 * Render inline (character-level) FB2 markup found inside a <p>, <v>, heading, etc.
 * ctx = { imageMap, ns, noteIds, notesFilename } — noteIds/notesFilename let
 * footnote <a l:href="#n_x" type="note"> links resolve to the generated notes chapter.
 */
function renderFb2Inline(srcEl, doc, targetEl, ctx) {
  for (const child of srcEl.childNodes) {
    if (child.nodeType === 3) {
      if (child.nodeValue) targetEl.appendChild(doc.createTextNode(child.nodeValue));
      continue;
    }
    if (child.nodeType !== 1) continue;
    const tag = child.localName || child.tagName;
    if (tag === 'image') {
      const info = fb2ImageInfo(child, ctx.imageMap);
      if (info) {
        const img = doc.createElementNS(ctx.ns, 'img');
        img.setAttribute('src', '../images/' + info.filename);
        img.setAttribute('alt', '');
        targetEl.appendChild(img);
      }
      continue;
    }
    if (tag === 'a') {
      const href = child.getAttribute('l:href') || child.getAttribute('xlink:href') || child.getAttribute('href');
      const rawId = href ? href.replace(/^#/, '') : null;
      if (rawId && ctx.noteIds && ctx.noteIds.has(rawId) && ctx.notesFilename) {
        const a = doc.createElementNS(ctx.ns, 'a');
        a.setAttribute('href', ctx.notesFilename + '#' + sanitizeXmlId(rawId));
        a.setAttribute('class', 'note-ref');
        a.setAttribute('epub:type', 'noteref');
        renderFb2Inline(child, doc, a, ctx);
        targetEl.appendChild(a);
        continue;
      }
      // Non-footnote or unresolved link — just flow its text so nothing is lost.
      renderFb2Inline(child, doc, targetEl, ctx);
      continue;
    }
    const mapped = FB2_INLINE_TAG_MAP[tag];
    if (mapped) {
      const e = doc.createElementNS(ctx.ns, mapped);
      renderFb2Inline(child, doc, e, ctx);
      targetEl.appendChild(e);
    } else {
      // Unknown inline element — keep its text flowing rather than dropping it.
      renderFb2Inline(child, doc, targetEl, ctx);
    }
  }
}

/** Render one FB2 block-level element (not its siblings) into xhtmlParent. */
function renderFb2SingleBlock(child, doc, xhtmlParent, ctx) {
  const tag = child.localName || child.tagName;
  switch (tag) {
    case 'p': {
      const p = doc.createElementNS(ctx.ns, 'p');
      renderFb2Inline(child, doc, p, ctx);
      xhtmlParent.appendChild(p);
      break;
    }
    case 'empty-line':
      xhtmlParent.appendChild(doc.createElementNS(ctx.ns, 'br'));
      break;
    case 'subtitle': {
      const h = doc.createElementNS(ctx.ns, 'p');
      h.setAttribute('class', 'subtitle');
      renderFb2Inline(child, doc, h, ctx);
      xhtmlParent.appendChild(h);
      break;
    }
    case 'image': {
      const info = fb2ImageInfo(child, ctx.imageMap);
      if (info) {
        const div = doc.createElementNS(ctx.ns, 'div');
        div.setAttribute('class', 'illustration');
        const img = doc.createElementNS(ctx.ns, 'img');
        img.setAttribute('src', '../images/' + info.filename);
        img.setAttribute('alt', '');
        div.appendChild(img);
        xhtmlParent.appendChild(div);
      }
      break;
    }
    case 'poem':
    case 'cite':
    case 'epigraph': {
      const div = doc.createElementNS(ctx.ns, 'div');
      div.setAttribute('class', tag);
      const t = fb2Child(child, 'title');
      if (t) {
        const h = doc.createElementNS(ctx.ns, 'p');
        h.setAttribute('class', 'block-title');
        renderFb2Inline(t, doc, h, ctx);
        div.appendChild(h);
      }
      for (const grandchild of child.children) {
        if ((grandchild.localName || grandchild.tagName) === 'title') continue;
        renderFb2SingleBlock(grandchild, doc, div, ctx);
      }
      xhtmlParent.appendChild(div);
      break;
    }
    case 'stanza': {
      const div = doc.createElementNS(ctx.ns, 'div');
      div.setAttribute('class', 'stanza');
      for (const grandchild of child.children) renderFb2SingleBlock(grandchild, doc, div, ctx);
      xhtmlParent.appendChild(div);
      break;
    }
    case 'v': {
      const p = doc.createElementNS(ctx.ns, 'p');
      p.setAttribute('class', 'verse');
      renderFb2Inline(child, doc, p, ctx);
      xhtmlParent.appendChild(p);
      break;
    }
    case 'text-author': {
      const p = doc.createElementNS(ctx.ns, 'p');
      p.setAttribute('class', 'text-author');
      renderFb2Inline(child, doc, p, ctx);
      xhtmlParent.appendChild(p);
      break;
    }
    case 'date': {
      const p = doc.createElementNS(ctx.ns, 'p');
      p.setAttribute('class', 'date');
      renderFb2Inline(child, doc, p, ctx);
      xhtmlParent.appendChild(p);
      break;
    }
    case 'table': {
      // Minimal fallback: flatten rows to text lines rather than a full <table>.
      for (const row of child.children) {
        const p = doc.createElementNS(ctx.ns, 'p');
        p.textContent = Array.from(row.children).map(c => c.textContent.trim()).join(' | ');
        xhtmlParent.appendChild(p);
      }
      break;
    }
    case 'title':
    case 'annotation':
      break; // handled by the caller where relevant
    default:
      // Unknown wrapper — recurse into its children so text isn't silently dropped.
      for (const grandchild of child.children) renderFb2SingleBlock(grandchild, doc, xhtmlParent, ctx);
  }
}

function renderFb2SectionTitle(sectionEl, doc, xhtmlParent, ctx, level) {
  const titleEl = fb2Child(sectionEl, 'title');
  if (!titleEl) return;
  const lines = fb2Children(titleEl, 'p');
  const tag = 'h' + Math.min(Math.max(level, 1), 6);
  if (lines.length === 0) {
    const h = doc.createElementNS(ctx.ns, tag);
    renderFb2Inline(titleEl, doc, h, ctx);
    xhtmlParent.appendChild(h);
    return;
  }
  lines.forEach((line, i) => {
    const h = doc.createElementNS(ctx.ns, i === 0 ? tag : 'p');
    if (i > 0) h.setAttribute('class', 'title-line');
    renderFb2Inline(line, doc, h, ctx);
    xhtmlParent.appendChild(h);
  });
}

/** Render a <section>'s own direct content (title + non-section children), skipping nested <section>s. */
function renderFb2SectionOwnContent(sectionEl, doc, xhtmlParent, ctx, level) {
  renderFb2SectionTitle(sectionEl, doc, xhtmlParent, ctx, level);
  for (const child of sectionEl.children) {
    const tag = child.localName || child.tagName;
    if (tag === 'title' || tag === 'section') continue;
    renderFb2SingleBlock(child, doc, xhtmlParent, ctx);
  }
}

/** Render a <section>'s title + full content into xhtmlParent; nested <section>s recurse with a deeper heading level. */
function renderFb2SectionContent(sectionEl, doc, xhtmlParent, ctx, level) {
  renderFb2SectionTitle(sectionEl, doc, xhtmlParent, ctx, level);
  for (const child of sectionEl.children) {
    const tag = child.localName || child.tagName;
    if (tag === 'title') continue;
    if (tag === 'section') {
      renderFb2SectionContent(child, doc, xhtmlParent, ctx, level + 1);
    } else {
      renderFb2SingleBlock(child, doc, xhtmlParent, ctx);
    }
  }
}

/** Append one footnote/endnote <section id="..."> as a labeled, anchored block. */
function appendFb2Note(sectionEl, doc, xhtmlParent, ctx) {
  const id = sectionEl.getAttribute('id');
  const wrapper = doc.createElementNS(ctx.ns, 'div');
  wrapper.setAttribute('class', 'note');
  if (id) wrapper.setAttribute('id', sanitizeXmlId(id));
  const label = fb2SectionTitleText(sectionEl, '');
  if (label) {
    const p = doc.createElementNS(ctx.ns, 'p');
    p.setAttribute('class', 'note-label');
    p.textContent = label;
    wrapper.appendChild(p);
  }
  for (const child of sectionEl.children) {
    const tag = child.localName || child.tagName;
    if (tag === 'title') continue;
    renderFb2SingleBlock(child, doc, wrapper, ctx);
  }
  xhtmlParent.appendChild(wrapper);
}

/**
 * Split an FB2 body into a flat chapter list, one XHTML file per real chapter
 * instead of one giant file per top-level section. Top-level sections that
 * only group nested chapters (title + epigraph, then a run of <section>
 * children — the common "book inside an omnibus" shape) get their own short
 * intro chapter for that title/epigraph, and each nested <section> becomes
 * its own chapter file. Deeper nesting beyond that stays inline (as before).
 */
function collectFb2ChapterSections(mainBody) {
  const topSections = fb2Children(mainBody, 'section');
  const chapters = []; // { el, mode: 'full' | 'ownOnly' }
  for (const top of topSections) {
    const subSections = fb2Children(top, 'section');
    if (subSections.length === 0) {
      chapters.push({ el: top, mode: 'full' });
      continue;
    }
    const hasOwnContent = Array.from(top.children).some(c => {
      const t = c.localName || c.tagName;
      return t !== 'title' && t !== 'section';
    });
    if (hasOwnContent) chapters.push({ el: top, mode: 'ownOnly' });
    subSections.forEach(sub => chapters.push({ el: sub, mode: 'full' }));
  }
  return chapters;
}

/**
 * Some FB2 exports (typically produced by calibre or similar tools from a
 * PDF or other source with no real markup) put the whole book in one flat
 * <section> — no nested <section> elements at all — with chapter breaks
 * marked only as short bold paragraphs like "Глава I" / "Chapter 12"
 * instead of real structural tags. Left alone, that flat section becomes
 * one giant EPUB chapter with no navigation. This detects that pattern in
 * a flat list of body children and reports where the real chapter breaks
 * are, so the caller can split on those instead.
 */
const FB2_CHAPTER_HEADING_RE = /^(глава|часть|книга|раздел|приложение|эпилог|пролог|предисловие|введение|примечания|заключение|послесловие|от\s+редактора|от\s+автора|chapter|part|book|section|appendix|epilogue|prologue|foreword|introduction|afterword|notes|conclusion)(?![a-zа-яё])/i;

function looksLikeFb2ChapterHeading(pEl) {
  const text = (pEl.textContent || '').replace(/\s+/g, ' ').trim();
  if (!text || text.length > 60) return false;
  // Must be a <p> whose entire content is a single <strong>/<emphasis> run —
  // real body paragraphs mix plain and emphasized text, headings don't.
  const kids = Array.from(pEl.children);
  if (kids.length !== 1) return false;
  const tag = (kids[0].localName || kids[0].tagName || '').toLowerCase();
  if (tag !== 'strong' && tag !== 'emphasis') return false;
  if ((kids[0].textContent || '').replace(/\s+/g, ' ').trim() !== text) return false;
  return FB2_CHAPTER_HEADING_RE.test(text);
}

/**
 * Split a flat list of a section/body's direct children into synthetic
 * per-chapter groups at each detected heading paragraph (see
 * looksLikeFb2ChapterHeading). Returns null — leave the content as one
 * chapter — unless at least two convincing headings are found; a single
 * match is too easy to confuse with an emphasized line inside normal prose.
 * Any content before the first heading (annotation, foreword blurb, etc.)
 * becomes its own untitled lead-in group instead of being dropped.
 */
function splitFb2ChildrenByHeadings(children) {
  const blocks = children.filter(c => (c.localName || c.tagName) !== 'title');
  const headingIdxs = [];
  blocks.forEach((c, i) => {
    if ((c.localName || c.tagName) === 'p' && looksLikeFb2ChapterHeading(c)) headingIdxs.push(i);
  });
  if (headingIdxs.length < 2) return null;

  const groups = [];
  if (headingIdxs[0] > 0) {
    groups.push({ headingText: '', children: blocks.slice(0, headingIdxs[0]) });
  }
  headingIdxs.forEach((startIdx, gi) => {
    const endIdx = gi + 1 < headingIdxs.length ? headingIdxs[gi + 1] : blocks.length;
    const headingText = (blocks[startIdx].textContent || '').replace(/\s+/g, ' ').trim();
    groups.push({ headingText, children: blocks.slice(startIdx + 1, endIdx) });
  });
  return groups;
}

const FB2_EPUB_STYLESHEET =
`body { text-align: justify; }
h1, h2, h3, h4, h5, h6 { text-align: center; font-weight: bold; margin: 1em 0 0.7em 0; }
p.subtitle { text-align: center; }
p { margin: 0.25em 0; }
div.epigraph, div.cite { margin: 0.7em 1.5em; text-indent: 0; }
div.poem { margin: 0.7em 1em; }
div.epigraph > div.poem { margin-bottom: 0; }
div.stanza { margin: 0; padding: 0; }
p.verse { text-indent: 0; text-align: left; margin: 0; }
p.text-author { text-align: right; text-indent: 0; margin: 0.5em 0 0 0; }
p.date, p.fb2-book-date { text-indent: 0; text-align: right; }
h2.fb2-book-author, h2.fb2-book-title { text-align: center; }
p.title-line { text-align: center; font-weight: bold; text-indent: 0; }
div.illustration { text-align: center; margin: 0.5em auto; }
div.illustration img, body.cover img, img { display: block; margin: 0.5em auto; max-width: 100%; height: auto; }
body.cover { text-align: center; margin: 0; }
a.note-ref { text-decoration: none; font-size: 0.75em; vertical-align: super; }
div.note { margin: 0 0 0.8em 0; font-size: 0.95em; }
p.note-label { font-weight: bold; text-indent: 0; margin: 0 0 0.2em 0; display: inline; }
div.note p { display: inline; text-indent: 0; margin: 0 0 0 0.3em; }
`;

/**
 * Convert an FB2/FB2.ZIP file into a real EPUB: chapters, images, footnotes
 * and metadata are generated from the FB2's actual structure (this is a
 * genuine format conversion, unlike convertFb2File which only re-encodes
 * the cover). Returns an EPUB Blob.
 */
async function convertFb2ToEpub(file, progressCallback, optimizeImages = true) {
  const startTime = Date.now();
  const originalSize = file.size;
  const isZip = isFb2ZipName(file.name);

  clearLog();
  showLog();
  log(`<strong>${escapeHtml(file.name)}</strong> <span class="log-detail">(${formatBytes(originalSize)})</span>`, '', 'INFO');
  log('Converting FB2 → EPUB (chapters, images, footnotes, metadata)…', '', 'INFO');
  log(`Cover target: ${MAX_WIDTH}×${MAX_HEIGHT} | Quality: ${JPEG_QUALITY}% | Grayscale: ${ENABLE_GRAYSCALE ? 'ON' : 'OFF'}`, '', 'INFO');

  let fb2Text;
  if (isZip) {
    const { zip, entryPath } = await openFb2Zip(file); // throws if no .fb2 found inside
    fb2Text = decodeFb2Bytes(await zip.file(entryPath).async('uint8array'));
  } else {
    fb2Text = await readFb2Text(file);
  }
  if (progressCallback) progressCallback(10);

  const xmlDoc = parseFb2Xml(fb2Text);
  const meta = extractFb2Metadata(xmlDoc);
  const binaries = extractFb2Binaries(xmlDoc);
  if (progressCallback) progressCallback(20);

  const imageMap = {}; // fb2 binary id -> { filename, contentType, base64 }
  let imgCounter = 1;
  for (const [id, bin] of Object.entries(binaries)) {
    const ext = fb2ExtFromMime(bin.contentType);
    const filename = (id === meta.coverId ? 'cover' : 'image' + String(imgCounter++).padStart(3, '0')) + '.' + ext;
    imageMap[id] = { filename, contentType: bin.contentType, base64: bin.base64 };
  }
  if (progressCallback) progressCallback(25);

  if (optimizeImages && meta.coverId && imageMap[meta.coverId]) {
    try {
      const cover = imageMap[meta.coverId];
      const result = await resizeImageToScreen(cover.base64, cover.contentType);
      if (result.resized) {
        cover.base64 = result.base64;
        cover.contentType = result.contentType;
        cover.filename = 'cover.jpg';
        log(`Cover resized: ${result.origWidth}×${result.origHeight} → ${result.width}×${result.height}`, '', 'INFO');
      } else {
        log(`Cover already fits screen (${result.width}×${result.height})`, '', 'INFO');
      }
    } catch (e) {
      log(`Cover resize failed (${e.message}) — keeping original`, '', 'INFO');
    }
  }
  if (progressCallback) progressCallback(30);

  // Body illustrations (everything in imageMap except the cover, handled
  // above) were previously copied into the EPUB byte-for-byte — unlike
  // convertFb2File's optimizeRemainingFb2Images, which does resize these for
  // the FB2-stays-FB2 path. Apply the same "shrink oversized images, skip
  // ones that already fit" treatment here so a real FB2→EPUB conversion
  // doesn't produce a larger, unoptimized book.
  let bodyImagesResized = 0;
  if (optimizeImages) for (const [id, info] of Object.entries(imageMap)) {
    if (id === meta.coverId) continue;
    try {
      const loaded = await loadImageFromBlob(base64ToBlob(info.base64, info.contentType));
      const { img, width: origW, height: origH, url } = loaded;
      if (origW <= MAX_WIDTH && origH <= MAX_HEIGHT) {
        URL.revokeObjectURL(url);
        continue;
      }
      const scale = Math.min(MAX_WIDTH / origW, MAX_HEIGHT / origH);
      const newW = Math.round(origW * scale);
      const newH = Math.round(origH * scale);
      const canvas = document.createElement('canvas');
      canvas.width = newW;
      canvas.height = newH;
      const imgCtx = canvas.getContext('2d');
      imgCtx.imageSmoothingEnabled = true;
      imgCtx.imageSmoothingQuality = 'high';
      imgCtx.fillStyle = '#FFF';
      imgCtx.fillRect(0, 0, newW, newH);
      imgCtx.drawImage(img, 0, 0, newW, newH);
      applyGrayscale(imgCtx, newW, newH);
      URL.revokeObjectURL(url);
      const newBlob = await new Promise(resolve => canvas.toBlob(resolve, 'image/jpeg', JPEG_QUALITY / 100));
      if (!newBlob) throw new Error('JPEG encoding failed');
      const origSize = Math.round(info.base64.length * 0.75);
      const origFormat = info.contentType.split('/')[1] || 'img';
      info.base64 = await blobToBase64(newBlob);
      info.contentType = 'image/jpeg';
      info.filename = info.filename.replace(/\.[^.]+$/, '.jpg');
      logImage(info.filename, origW, origH, origFormat, origSize, newW, newH, newBlob.size);
      bodyImagesResized++;
    } catch (e) {
      logError(`Failed to optimize image ${id}: ${e.message}`);
    }
  }
  if (bodyImagesResized > 0) {
    log(`Resized ${bodyImagesResized} oversized illustration(s)`, '', 'INFO');
  }
  if (progressCallback) progressCallback(35);

  const ns = 'http://www.w3.org/1999/xhtml';
  const bodies = fb2Children(xmlDoc.documentElement, 'body');
  const mainBody = bodies.find(b => !b.getAttribute('name')) || bodies[0];
  if (!mainBody) throw new Error('FB2 has no readable <body>');
  const extraBodies = bodies.filter(b => b !== mainBody);

  // Collect footnote/endnote ids up front so inline <a> links inside the
  // main text know which references resolve to the notes chapter.
  const noteIds = new Set();
  extraBodies.forEach(b => fb2Children(b, 'section').forEach(sec => {
    const id = sec.getAttribute('id');
    if (id) noteIds.add(id);
  }));
  const notesFilename = noteIds.size > 0 ? 'notes.xhtml' : null;
  const ctx = { imageMap, ns, noteIds, notesFilename };

  const chapters = []; // { id, filename, title, xhtmlString }
  const chapterSections = collectFb2ChapterSections(mainBody);

  // Native FB2 on the device renders metadata/annotation and direct body-level
  // epigraphs before the first real <section>. Build the same front-matter
  // fragment in browser-prepared packages instead of silently dropping it or
  // flattening <emphasis> to plain text. Private inkMOD classes let the reader
  // suppress the chapter footer/page counter on these pages exactly as for the
  // native FB2 path.
  {
    const frontDoc = createXhtmlDoc(meta.title);
    const frontBody = frontDoc.querySelector('body');
    const wrapper = frontDoc.createElementNS(ns, 'div');
    wrapper.setAttribute('class', 'annotation inkmod-fb2-frontmatter');
    if (meta.authors.length) {
      const h = frontDoc.createElementNS(ns, 'h2');
      h.setAttribute('class', 'fb2-book-author');
      h.textContent = meta.authors.join('; ');
      wrapper.appendChild(h);
    }
    if (meta.title) {
      const h = frontDoc.createElementNS(ns, 'h2');
      h.setAttribute('class', 'fb2-book-title');
      h.textContent = meta.title;
      wrapper.appendChild(h);
    }
    if (meta.annotationEl) {
      for (const child of meta.annotationEl.children) renderFb2SingleBlock(child, frontDoc, wrapper, ctx);
    }
    if (meta.date) {
      const d = frontDoc.createElementNS(ns, 'p');
      d.setAttribute('class', 'fb2-book-date');
      d.textContent = meta.date;
      wrapper.appendChild(d);
    }
    frontBody.appendChild(wrapper);

    // Direct children before the first <section> (notably epigraph/poem) are
    // legal FB2 and are rendered by the device's preamble pass. Mirror them.
    for (const child of mainBody.children) {
      const tag = child.localName || child.tagName;
      if (tag === 'section') break;
      if (tag === 'title') continue;
      const pre = frontDoc.createElementNS(ns, 'div');
      pre.setAttribute('class', 'inkmod-fb2-frontmatter');
      renderFb2SingleBlock(child, frontDoc, pre, ctx);
      frontBody.appendChild(pre);
    }
    const br = frontDoc.createElementNS(ns, 'div');
    br.setAttribute('class', 'inkmod-fb2-frontmatter-page-break');
    frontBody.appendChild(br);
    if (frontBody.textContent.trim() || frontBody.querySelector('img')) {
      chapters.push({ id: 'frontmatter', filename: 'frontmatter.xhtml', title: meta.title, xhtmlString: serializeXhtmlDoc(frontDoc), navHidden: true });
    }
  }

  // Flat FB2 (no <section> nesting at all, or exactly one lone top-level
  // section with none nested inside it) — before dumping everything into a
  // single chapter, check whether chapter breaks are marked as plain bold
  // heading paragraphs ("Глава I", "Chapter 12", ...) instead of real
  // <section> tags, and split on those if so.
  let flatChildrenForHeuristic = null;
  if (chapterSections.length === 0) {
    flatChildrenForHeuristic = Array.from(mainBody.children);
  } else if (chapterSections.length === 1 && chapterSections[0].mode === 'full') {
    flatChildrenForHeuristic = Array.from(chapterSections[0].el.children);
  }
  const pseudoChapters = flatChildrenForHeuristic ? splitFb2ChildrenByHeadings(flatChildrenForHeuristic) : null;

  if (pseudoChapters) {
    log(`No &lt;section&gt; markup found for chapters — detected ${pseudoChapters.length} chapter heading(s) in the flat text and split on those instead.`, '', 'INFO');
    pseudoChapters.forEach((grp, i) => {
      const doc = createXhtmlDoc('');
      const body = doc.querySelector('body');
      if (grp.headingText) {
        const h = doc.createElementNS(ns, 'h2');
        h.textContent = grp.headingText;
        body.appendChild(h);
      }
      for (const child of grp.children) renderFb2SingleBlock(child, doc, body, ctx);
      const chapterTitle = grp.headingText || meta.title;
      doc.querySelector('title').textContent = chapterTitle;
      chapters.push({ id: 'chap' + (i + 1), filename: `chapter${i + 1}.xhtml`, title: chapterTitle, xhtmlString: serializeXhtmlDoc(doc) });
    });
  } else if (chapterSections.length === 0) {
    const doc = createXhtmlDoc(meta.title);
    const body = doc.querySelector('body');
    for (const child of mainBody.children) renderFb2SingleBlock(child, doc, body, ctx);
    chapters.push({ id: 'chap1', filename: 'chapter1.xhtml', title: meta.title, xhtmlString: serializeXhtmlDoc(doc) });
  } else {
    chapterSections.forEach((entry, i) => {
      const doc = createXhtmlDoc('');
      const body = doc.querySelector('body');
      if (entry.mode === 'ownOnly') {
        renderFb2SectionOwnContent(entry.el, doc, body, ctx, 2);
      } else {
        renderFb2SectionContent(entry.el, doc, body, ctx, 2);
      }
      const chapterTitle = fb2SectionTitleText(entry.el, `Chapter ${i + 1}`);
      doc.querySelector('title').textContent = chapterTitle;
      chapters.push({ id: 'chap' + (i + 1), filename: `chapter${i + 1}.xhtml`, title: chapterTitle, xhtmlString: serializeXhtmlDoc(doc) });
    });
  }
  if (chapters.length === 0) throw new Error('No readable content found in FB2 body');

  // Footnotes/endnotes become one appendix chapter, anchored so the
  // in-text <a class="note-ref"> links generated above can jump to them.
  if (notesFilename) {
    const doc = createXhtmlDoc('Примечания');
    const body = doc.querySelector('body');
    const h = doc.createElementNS(ns, 'h2');
    h.textContent = 'Примечания';
    body.appendChild(h);
    extraBodies.forEach(b => fb2Children(b, 'section').forEach(sec => appendFb2Note(sec, doc, body, ctx)));
    chapters.push({ id: 'notes', filename: notesFilename, title: 'Примечания', xhtmlString: serializeXhtmlDoc(doc) });
    log(`Footnotes: ${noteIds.size} → notes.xhtml`, '', 'INFO');
  }
  if (progressCallback) progressCallback(60);

  // ---- Package as EPUB ----
  const out = new JSZip();
  out.file('mimetype', 'application/epub+zip', { compression: 'STORE' });
  out.file('META-INF/container.xml',
`<?xml version="1.0" encoding="UTF-8"?>
<container version="1.0" xmlns="urn:oasis:names:tc:opendocument:xmlns:container">
  <rootfiles>
    <rootfile full-path="OEBPS/content.opf" media-type="application/oebps-package+xml"/>
  </rootfiles>
</container>`, { compression: 'DEFLATE', createFolders: false });

  out.file('OEBPS/style.css', FB2_EPUB_STYLESHEET, { compression: 'DEFLATE', createFolders: false });

  for (const info of Object.values(imageMap)) {
    out.file('OEBPS/images/' + info.filename, base64ToBlob(info.base64, info.contentType), { compression: 'STORE', createFolders: false });
  }
  chapters.forEach(ch => out.file('OEBPS/text/' + ch.filename, ch.xhtmlString, { compression: 'DEFLATE', createFolders: false }));

  let hasCoverPage = false;
  if (meta.coverId && imageMap[meta.coverId]) {
    hasCoverPage = true;
    const coverDoc = createXhtmlDoc('Cover');
    const coverBody = coverDoc.querySelector('body');
    coverBody.setAttribute('class', 'cover');
    const img = coverDoc.createElementNS(ns, 'img');
    img.setAttribute('src', '../images/' + imageMap[meta.coverId].filename);
    img.setAttribute('alt', 'Cover');
    coverBody.appendChild(img);
    out.file('OEBPS/text/cover.xhtml', serializeXhtmlDoc(coverDoc), { compression: 'DEFLATE', createFolders: false });
  }

  const uid = 'urn:uuid:' + generateEpubUuid();
  const manifestItems = [];
  const spineItems = [];

  if (hasCoverPage) {
    manifestItems.push('<item id="cover-page" href="text/cover.xhtml" media-type="application/xhtml+xml"/>');
    spineItems.push('<itemref idref="cover-page"/>');
  }
  chapters.forEach(ch => {
    manifestItems.push(`<item id="${ch.id}" href="text/${ch.filename}" media-type="application/xhtml+xml"/>`);
    spineItems.push(`<itemref idref="${ch.id}"/>`);
  });
  for (const [id, info] of Object.entries(imageMap)) {
    const props = (id === meta.coverId) ? ' properties="cover-image"' : '';
    manifestItems.push(`<item id="img-${sanitizeXmlId(id)}" href="images/${info.filename}" media-type="${info.contentType}"${props}/>`);
  }
  manifestItems.push('<item id="ncx" href="toc.ncx" media-type="application/x-dtbncx+xml"/>');
  manifestItems.push('<item id="nav" href="nav.xhtml" media-type="application/xhtml+xml" properties="nav"/>');
  manifestItems.push('<item id="css" href="style.css" media-type="text/css"/>');

  const coverMetaTag = hasCoverPage ? `<meta name="cover" content="img-${sanitizeXmlId(meta.coverId)}"/>` : '';
  const opf =
`<?xml version="1.0" encoding="UTF-8"?>
<package xmlns="http://www.idpf.org/2007/opf" version="3.0" unique-identifier="BookId">
  <metadata xmlns:dc="http://purl.org/dc/elements/1.1/">
    <dc:identifier id="BookId">${uid}</dc:identifier>
    <dc:title>${escapeHtml(meta.title)}</dc:title>
    <dc:language>${escapeHtml(meta.lang)}</dc:language>
    ${meta.authors.map(a => `<dc:creator>${escapeHtml(a)}</dc:creator>`).join('\n    ')}
    ${meta.annotation ? `<dc:description>${escapeHtml(meta.annotation)}</dc:description>` : ''}
    <meta property="dcterms:modified">${new Date().toISOString().replace(/\.\d+Z$/, 'Z')}</meta>
    ${coverMetaTag}
  </metadata>
  <manifest>
    ${manifestItems.join('\n    ')}
  </manifest>
  <spine toc="ncx">
    ${spineItems.join('\n    ')}
  </spine>
</package>`;
  out.file('OEBPS/content.opf', opf, { compression: 'DEFLATE', createFolders: false });

  const tocChapters = chapters.filter(ch => !ch.navHidden);
  const navPoints = tocChapters.map((ch, i) =>
`    <navPoint id="navpoint-${i + 1}" playOrder="${i + 1}">
      <navLabel><text>${escapeHtml(ch.title)}</text></navLabel>
      <content src="text/${ch.filename}"/>
    </navPoint>`).join('\n');
  out.file('OEBPS/toc.ncx',
`<?xml version="1.0" encoding="UTF-8"?>
<ncx xmlns="http://www.daisy.org/z3986/2005/ncx/" version="2005-1">
  <head><meta name="dtb:uid" content="${uid}"/></head>
  <docTitle><text>${escapeHtml(meta.title)}</text></docTitle>
  <navMap>
${navPoints}
  </navMap>
</ncx>`, { compression: 'DEFLATE', createFolders: false });

  const navLis = tocChapters.map(ch => `      <li><a href="text/${ch.filename}">${escapeHtml(ch.title)}</a></li>`).join('\n');
  out.file('OEBPS/nav.xhtml',
`<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE html>
<html xmlns="http://www.w3.org/1999/xhtml" xmlns:epub="http://www.idpf.org/2007/ops">
<head><meta charset="utf-8"/><title>${escapeHtml(meta.title)}</title></head>
<body>
  <nav epub:type="toc" id="toc">
    <h1>Contents</h1>
    <ol>
${navLis}
    </ol>
  </nav>
</body>
</html>`, { compression: 'DEFLATE', createFolders: false });

  if (progressCallback) progressCallback(90);

  const blob = await out.generateAsync({ type: 'blob', mimeType: 'application/epub+zip' });

  log(`Chapters: ${chapters.length} | Images: ${Object.keys(imageMap).length}`, '', 'INFO');
  log(t('files.conversion_complete'), 'success', 'DONE');
  logSummary(originalSize, blob.size, (Date.now() - startTime) / 1000);
  if (progressCallback) progressCallback(100);

  return blob;
}

// --- Oversized-chapter splitting (runs as part of EPUB optimization) ---
//
// Some EPUBs (an entire short-story collection as one multi-megabyte
// chapter is a real example, not hypothetical) ship a single spine item
// too large for the device to lay out - it either hangs for minutes
// retrying or gets refused outright by the device's own size guard. An
// on-device splitter exists too, but splitting a multi-MB file needs to
// unpack the whole zip archive first, which has already failed silently
// on-device for exactly this kind of file (not enough free heap for a
// multi-MB decompression buffer on hardware with ~275KB of RAM total).
// Doing the same split here instead - at upload time, in the browser,
// where memory isn't a real constraint - means the device never has to
// attempt the risky part at all; it just receives a book that was never
// oversized to begin with.
const OVERSIZED_XHTML_BYTES = 700 * 1024; // above this, split into parts
const SPLIT_CHUNK_TARGET_BYTES = 250 * 1024;

// Collect every id="..." in a chunk of HTML text (used to know which
// split part a given anchor ended up in).
function collectHtmlIds(html) {
  const ids = new Set();
  const re = /\bid\s*=\s*["']([^"']+)["']/g;
  let m;
  while ((m = re.exec(html)) !== null) ids.add(m[1]);
  return ids;
}

function isVoidHtmlElement(name) {
  return ['area', 'base', 'br', 'col', 'embed', 'hr', 'img', 'input', 'link', 'meta', 'param', 'source',
    'track', 'wbr'].includes(name.toLowerCase());
}

// Walks `bodyInner` (the content between <body> and </body>) tracking tag
// nesting depth, and returns every byte offset where depth is back to 0
// (i.e. a point between two top-level elements/text runs) - the only
// positions it's safe to cut the HTML without splitting a tag in half or
// separating an element from its own closing tag. Comments, CDATA, and
// <script>/<style> content are treated as opaque (never scanned for tags
// inside them), matching how a real HTML parser would.
function collectDepthZeroBoundaries(bodyInner) {
  const boundaries = [];
  let depth = 0;
  let i = 0;
  const n = bodyInner.length;

  while (i < n) {
    const c = bodyInner[i];
    if (c === '<') {
      if (bodyInner.startsWith('<!--', i)) {
        const end = bodyInner.indexOf('-->', i + 4);
        i = end === -1 ? n : end + 3;
        if (depth === 0) boundaries.push(i);
        continue;
      }
      if (bodyInner.startsWith('<![CDATA[', i)) {
        const end = bodyInner.indexOf(']]>', i + 9);
        i = end === -1 ? n : end + 3;
        if (depth === 0) boundaries.push(i);
        continue;
      }
      if (bodyInner[i + 1] === '!' || bodyInner[i + 1] === '?') {
        const end = bodyInner.indexOf('>', i);
        i = end === -1 ? n : end + 1;
        if (depth === 0) boundaries.push(i);
        continue;
      }
      const isClose = bodyInner[i + 1] === '/';
      const tagStart = i + (isClose ? 2 : 1);
      const tagNameMatch = /^[a-zA-Z][a-zA-Z0-9:-]*/.exec(bodyInner.slice(tagStart));
      if (!tagNameMatch) { i++; continue; }
      const tagName = tagNameMatch[0];
      let j = tagStart + tagName.length;
      let inQuote = null;
      while (j < n) {
        const cj = bodyInner[j];
        if (inQuote) { if (cj === inQuote) inQuote = null; }
        else if (cj === '"' || cj === "'") { inQuote = cj; }
        else if (cj === '>') { break; }
        j++;
      }
      const tagEnd = j < n ? j + 1 : n;
      const selfClosing = bodyInner[j - 1] === '/';

      if (!isClose && !selfClosing && (tagName.toLowerCase() === 'script' || tagName.toLowerCase() === 'style')) {
        const closeTag = `</${tagName}>`;
        const closeIdx = bodyInner.toLowerCase().indexOf(closeTag.toLowerCase(), tagEnd);
        i = closeIdx === -1 ? n : closeIdx + closeTag.length;
        if (depth === 0) boundaries.push(i);
        continue;
      }

      if (isClose) depth = Math.max(0, depth - 1);
      else if (!selfClosing && !isVoidHtmlElement(tagName)) depth++;
      i = tagEnd;
      if (depth === 0) boundaries.push(i);
      continue;
    }
    i++;
  }
  return boundaries;
}

// Splits `bodyInner` into chunks, each as close to targetSize as possible
// without ever cutting through the middle of a tag - each returned chunk
// boundary sits exactly on one of collectDepthZeroBoundaries()'s offsets.
function splitHtmlBody(bodyInner, targetSize) {
  const boundaries = collectDepthZeroBoundaries(bodyInner);
  const chunks = [];
  let start = 0;
  let prevBoundary = 0;
  for (const b of boundaries) {
    if (b - start >= targetSize && prevBoundary > start) {
      chunks.push(bodyInner.slice(start, prevBoundary));
      start = prevBoundary;
    }
    prevBoundary = b;
  }
  chunks.push(bodyInner.slice(start));
  return chunks.filter(c => c.length > 0);
}

// Splits one oversized XHTML document's content into N self-contained
// XHTML documents (same <head>, a slice of the original <body> each).
// Returns null if the document doesn't look splittable (no <body>) or
// isn't actually big enough to need it.
function splitXhtmlDocument(originalHtml, baseName) {
  const bodyOpenMatch = /<body[^>]*>/i.exec(originalHtml);
  const bodyCloseMatch = /<\/body>/i.exec(originalHtml);
  if (!bodyOpenMatch || !bodyCloseMatch) return null;
  const bodyOpenTag = bodyOpenMatch[0];
  const bodyStart = bodyOpenMatch.index + bodyOpenTag.length;
  const bodyEnd = bodyCloseMatch.index;
  const head = originalHtml.slice(0, bodyStart);
  const tail = originalHtml.slice(bodyEnd);
  const bodyInner = originalHtml.slice(bodyStart, bodyEnd);

  const chunks = splitHtmlBody(bodyInner, SPLIT_CHUNK_TARGET_BYTES);
  if (chunks.length <= 1) return null;

  const parts = [];
  const idToPart = new Map();
  chunks.forEach((chunk, idx) => {
    const partName = `${baseName}_part${idx}.html`;
    parts.push({ name: partName, html: head + chunk + tail });
    for (const id of collectHtmlIds(chunk)) idToPart.set(id, idx);
  });
  return { parts, idToPart };
}

function escapeRegExpLiteral(s) {
  return s.replace(/[.*+?^${}()|[\]\\]/g, '\\$&');
}

// Replaces the single manifest <item> (and its one <itemref> in the
// spine) for the file being split with one item/itemref per part, in
// order, so reading order is preserved. Returns null if the manifest
// entry or spine reference couldn't be found (caller should then leave
// the file unsplit rather than produce a manifest that doesn't match
// what's actually in the zip).
function rewriteOpfForSplitFile(opfText, originalHref, originalDir, parts) {
  const itemRe = new RegExp(`<item\\b[^>]*href=["'][^"']*${escapeRegExpLiteral(originalHref)}["'][^>]*/>`, 'i');
  const itemMatch = itemRe.exec(opfText);
  if (!itemMatch) return null;
  const originalItemTag = itemMatch[0];
  const idMatch = /\bid=["']([^"']+)["']/.exec(originalItemTag);
  const mediaTypeMatch = /\bmedia-type=["']([^"']+)["']/.exec(originalItemTag);
  if (!idMatch || !mediaTypeMatch) return null;
  const originalId = idMatch[1];
  const mediaType = mediaTypeMatch[1];

  const newIds = parts.map((_, idx) => `${originalId}_part${idx}`);
  const newItems = parts.map((p, idx) =>
    `<item id="${newIds[idx]}" media-type="${mediaType}" href="${originalDir}${p.name}"/>`).join('');
  let out = opfText.replace(originalItemTag, newItems);

  const itemrefRe = new RegExp(`<itemref\\b[^>]*idref=["']${escapeRegExpLiteral(originalId)}["'][^>]*/>`, 'i');
  const itemrefMatch = itemrefRe.exec(out);
  if (!itemrefMatch) return null;
  out = out.replace(itemrefMatch[0], newIds.map(id => `<itemref idref="${id}"/>`).join(''));

  return { opfText: out, originalId, newIds };
}

// Rewrites every href="<originalHref>" or href="<originalHref>#<anchor>"
// found in `html` to point at whichever split part actually contains
// that anchor (an unrecognized or missing anchor falls back to part 0 -
// better to land at the top of the right file than at a broken link).
// Used both for the NCX/nav table of contents and for any other chapter
// that happens to link into the file that got split (e.g. a footnote
// backlink), not just the file's own manifest entry.
function rewriteLinksToSplitFile(html, originalHref, idToPart, partNames) {
  const escaped = escapeRegExpLiteral(originalHref);
  const re = new RegExp(`(["'])${escaped}(#[^"']*)?\\1`, 'g');
  return html.replace(re, (full, quote, fragment) => {
    if (!fragment) return `${quote}${partNames[0]}${quote}`;
    const anchor = fragment.slice(1);
    const partIdx = idToPart.has(anchor) ? idToPart.get(anchor) : 0;
    return `${quote}${partNames[partIdx]}${fragment}${quote}`;
  });
}

// Convert EPUB file - returns converted blob
async function convertEpubFile(file, progressCallback) {
  const startTime = Date.now();

  const originalSize = file.size;

  // Initialize logging
  clearLog();
  showLog();
  log(`<strong>${file.name}</strong> <span class="log-detail">(${formatBytes(originalSize)})</span>`, '', 'INFO');
  log(`Quality: ${JPEG_QUALITY}% | Overlap: ${OVERLAP_PERCENT}% | Rotation: ${HANDEDNESS === 'right' ? 'CW' : 'CCW'} | Grayscale: ${ENABLE_GRAYSCALE ? 'ON' : 'OFF'}`, '', 'INFO');

  const zip = await JSZip.loadAsync(file);
  const renamed = {};
  zip.forEach(p => {
    const l = p.toLowerCase();
    if (l.match(/\.(png|gif|webp|bmp|jpeg)$/)) {
      renamed[p] = p.replace(/\.(png|gif|webp|bmp|jpeg)$/i, '.jpg');
    }
  });

  const out = new JSZip();
  const entries = Object.entries(zip.files);
  const splitImages = {};
  const xhtmlFiles = {};
  let opfPath = null, opfContent = null;
  let mainIdentifier = null;

  // Write mimetype FIRST per EPUB OCF spec
  if (zip.files['mimetype']) {
    const mimetypeData = await zip.files['mimetype'].async('arraybuffer');
    out.file('mimetype', mimetypeData, { compression: 'STORE', createFolders: false });
  }

  // First pass: process images
  for (let i = 0; i < entries.length; i++) {
    if (operationCancelled) throw new Error('Cancelled by user');
    const [path, fileObj] = entries[i];
    if (fileObj.dir || path === 'mimetype') continue;
    const low = path.toLowerCase();

    if (low.match(/\.(png|gif|webp|bmp|jpg|jpeg)$/)) {
      const data = await fileObj.async('arraybuffer');
      const imageState = getImageState(path);

      let result;
      try {
        result = await processImage(data, imageState, path);
      } catch (imageError) {
        // Log error but continue with original image
        console.error(`Failed to process image ${path}:`, imageError);
        log(`Warning: Failed to process ${path.split('/').pop()}, using original`, 'warning', 'IMG-ERR');

        // Use original image data as fallback
        result = {
          parts: [{
            data: data,
            suffix: '',
            width: 0,
            height: 0,
            size: data.byteLength
          }],
          meta: {
            origW: 0,
            origH: 0,
            origSize: data.byteLength,
            wasSplit: false,
            rotated: false,
            finalW: 0,
            finalH: 0,
            finalSize: data.byteLength,
            imageState: imageState,
            processingError: true
          }
        };
      }

      const parts = result.parts;
      const meta = result.meta;

      const baseName = path.replace(/\.[^.]+$/, '');
      const newExt = '.jpg';

      // Log image processing
      const imgName = path.split('/').pop();
      const origFormat = path.split('.').pop();
      logImage(imgName, meta.origW, meta.origH, origFormat, meta.origSize, meta.finalW, meta.finalH, meta.finalSize, meta.wasSplit, meta.splitCount || 0, parts, meta.imageState || 0);

      if (parts.length === 1 && parts[0].suffix === '') {
        const newPath = renamed[path] || path.replace(/\.[^.]+$/, newExt);
        out.file(newPath, parts[0].data, { compression: 'STORE', createFolders: false });
      } else {
        // Store with full path for collision prevention, but also keep original filename
        const origName = path.split('/').pop();
        const origDir = path.includes('/') ? path.substring(0, path.lastIndexOf('/')) : '';

        // Key by full path to avoid collisions
        splitImages[path] = {
          origName: origName,
          origDir: origDir,
          parts: []
        };

        for (const part of parts) {
          const partName = baseName.split('/').pop() + part.suffix + newExt;
          const partPath = (path.includes('/') ? path.substring(0, path.lastIndexOf('/') + 1) : '') + partName;
          out.file(partPath, part.data, { compression: 'STORE', createFolders: false });
          // Store metadata for XHTML/OPF updates
          splitImages[path].parts.push({
            path: partPath,
            imgName: partName,
            id: baseName.split('/').pop() + part.suffix,
            suffix: part.suffix
          });
        }
      }
    } else if (low.match(/\.(xhtml|html|htm)$/)) {
      xhtmlFiles[path] = await safeReadText(fileObj);
    } else if (low.endsWith('.opf')) {
      opfPath = path;
      opfContent = await safeReadText(fileObj);
    }

    if (progressCallback) progressCallback((i / entries.length) * 60);
  }

  // Split any oversized XHTML chapter into several smaller files - see
  // this function's own comment block above for why. Runs right after
  // xhtmlFiles is fully populated and before anything else touches it,
  // so every later pass (image src rewrites, the DEFENSIVE_STYLE
  // injection, OPF/NCX handling) sees the split parts as ordinary
  // documents rather than needing its own special case for them.
  const splitFileInfo = {}; // originalPath -> { dir, parts, idToPart }
  for (const [xhtmlPath, content] of Object.entries(xhtmlFiles)) {
    if (content.length < OVERSIZED_XHTML_BYTES) continue;
    const dir = xhtmlPath.includes('/') ? xhtmlPath.substring(0, xhtmlPath.lastIndexOf('/') + 1) : '';
    const baseName = xhtmlPath.split('/').pop().replace(/\.[^.]+$/, '');
    const splitResult = splitXhtmlDocument(content, baseName);
    if (!splitResult) continue; // not splittable (no <body>) or not actually oversized once parsed

    delete xhtmlFiles[xhtmlPath];
    for (const part of splitResult.parts) {
      xhtmlFiles[dir + part.name] = part.html;
    }
    splitFileInfo[xhtmlPath] = { dir, parts: splitResult.parts, idToPart: splitResult.idToPart };
    log(`Split oversized chapter ${xhtmlPath.split('/').pop()} (${formatBytes(content.length)}) into ${splitResult.parts.length} parts`, '', 'INFO');
  }

  // Second pass: update XHTML using DOMParser
  for (const [xhtmlPath, content] of Object.entries(xhtmlFiles)) {
    if (operationCancelled) throw new Error('Cancelled by user');
    let t = content;
    const r = fixSvgCover(t);
    if (r.fixed) { t = r.c; logFix('SVG cover', xhtmlPath.split('/').pop()); }

    const r2 = fixSvgWrappedImages(t);
    if (r2.fixed) { t = r2.c; logFix(`SVG images (${r2.count})`, xhtmlPath.split('/').pop()); }

    // Use DOMParser for all img modifications: remove width/height and handle split images
    try {
      const whitespaceGuard = protectWhitespaceOnlyTextNodes(t);
      const parser = new DOMParser();
      const doc = parser.parseFromString(whitespaceGuard.content, 'application/xhtml+xml');
      const parseError = doc.querySelector('parsererror');

      if (!parseError) {
        let modified = false;

        // Remove width/height attributes from ALL img tags (dimensions may have changed)
        // This prevents inkMOD and other readers from using wrong dimensions
        const allImgElements = doc.querySelectorAll('img');
        for (const img of allImgElements) {
          if (img.hasAttribute('width')) { img.removeAttribute('width'); modified = true; }
          if (img.hasAttribute('height')) { img.removeAttribute('height'); modified = true; }

          const src = img.getAttribute('src');
          if (src) {
            const decodedSrc = decodeHref(src);
            const resolvedSrc = resolvePath(xhtmlPath, decodedSrc);

            const match = Object.entries(renamed).find(([oldPath]) => resolvedSrc === oldPath);

            if (match) {
              const [oldPath, newPath] = match;
              img.setAttribute('src', decodedSrc.replace(oldPath.split('/').pop(), newPath.split('/').pop()));
              modified = true;
            }
          }
        }

        // Handle split images with path collision prevention
        if (Object.keys(splitImages).length > 0) {
          // Get XHTML directory for resolving relative paths
          const xhtmlDir = xhtmlPath.includes('/') ? xhtmlPath.substring(0, xhtmlPath.lastIndexOf('/')) : '';
          const rootFolders = ['ops', 'oebps', 'epub', 'content'];

          for (const [fullPath, splitInfo] of Object.entries(splitImages)) {
            const origName = splitInfo.origName;
            const origDir = splitInfo.origDir;
            const parts = splitInfo.parts;
            const newName = origName.replace(/\.(png|gif|webp|bmp|jpeg)$/i, '.jpg');

            // Extract immediate parent directory for collision prevention
            const splitDirParts = origDir.split('/').filter(p => p);
            const lastDir = splitDirParts.length > 0 ? splitDirParts[splitDirParts.length - 1].toLowerCase() : null;
            const immediateParent = (lastDir && !rootFolders.includes(lastDir)) ? splitDirParts[splitDirParts.length - 1] : null;

            // Get XHTML's parent directory parts for relative path resolution
            const xhtmlDirParts = xhtmlDir.split('/').filter(p => p);

            // Find all img elements
            const allImgs = doc.querySelectorAll('img');
            const matchingImgs = [];

            for (const img of allImgs) {
              const src = img.getAttribute('src') || '';
              const srcParts = src.split('/').filter(p => p && p !== '..' && p !== '.');
              const srcName = srcParts.pop() || '';

              // Check filename match
              if (srcName !== origName && srcName !== newName) continue;

              // Path collision prevention with root folder handling
              if (immediateParent) {
                // Image is in a specific subfolder (not root like OPS/OEBPS)
                if (srcParts.length === 0) {
                  // src has no path - check if XHTML is in same folder as image
                  const xhtmlLastDir = xhtmlDirParts.length > 0 ? xhtmlDirParts[xhtmlDirParts.length - 1] : null;
                  if (xhtmlLastDir !== immediateParent) continue;
                } else {
                  // src has path - verify parent directory matches
                  if (srcParts[srcParts.length - 1] !== immediateParent) continue;
                }
              } else {
                // Image is in root folder (like OEBPS/cover.jpg)
                // Only match if src has NO subfolder path OR points to root folder
                if (srcParts.length > 0) {
                  const srcLastDir = srcParts[srcParts.length - 1].toLowerCase();
                  if (!rootFolders.includes(srcLastDir)) continue;
                }
              }

              matchingImgs.push(img);
            }

            // Process each matching img — Pro's strip+inject approach
            for (const img of matchingImgs) {
              const src = img.getAttribute('src') || '';

              // Part 1: update src in-place, strip original sizing
              img.setAttribute('src', src.replace(origName, parts[0].imgName).replace(newName, parts[0].imgName));

              if (parts.length > 1) {
                // Strip original width/height/class that were sized for the unsplit image
                img.removeAttribute('width');
                img.removeAttribute('height');
                img.removeAttribute('class');
                img.setAttribute('style', 'max-width:100%;height:auto');

                // Neutralize container height constraints that were sized for the original
                let container = img.parentElement;
                const safeContainers = ['div', 'p', 'figure', 'aside', 'section', 'body'];
                while (container && !safeContainers.includes(container.tagName.toLowerCase())) container = container.parentElement;
                const insertTarget = container || img.parentElement;
                // Strip constraining classes/styles from container — they were for the unsplit image
                if (insertTarget && insertTarget.tagName.toLowerCase() !== 'body') {
                  insertTarget.removeAttribute('class');
                  insertTarget.removeAttribute('style');
                }
                const insertParent = insertTarget.parentElement;
                const insertRef = insertTarget.nextSibling;
                const ns = doc.documentElement.namespaceURI || 'http://www.w3.org/1999/xhtml';

                // Insert new minimal wrappers for parts 2+ in reading order
                for (let pi = 1; pi < parts.length; pi++) {
                  const wrapper = doc.createElementNS(ns, 'div');
                  const newImg = doc.createElementNS(ns, 'img');
                  const partSrc = src.replace(origName, parts[pi].imgName).replace(newName, parts[pi].imgName);
                  newImg.setAttribute('src', partSrc);
                  newImg.setAttribute('alt', '');
                  newImg.setAttribute('style', 'max-width:100%;height:auto');
                  wrapper.appendChild(newImg);
                  if (insertRef) insertParent.insertBefore(wrapper, insertRef);
                  else insertParent.appendChild(wrapper);
                }
              }
              modified = true;
            }
          }
        }

        // Only serialize if we made changes
        if (modified) {
          t = whitespaceGuard.restore(safeSerialize(doc, whitespaceGuard.content));
        }
      } else if (Object.keys(renamed).length > 0) {
        // DOMParser couldn't parse this as strict application/xhtml+xml - very common
        // for epubs with HTML-style (non-self-closing) tags like <img src="...">
        // instead of <img src="..." />. Without this fallback, the image-renaming
        // pass above never ran for this file, so any <img src="..."> still points at
        // the pre-rename filename (e.g. "i_001.png") even though the actual image was
        // recompressed and saved as "i_001.jpg" - the reader then fails to find the
        // referenced file and simply shows nothing. Patch just the src attributes via
        // plain text replacement instead; width/height stripping and split-image
        // wrapping are skipped for these files, but the image itself still shows up.
        const beforeFallback = t;
        t = t.replace(/(<img\b[^>]*\bsrc\s*=\s*["'])([^"']+)(["'])/gi, (full, pre, src, post) => {
          const decodedSrc = decodeHref(src);
          const resolvedSrc = resolvePath(xhtmlPath, decodedSrc);
          const match = Object.entries(renamed).find(([oldPath]) => resolvedSrc === oldPath);
          if (!match) return full;
          const [oldPath, newPath] = match;
          return pre + decodedSrc.replace(oldPath.split('/').pop(), newPath.split('/').pop()) + post;
        });
        if (t !== beforeFallback) {
          logFix('img src (regex fallback)', xhtmlPath.split('/').pop());
        }
      }
    } catch (e) {
      console.warn('DOMParser error for', xhtmlPath, e.message);
    }

    // Repoint any link into a file that got split above (its own manifest
    // entry's links were already rewound to point at itself via the split,
    // but OTHER chapters/footnotes can also reference into a chapter that
    // used to be one file and is now several) - matched by basename rather
    // than full path, since that's how these href values are written in
    // practice (relative to the file's own directory, which for a book
    // this large is essentially always the same directory as the file
    // itself); a reference via a different relative path (e.g. into a
    // sibling subdirectory) wouldn't be caught by this.
    for (const [originalPath, info] of Object.entries(splitFileInfo)) {
      const originalBase = originalPath.split('/').pop();
      if (!t.includes(originalBase)) continue;
      const partNames = info.parts.map(p => p.name);
      t = rewriteLinksToSplitFile(t, originalBase, info.idToPart, partNames);
    }

    // Inject universal image constraint — prevents overflow on e-ink displays
    if (t.includes('</head>')) {
      t = t.replace('</head>', DEFENSIVE_STYLE + '</head>');
    }

    out.file(xhtmlPath, t, { compression: 'DEFLATE', compressionOptions: { level: 8 }, createFolders: false });
  }

  // Extract main identifier from OPF using DOMParser with regex fallback
  if (opfContent) {
    mainIdentifier = extractIdentifier(opfContent);
  }

  // Third pass: update OPF using fixOPF (DOMParser with regex fallback)
  if (opfContent) {
    let t = opfContent;
    for (const [o, n] of Object.entries(renamed)) {
      t = t.split(o.split('/').pop()).join(n.split('/').pop());
    }
    const opfDir = opfPath.includes('/') ? opfPath.substring(0, opfPath.lastIndexOf('/')) : '';
    t = fixOPF(t, opfContent, opfDir, splitImages);

    // Manifest/spine entries for a chapter that got split above - same
    // basename-matching caveat as the cross-link rewrite: this assumes
    // the split file sits in the same directory as (or a computable
    // relative path from) the OPF, which covers real-world EPUBs where
    // content generally lives alongside the .opf.
    const opfDirPrefix = opfDir ? opfDir + '/' : '';
    for (const [originalPath, info] of Object.entries(splitFileInfo)) {
      const originalBase = originalPath.split('/').pop();
      const relativeDir = info.dir.startsWith(opfDirPrefix) ? info.dir.slice(opfDirPrefix.length) : info.dir;
      const rewritten = rewriteOpfForSplitFile(t, originalBase, relativeDir, info.parts);
      if (rewritten) {
        t = rewritten.opfText;
        log(`OPF manifest/spine updated for split chapter ${originalBase} (${info.parts.length} parts)`, '', 'INFO');
      } else {
        log(`Could not update OPF for split chapter ${originalBase} - manifest entry not found as expected`, 'warning', 'WARN');
      }
    }

    if (t !== opfContent) logFix('OPF', 'manifest updated');
    out.file(opfPath, t, { compression: 'DEFLATE', compressionOptions: { level: 8 }, createFolders: false });
  }

  // Copy remaining files
  for (const [path, fileObj] of entries) {
    if (operationCancelled) throw new Error('Cancelled by user');
    if (fileObj.dir || path === 'mimetype') continue;
    const low = path.toLowerCase();
    if (low.match(/\.(png|gif|webp|bmp|jpg|jpeg)$/) || low.match(/\.(xhtml|html|htm)$/) || low.endsWith('.opf')) continue;

    let data = await fileObj.async('arraybuffer');
    if (low.endsWith('.css')) {
      let t = await safeReadText(fileObj);
      for (const [o, n] of Object.entries(renamed)) {
        t = t.split(o.split('/').pop()).join(n.split('/').pop());
      }
      data = new TextEncoder().encode(t);
    } else if (low.endsWith('.ncx')) {
      let t = await safeReadText(fileObj);
      for (const [o, n] of Object.entries(renamed)) {
        t = t.split(o.split('/').pop()).join(n.split('/').pop());
      }
      const oldT = t;
      t = syncNCXIdentifier(t, mainIdentifier);
      if (t !== oldT) logFix('NCX identifier', 'Synced with OPF');

      // Table-of-contents entries that used to point into a chapter that
      // got split above need the same repointing as any other cross-link -
      // otherwise every TOC entry for stories after the first would jump
      // to the wrong place (or nowhere) once that one big file became
      // several smaller ones.
      for (const [originalPath, info] of Object.entries(splitFileInfo)) {
        const originalBase = originalPath.split('/').pop();
        if (!t.includes(originalBase)) continue;
        const partNames = info.parts.map(p => p.name);
        const beforeNcxSplit = t;
        t = rewriteLinksToSplitFile(t, originalBase, info.idToPart, partNames);
        if (t !== beforeNcxSplit) logFix('NCX', `repointed links into split chapter ${originalBase}`);
      }

      data = new TextEncoder().encode(t);
    }
    out.file(path, data, { compression: 'DEFLATE', compressionOptions: { level: 8 }, createFolders: false });
  }

  if (progressCallback) progressCallback(100);

  // Generate final blob
  const newBlob = await out.generateAsync({ type: 'blob', mimeType: 'application/epub+zip' });
  const newSize = newBlob.size;
  const timeElapsed = (Date.now() - startTime) / 1000;

  // Log completion
  log(t('files.conversion_complete'), 'success', 'DONE');
  logSummary(originalSize, newSize, timeElapsed);

  // Auto-export only if NOT in batch mode (batch mode exports at the end)
  if (!isBatchMode && exportLogCheckbox && exportLogCheckbox.checked) {
    setTimeout(() => {
      exportLogToFile(null, false); // isBatch = false for single file
    }, 100);
  }

  return newBlob;
}

// Get WebSocket URL based on current page location
function getWsUrl() {
  const host = window.location.hostname;
  return `ws://${host}:${WS_PORT}/`;
}

// Upload file via WebSocket (faster, binary protocol)
// Remote subfolder creation for folder uploads. Cached per upload session
// (reset in uploadFile()) so multiple files sharing a subfolder only POST
// /mkdir once instead of once per file.
let folderExistsCache = new Set();

function joinRemotePath(base, seg) {
  return base === '/' ? '/' + seg : base.replace(/\/$/, '') + '/' + seg;
}

/**
 * Make sure every segment of `fullPath` exists on the device, creating any
 * missing folders one level at a time via /mkdir (mirrors the New Folder
 * modal's own API call — same endpoint, same request shape).
 *
 * This used to pre-check each segment via GET /api/files before deciding
 * whether to call /mkdir. That listing endpoint returns 200 OK even for a
 * path that doesn't exist yet (it just lists nothing), so the pre-check
 * always reported "exists" and /mkdir was never actually called — folders
 * were silently never created, and every file write into them then failed
 * on the device with "Failed to open file for writing". So: just call
 * /mkdir unconditionally for every not-yet-attempted segment, exactly like
 * clicking "New Folder" does. If a folder already exists, /mkdir failing on
 * that is harmless — we don't treat it as fatal here; the file write right
 * after this is the real test of whether the folder is usable, and any
 * genuine failure surfaces through the normal upload error/retry path.
 */
async function ensureRemoteFolderPath(fullPath) {
  if (!fullPath || fullPath === '/') return;
  const segments = fullPath.split('/').filter(Boolean);
  let current = '';
  for (const seg of segments) {
    const parent = current === '' ? '/' : current;
    current = joinRemotePath(parent, seg);
    if (folderExistsCache.has(current)) continue;

    let lastError = null;
    for (let attempt = 1; attempt <= 3; attempt++) {
      const formData = new FormData();
      formData.append('name', seg);
      formData.append('path', parent);
      try {
        const response = await fetch('/mkdir?_=' + Date.now(), {
          method: 'POST', body: formData, cache: 'no-store'
        });
        const text = await response.text();
        if (response.ok || (response.status === 400 && /already exists/i.test(text))) {
          folderExistsCache.add(current);
          lastError = null;
          break;
        }
        lastError = new Error(text || `mkdir HTTP ${response.status}`);
      } catch (e) {
        lastError = e;
      }
      await new Promise(r => setTimeout(r, 120 * attempt));
    }
    if (lastError) {
      throw new Error(`Failed to create folder ${current}: ${lastError.message || lastError}`);
    }
  }
}


function uploadTransportBasename(file) {
  const raw = String((file && file.name) || 'book').replace(/\\/g, '/');
  const slash = raw.lastIndexOf('/');
  return (slash >= 0 ? raw.slice(slash + 1) : raw) || 'book';
}

function uploadFileWebSocket(file, onProgress, onComplete, onError, targetPath) {
  return new Promise((resolve, reject) => {
    const ws = new WebSocket(getWsUrl());
    currentUploadWs = ws;
    let uploadStarted = false;
    let sendingChunks = false;
    let uploadComplete = false; // set only when DONE is received and resolve() called

    ws.binaryType = 'arraybuffer';

    ws.onopen = function() {
      const uploadName = uploadTransportBasename(file);
      console.log('[WS] Connected, starting upload:', uploadName);
      // Never send webkitRelativePath/folder components as the filename.
      // The directory is carried separately in targetPath.
      ws.send(`START:${uploadName}:${file.size}:${targetPath || currentPath}`);
    };

    ws.onmessage = async function(event) {
      const msg = event.data;
      console.log('[WS] Message:', msg);

      if (msg === 'READY') {
        uploadStarted = true;
        sendingChunks = true;

        // Small delay to let connection stabilize
        await new Promise(r => setTimeout(r, 50));

        try {
          // Send file in chunks
          const totalSize = file.size;
          let offset = 0;

          while (offset < totalSize && ws.readyState === WebSocket.OPEN) {
            const chunkSize = Math.min(WS_CHUNK_SIZE, totalSize - offset);
            const chunk = file.slice(offset, offset + chunkSize);
            const buffer = await chunk.arrayBuffer();

            // Wait for buffer to clear - more aggressive backpressure
            while (ws.bufferedAmount > WS_CHUNK_SIZE * 2 && ws.readyState === WebSocket.OPEN) {
              await new Promise(r => setTimeout(r, 5));
            }

            if (ws.readyState !== WebSocket.OPEN) {
              throw new Error('WebSocket closed during upload');
            }

            ws.send(buffer);
            offset += chunkSize;

            // Update local progress with real transfer progress
            // Server will confirm 100% with DONE message
            if (onProgress) {
              onProgress(offset, totalSize);
            }
          }

          sendingChunks = false;
          console.log('[WS] All chunks sent, waiting for DONE');
        } catch (err) {
          console.error('[WS] Error sending chunks:', err);
          sendingChunks = false;
          ws.close();
          reject(err);
        }
      } else if (msg.startsWith('PROGRESS:')) {
        // Server confirmed progress - log for debugging but don't update UI
        // (local progress is smoother, server progress causes jumping)
        console.log('[WS] Server progress:', msg);
      } else if (msg === 'DONE') {
        // Show 100% when server confirms completion
        if (onProgress) onProgress(file.size, file.size);
        uploadComplete = true;
        currentUploadWs = null;
        ws.close();
        if (onComplete) onComplete();
        resolve();
      } else if (msg.startsWith('ERROR:')) {
        const error = msg.substring(6);
        ws.close();
        if (onError) onError(error);
        reject(new Error(error));
      }
    };

    ws.onerror = function(event) {
      console.error('[WS] Error:', event);
      currentUploadWs = null;
      if (!uploadStarted) {
        reject(new Error('WebSocket connection failed'));
      } else if (!sendingChunks) {
        reject(new Error('WebSocket error during upload'));
      } else {
        // Error during chunk sending - reject with appropriate message
        reject(new Error('WebSocket error during file transfer'));
      }
    };

    ws.onclose = function(event) {
      console.log('[WS] Connection closed, code:', event.code, 'reason:', event.reason);
      // Reject for any close before upload was confirmed complete (covers both
      // mid-chunk-send closes and the "all chunks sent, waiting for DONE" window)
      if (!uploadComplete) {
        reject(new Error('WebSocket closed during upload'));
      }
    };
  });
}

// Upload file via HTTP (fallback method)
function uploadFileHTTP(file, onProgress, onComplete, onError, targetPath) {
  return new Promise((resolve, reject) => {
    const formData = new FormData();
    // Safari can serialize a directory-picked File using its relative path as
    // multipart filename. Pass the filename explicitly so folder components
    // never become part of the on-device filename.
    formData.append('file', file, uploadTransportBasename(file));

    const xhr = new XMLHttpRequest();
    currentUploadXhr = xhr;
    xhr.open('POST', '/upload?path=' + encodeURIComponent(targetPath || currentPath), true);

    xhr.upload.onprogress = function(e) {
      if (e.lengthComputable && onProgress) {
        onProgress(e.loaded, e.total);
      }
    };

    xhr.onload = function() {
      currentUploadXhr = null;
      if (xhr.status === 200) {
        if (onComplete) onComplete();
        resolve();
      } else {
        const error = xhr.responseText || 'Upload failed';
        if (onError) onError(error);
        reject(new Error(error));
      }
    };

    xhr.onerror = function() {
      currentUploadXhr = null;
      const error = 'Network error';
      if (onError) onError(error);
      reject(new Error(error));
    };

    xhr.onabort = function() {
      currentUploadXhr = null;
      reject(new Error('Upload aborted'));
    };

    xhr.send(formData);
  });
}

function uploadInternalBookArtifact(endpoint, file, remoteBookPath, extraQuery = '') {
  return new Promise((resolve, reject) => {
    const formData = new FormData();
    formData.append('file', file, 'book.bin');
    const xhr = new XMLHttpRequest();
    currentUploadXhr = xhr;
    xhr.open('POST', endpoint + '?bookPath=' + encodeURIComponent(remoteBookPath) + extraQuery, true);
    xhr.onload = () => {
      currentUploadXhr = null;
      if (xhr.status === 200) resolve();
      else reject(new Error(xhr.responseText || `Cache upload failed (${xhr.status})`));
    };
    xhr.onerror = () => {
      currentUploadXhr = null;
      reject(new Error('Prepared cache network error'));
    };
    xhr.onabort = () => {
      currentUploadXhr = null;
      reject(new Error('Upload aborted'));
    };
    xhr.send(formData);
  });
}

function uploadPreparedBookCache(file, remoteBookPath, fb2Mode = false) {
  return uploadInternalBookArtifact('/api/books/cache', file, remoteBookPath, fb2Mode ? '&fb2=1' : '');
}

function uploadPreparedFb2Package(file, remoteBookPath) {
  return uploadInternalBookArtifact('/api/books/fb2-package', file, remoteBookPath);
}

// Build the firmware's native BookMetadataCache in the browser. It is sent to
// the protected cache-install endpoint, never stored beside the user's book.
let bookPrepRequestId = 0;
function readEpubStructureInWorker(file, progressCallback) {
  return new Promise((resolve, reject) => {
    const worker = new Worker('/js/bookprep-worker.js');
    const id = ++bookPrepRequestId;
    worker.onmessage = event => {
      const message = event.data || {};
      if (message.id !== id) return;
      if (typeof message.progress === 'number') progressCallback && progressCallback(message.progress);
      if (message.error) {
        worker.terminate();
        reject(new Error(message.error));
      } else if (message.result) {
        worker.terminate();
        resolve(message.result);
      }
    };
    worker.onerror = event => {
      worker.terminate();
      reject(new Error(event.message || 'Book preparation worker failed'));
    };
    worker.postMessage({ id, file });
  });
}

async function buildBrowserBookCache(epubFile, progressCallback) {
  const prepared = await readEpubStructureInWorker(epubFile, progressCallback);
  const parser = new DOMParser();
  const clean = value => (value || '').replace(/\s+/g, ' ').trim();
  const localName = node => (node.localName || node.nodeName.split(':').pop()).toLowerCase();
  const children = (node, name) => Array.from(node.getElementsByTagName('*')).filter(n => localName(n) === name);
  const normalise = path => {
    const out = [];
    String(path || '').replace(/\\/g, '/').split('/').forEach(part => {
      if (!part || part === '.') return;
      if (part === '..') out.pop(); else out.push(part);
    });
    return out.join('/');
  };
  const dirname = path => path.includes('/') ? path.slice(0, path.lastIndexOf('/') + 1) : '';
  const decodeHref = href => {
    try { return decodeURIComponent(href); } catch (_) { return href; }
  };
  const xmlEntry = path => {
    const wanted = normalise(path);
    let xml = '';
    if (wanted === 'META-INF/container.xml') xml = prepared.containerXml;
    else if (wanted === prepared.opfPath) xml = prepared.opfXml;
    else if (wanted === prepared.navPath) xml = prepared.navXml;
    else if (wanted === prepared.ncxPath) xml = prepared.ncxXml;
    if (!xml) throw new Error('Missing EPUB entry: ' + path);
    return parser.parseFromString(xml, 'application/xml');
  };

  progressCallback && progressCallback(5);
  const container = xmlEntry('META-INF/container.xml');
  const rootfile = children(container, 'rootfile')[0];
  if (!rootfile) throw new Error('EPUB container has no rootfile');
  const opfPath = normalise(rootfile.getAttribute('full-path'));
  const opfBase = dirname(opfPath);
  const opf = xmlEntry(opfPath);
  const firstText = name => {
    const node = children(opf, name)[0];
    return node ? clean(node.textContent) : '';
  };

  const manifest = new Map();
  children(opf, 'item').forEach(item => {
    const id = item.getAttribute('id') || '';
    const href = normalise(opfBase + decodeHref(item.getAttribute('href') || ''));
    manifest.set(id, {
      id, href,
      mediaType: item.getAttribute('media-type') || '',
      properties: item.getAttribute('properties') || ''
    });
  });

  const spineNode = children(opf, 'spine')[0];
  const spines = [];
  if (spineNode) {
    Array.from(spineNode.children).filter(n => localName(n) === 'itemref').forEach(ref => {
      const item = manifest.get(ref.getAttribute('idref') || '');
      if (item && item.href) spines.push({ href: item.href, cumulativeSize: 0, tocIndex: -1 });
    });
  }
  if (!spines.length) throw new Error('EPUB has no readable spine');

  let coverHref = '';
  const coverMeta = children(opf, 'meta').find(n => (n.getAttribute('name') || '').toLowerCase() === 'cover');
  if (coverMeta) {
    const cover = manifest.get(coverMeta.getAttribute('content') || '');
    if (cover && cover.mediaType.startsWith('image/')) coverHref = cover.href;
  }
  if (!coverHref) {
    const cover = Array.from(manifest.values()).find(i => i.properties.split(/\s+/).includes('cover-image'));
    if (cover) coverHref = cover.href;
  }

  const toc = [];
  const addToc = (title, rawTarget, base, level) => {
    if (!rawTarget) return;
    const hash = rawTarget.indexOf('#');
    const rawPath = hash < 0 ? rawTarget : rawTarget.slice(0, hash);
    const href = normalise(base + decodeHref(rawPath));
    const anchor = hash < 0 ? '' : decodeHref(rawTarget.slice(hash + 1));
    const spineIndex = spines.findIndex(s => s.href === href);
    toc.push({ title: clean(title), href, anchor, level: Math.max(0, Math.min(255, level)), spineIndex });
  };

  const navItem = Array.from(manifest.values()).find(i => i.properties.split(/\s+/).includes('nav'));
  if (navItem && prepared.navXml && normalise(navItem.href) === prepared.navPath) {
    const nav = xmlEntry(navItem.href);
    const navRoot = children(nav, 'nav').find(n =>
      (n.getAttribute('epub:type') || n.getAttribute('type') || '').split(/\s+/).includes('toc')) || children(nav, 'nav')[0];
    if (navRoot) {
      const walkOl = (ol, depth) => Array.from(ol.children).filter(n => localName(n) === 'li').forEach(li => {
        const a = Array.from(li.children).find(n => localName(n) === 'a');
        if (a) addToc(a.textContent, a.getAttribute('href'), dirname(navItem.href), depth);
        Array.from(li.children).filter(n => localName(n) === 'ol').forEach(child => walkOl(child, depth + 1));
      });
      Array.from(navRoot.children).filter(n => localName(n) === 'ol').forEach(ol => walkOl(ol, 1));
    }
  } else {
    const ncxId = spineNode ? spineNode.getAttribute('toc') : '';
    const ncxItem = manifest.get(ncxId) || Array.from(manifest.values()).find(i => i.mediaType === 'application/x-dtbncx+xml');
    if (ncxItem && prepared.ncxXml && normalise(ncxItem.href) === prepared.ncxPath) {
      const ncx = xmlEntry(ncxItem.href);
      const walkPoint = (point, depth) => {
        const label = children(point, 'navlabel')[0];
        const content = Array.from(point.children).find(n => localName(n) === 'content');
        if (content) addToc(label ? label.textContent : '', content.getAttribute('src'), dirname(ncxItem.href), depth);
        Array.from(point.children).filter(n => localName(n) === 'navpoint').forEach(p => walkPoint(p, depth + 1));
      };
      const map = children(ncx, 'navmap')[0];
      if (map) Array.from(map.children).filter(n => localName(n) === 'navpoint').forEach(p => walkPoint(p, 1));
    }
  }

  let cumulative = 0;
  spines.forEach((spine, index) => {
    const size = Number(prepared.entrySizes[normalise(spine.href)]) || 0;
    cumulative = Math.min(0xffffffff, cumulative + size);
    spine.cumulativeSize = cumulative;
    const direct = toc.findIndex(t => t.spineIndex === index);
    spine.tocIndex = direct >= 0 ? direct : (index ? spines[index - 1].tocIndex : -1);
  });

  progressCallback && progressCallback(70);
  const enc = new TextEncoder();
  const bytes = [];
  const u8 = n => bytes.push(n & 255);
  const u16 = n => { u8(n); u8(n >>> 8); };
  const i16 = n => u16(n < 0 ? 0x10000 + n : n);
  const u32 = n => { u8(n); u8(n >>> 8); u8(n >>> 16); u8(n >>> 24); };
  const strBytes = value => enc.encode(value || '');
  const putString = value => { const b = strBytes(value); u32(b.length); for (const v of b) u8(v); };
  const metadata = {
    title: firstText('title') || epubFile.name.replace(/\.epub$/i, ''),
    author: firstText('creator'), language: firstText('language') || 'und',
    cover: coverHref, textRef: ''
  };
  const metadataSize = [metadata.title, metadata.author, metadata.language, metadata.cover, metadata.textRef]
    .reduce((sum, s) => sum + 4 + strBytes(s).length, 0);
  const lutOffset = 13 + metadataSize;
  const spineEntrySizes = spines.map(s => 4 + strBytes(s.href).length + 4 + 2);
  const tocEntrySizes = toc.map(t => 4 + strBytes(t.title).length + 4 + strBytes(t.href).length + 4 + strBytes(t.anchor).length + 1 + 2);
  const lutSize = (spines.length + toc.length) * 4;

  u32(0x425843ff); u8(7); u32(lutOffset); u16(spines.length); u16(toc.length);
  putString(metadata.title); putString(metadata.author); putString(metadata.language); putString(metadata.cover); putString(metadata.textRef);
  let offset = lutOffset + lutSize;
  spineEntrySizes.forEach(size => { u32(offset); offset += size; });
  tocEntrySizes.forEach(size => { u32(offset); offset += size; });
  spines.forEach(s => { putString(s.href); u32(s.cumulativeSize); i16(s.tocIndex); });
  toc.forEach(t => { putString(t.title); putString(t.href); putString(t.anchor); u8(t.level); i16(t.spineIndex); });
  progressCallback && progressCallback(100);
  return new File([new Uint8Array(bytes)], 'book.bin', { type: 'application/octet-stream' });
}

function uploadFile() {
  const fileInput = document.getElementById('fileInput');
  const files = folderPickedFiles ? folderPickedFiles.slice() : Array.from(fileInput.files);
  const convertEnabled = document.getElementById('convertBeforeUpload').checked;
  const prepareBookEl = document.getElementById('prepareBookBeforeUpload');
  const prepareBookEnabled = !!(prepareBookEl && prepareBookEl.checked);
  const fb2ToEpubEl = document.getElementById('fb2ToEpubCheckbox');
  const fb2ToEpubEnabled = !!(fb2ToEpubEl && fb2ToEpubEl.checked);
  const anyBookTransformEnabled = convertEnabled || prepareBookEnabled || fb2ToEpubEnabled;
  folderExistsCache = new Set(); // fresh per batch — subfolders may have changed since the last upload

  if (files.length === 0) {
    alert(t('files.please_select_file'));
    return;
  }

  // Prevent modal close during upload
  isUploadInProgress = true;
  uploadGeneration++;
  const myGeneration = uploadGeneration;
  document.getElementById('uploadModalClose').classList.add('disabled');

  const progressContainer = document.getElementById('progress-container');
  const progressFill = document.getElementById('progress-fill');
  const progressText = document.getElementById('progress-text');
  const uploadBtn = document.getElementById('uploadBtn');

  progressContainer.style.display = 'block';
  uploadBtn.disabled = true;

  let currentIndex = 0;
  const failedFiles = [];
  let useWebSocket = !IS_SAFARI; // Safari/macOS is more reliable with HTTP multipart

  // Check if we should use batch logging mode
  const epubFilesToConvert = files.filter(f => {
    const n = f.name.toLowerCase();
    return (n.endsWith('.epub') || isFb2Name(n)) && convertEnabled;
  });
  const useBatchLog = epubFilesToConvert.length > 1 && exportLogCheckbox && exportLogCheckbox.checked;

  // Start batch log mode if needed
  if (useBatchLog) {
    startBatchLog(epubFilesToConvert.length);
    showLog();
  }

  async function uploadNextFile() {
    if (currentIndex >= files.length) {
      // All files processed - show summary
      if (failedFiles.length === 0) {
        progressFill.style.backgroundColor = '#4caf50';
        progressText.textContent = t('files.all_uploads_complete');

        // Finalize batch log if in batch mode
        if (useBatchLog) {
          finalizeBatchLog();
          setTimeout(() => {
            window.location.reload();
          }, 2000);
        } else {
          setTimeout(() => {
            window.location.reload();
          }, 1000);
        }
      } else {
        progressFill.style.backgroundColor = '#e74c3c';
        const failedList = failedFiles.map(f => f.name).join(', ');
        progressText.textContent = `${files.length - failedFiles.length}/${files.length} uploaded. Failed: ${failedList}`;

        // Add upload errors to batch log
        if (useBatchLog) {
          failedFiles.forEach(ff => {
            logError(`Upload failed for ${ff.name}: ${ff.error}`);
          });
          finalizeBatchLog();
        }

        // Only show banner if THIS upload session had failures
        // Use local failedFiles, not the global shared variable
        if (failedFiles.length > 0) {
          // Accumulate failed uploads to global (don't replace)
          failedUploadsGlobal = failedUploadsGlobal.concat(failedFiles);
          // Clear flag and close modal, then show banner with retry options
          isUploadInProgress = false;
          document.getElementById('uploadModalClose').classList.remove('disabled');
          closeUploadModal();
          showFailedUploadsBanner();
        }
      }
      return;
    }

    let file = files[currentIndex];
    const originalFile = file;
    // Captured now: convertFb2ToEpub/convertEpubFile replace `file` with a
    // freshly-constructed File below, which does not carry webkitRelativePath.
    const relPath = originalFile.webkitRelativePath || '';
    const relDir = relPath ? relPath.slice(0, relPath.length - originalFile.name.length).replace(/\/+$/, '') : '';
    const targetPath = relDir ? joinRemotePath(currentPath, relDir) : currentPath;
    // Large transfers are deliberately sent over HTTP. ESP32 WebSocket upload
    // remains the fast path for small files, but Safari/large-file buffering can
    // outrun the device and stall before DONE. HTTP streaming is slower but far
    // more predictable for 10–100+ MB books and big folder batches.
    if (originalFile.size >= WS_LARGE_FILE_LIMIT) useWebSocket = false;
    // Reset progress bar instantly without transition when starting a new file
    progressFill.classList.add('no-transition');
    progressFill.style.width = '0%';
    progressFill.style.backgroundColor = '#27ae60';
    // Re-enable transition after a brief delay
    setTimeout(() => progressFill.classList.remove('no-transition'), 50);

    // Plain copy is deliberately zero-touch: no JSZip scan, no content
    // detection, no renaming and no browser-side book preparation. This keeps
    // large-file/folder uploads cheap and makes the bytes on SD identical to
    // the selected file. Content inspection is only needed when the user
    // explicitly enabled Optimize / Prepare / FB2→EPUB.
    if (anyBookTransformEnabled) {
      file = await extractEpubFromUpload(file);
    }
    const lowerFileName = file.name.toLowerCase();
    const isEpub = lowerFileName.endsWith('.epub');
    const isFb2 = anyBookTransformEnabled ? await isFb2UploadFile(file) : isFb2Name(lowerFileName);
    const isImage = isImageName(lowerFileName);

    if (anyBookTransformEnabled && isFb2 && lowerFileName.endsWith('.zip') && !isFb2ZipName(lowerFileName)) {
      const fb2ZipName = canonicalFb2ZipUploadName(file.name);
      if (fb2ZipName !== file.name) {
        file = new File([file], fb2ZipName, { type: file.type || 'application/zip' });
      }
    }
    // Web uploads prepare FB2 completely in the browser. Converting it to a
    // normal EPUB lets the browser also build the final native metadata cache;
    // books copied directly to SD keep the original FB2 lazy-open path.
    const prepareNativeFb2 = isFb2 && prepareBookEnabled && !fb2ToEpubEnabled;
    const convertFb2ToRealEpub = isFb2 && fb2ToEpubEnabled;

    // Transformation is opt-in. When all checkboxes are off, FB2/FB2.ZIP is
    // uploaded byte-for-byte; even the cover is left untouched.
    const needsConversion =
      ((isEpub || isImage || isFb2) && convertEnabled) ||
      convertFb2ToRealEpub || prepareNativeFb2;
    let conversionSucceeded = false;
    let conversionFailed = false;  // Track if conversion actually failed
    let preparedCompanion = null;
    let preparedFb2Package = null;
    let convOriginalSize = 0;      // Picked-file size; 0 unless conversion succeeded
    let convNewSize = 0;           // Generated blob size; 0 unless conversion succeeded

    const methodText = useWebSocket ? ' [WS]' : ' [HTTP]';
    const stageText = needsConversion ? 'Converting & uploading' : 'Copying';
    progressText.style.color = '';
    progressText.textContent = `${stageText} ${file.name} (${currentIndex + 1}/${files.length})${methodText}`;

    const onProgress = (loaded, total) => {
      const uploadPercent = Math.round((loaded / total) * 100);
      // If conversion succeeded, display goes from 50-100%, otherwise 0-100%
      const displayPercent = conversionSucceeded ? 50 + Math.round(uploadPercent / 2) : uploadPercent;
      progressFill.style.width = displayPercent + '%';
      const prefix = conversionSucceeded ? 'Converting & uploading' : 'Copying';
      progressText.textContent = `${prefix} ${file.name} (${currentIndex + 1}/${files.length})${methodText} — ${uploadPercent}%`;
    };

    const onComplete = () => {
      // Save file log to batch if in batch mode and this file was converted
      // Consider it successful only if conversion didn't fail
      if (useBatchLog && needsConversion) {
        const ok = !conversionFailed && conversionSucceeded;
        saveToFileBatchLog(file.name, ok, ok ? convOriginalSize : 0, ok ? convNewSize : 0);
      }

      currentIndex++;
      uploadNextFile();
    };

    const onError = (error) => {
      // Save failed file log to batch if in batch mode
      if (useBatchLog && needsConversion) {
        logError(`Upload failed: ${error}`);
        // Preserve conversion size totals when the convert step succeeded but
        // the upload failed; otherwise nothing was produced and 0/0 is correct.
        saveToFileBatchLog(file.name, false, convOriginalSize, convNewSize);
      }

      failedFiles.push({ name: file.name, error: error, file: originalFile });

      // If network error, mark all remaining files as failed and show retry banner
      if (error.includes('connection failed') || error.includes('Network error') ||
          error.includes('timeout') || error.includes('disconnected')) {
        console.log(`[Network] Network error detected: ${error}`);

        // Add all remaining files to failed list
        const remainingFiles = files.slice(currentIndex + 1);
        remainingFiles.forEach(remainingFile => {
          failedFiles.push({
            name: remainingFile.name,
            error: 'Network error - upload interrupted',
            file: remainingFile
          });
        });

        // Show retry banner immediately with all failed files
        failedUploadsGlobal = failedUploadsGlobal.concat(failedFiles);
        isUploadInProgress = false;
        document.getElementById('uploadModalClose').classList.remove('disabled');
        closeUploadModal();
        showFailedUploadsBanner();
        return; // Stop processing
      }

      currentIndex++;
      uploadNextFile();
    };

    try {
      // Convert EPUB if needed
      if (needsConversion) {
        progressFill.style.backgroundColor = '#9b59b6'; // Purple for conversion
        progressText.textContent = `Converting ${file.name} (${currentIndex + 1}/${files.length})...`;

        // Clear log for single file mode, or just add separator for batch mode
        if (!useBatchLog) {
          clearLog();
          showLog();
        }

        // Snapshot before the `file =` reassignment below, which swaps in the
        // converted blob and makes file.size point at the optimised size.
        const origFileSize = file.size;

        try {
          let converted;
          let imageResult = null;
          if (convertFb2ToRealEpub || prepareNativeFb2) {
            converted = await convertFb2ToEpub(file, (percent) => {
              progressFill.style.width = (percent * 0.5) + '%'; // Conversion takes first 50%
            }, convertEnabled);
          } else if (isFb2) {
            converted = await convertFb2File(file, (percent) => {
              progressFill.style.width = (percent * 0.5) + '%'; // Preparation takes first 50%
            }, convertEnabled);
          } else if (isEpub) {
            converted = await convertEpubFile(file, (percent) => {
              // Pass current quality setting to converter
              progressFill.style.width = (percent * 0.5) + '%'; // Conversion takes first 50%
            });
          } else if (isImage) {
            // Standalone image (not embedded in EPUB/FB2): resize to fit the
            // active device screen (MAX_WIDTH × MAX_HEIGHT), same pipeline as
            // EPUB/FB2 covers use.
            imageResult = await convertImageFile(file, (percent) => {
              progressFill.style.width = (percent * 0.5) + '%'; // Conversion takes first 50%
            });
            converted = imageResult.blob;
          }

          // convertFb2File resolves with a File already (possibly the original,
          // unchanged, if there was nothing to optimize); convertFb2ToEpub and
          // convertEpubFile resolve with a Blob that needs wrapping into a File.
          // A real FB2→EPUB conversion also gets a renamed .epub filename.
          // convertImageFile resolves with {blob, resized, width, height} — only
          // rename/rewrap when it actually re-encoded the image.
          if (prepareNativeFb2) {
            const preparedName = file.name.replace(/[._-]?fb2\.zip$/i, '.epub').replace(/\.fb2$/i, '.epub');
            preparedFb2Package = new File([converted], preparedName, { type: 'application/epub+zip' });
            // Deliberately keep `file` as the original FB2/FB2.ZIP. The EPUB
            // derivative is uploaded only to the hidden cache endpoint.
          } else if (convertFb2ToRealEpub) {
            const newName = file.name.replace(/[._-]?fb2\.zip$/i, '.epub').replace(/\.fb2$/i, '.epub');
            file = new File([converted], newName, { type: 'application/epub+zip' });
          } else if (isFb2) {
            file = converted;
          } else if (isImage) {
            if (imageResult.resized || converted !== file) {
              const newName = file.name.replace(/\.[^.\/]+$/, '.jpg');
              file = new File([converted], newName, { type: 'image/jpeg' });
              log(`Resized ${file.name}: ${imageResult.origWidth || imageResult.width}×${imageResult.origHeight || imageResult.height} → ${imageResult.width}×${imageResult.height}`, '', 'INFO');
            } else {
              log(`${file.name} already fits the screen — uploaded as-is`, '', 'INFO');
            }
          } else {
            file = new File([converted], file.name, { type: 'application/epub+zip' });
          }
          progressFill.style.backgroundColor = '#27ae60'; // Back to green for upload
          conversionSucceeded = true;
          convOriginalSize = origFileSize;
          convNewSize = converted.size;
        } catch (convError) {
          if (operationCancelled) { if (uploadGeneration === myGeneration) restoreAfterCancel(); return; }
          console.error('Conversion error:', convError);
          // Log the error
          logError(`Conversion failed: ${convError.message}`);
          log('Uploading original file instead...', 'warning', 'INFO');
          conversionFailed = true;

          // In single file mode, export error log
          if (!useBatchLog && exportLogCheckbox && exportLogCheckbox.checked) {
            setTimeout(() => {
              exportLogToFile(null, false); // isBatch = false for single file
            }, 100);
          }

          // If conversion fails, try uploading original file
          progressText.textContent = `Conversion failed, uploading original ${file.name}...`;
          progressFill.style.backgroundColor = '#e67e22'; // Orange for fallback
          // Reset progress bar to 0% for original file upload
          progressFill.style.width = '0%';
        }
      }

      if (relDir) {
        progressText.textContent = `Creating folder ${relDir}...`;
        await ensureRemoteFolderPath(targetPath);
      }

      // Always prepare EPUB metadata, independently of image optimization.
      // Failure is non-fatal: upload the book and let the existing device-side
      // first-open path build its cache.
      const cacheSourceFile = preparedFb2Package ||
        (file.name.toLowerCase().endsWith('.epub') ? file : null);
      if (prepareBookEnabled && cacheSourceFile) {
        try {
          progressText.textContent = `Preparing cache ${file.name}...`;
          preparedCompanion = await buildBrowserBookCache(cacheSourceFile, percent => {
            progressFill.style.width = Math.min(48, Math.round(percent * 0.48)) + '%';
          });
        } catch (cacheError) {
          preparedCompanion = null;
          console.warn('[Book cache] Browser preparation failed:', cacheError);
          log(`Browser cache skipped: ${cacheError.message}; device fallback remains available`, 'warning', 'INFO');
        }
      }

      if (useWebSocket) {
        await uploadFileWebSocket(file, onProgress, null, null, targetPath);
      } else {
        await uploadFileHTTP(file, onProgress, null, null, targetPath);
      }

      if (preparedCompanion) {
        progressText.textContent = `Uploading prepared cache for ${file.name}...`;
        const remoteBookPath = joinRemotePath(targetPath, file.name);
        if (preparedFb2Package) {
          await uploadPreparedFb2Package(preparedFb2Package, remoteBookPath);
        }
        await uploadPreparedBookCache(preparedCompanion, remoteBookPath, !!preparedFb2Package);
      }

      // Ensure progress bar shows 100% before moving to next file
      progressFill.style.width = '100%';
      progressText.textContent = `Upload complete: ${file.name}`;
      onComplete();
    } catch (error) {
      if (operationCancelled) { if (uploadGeneration === myGeneration) restoreAfterCancel(); return; }
      console.error('Upload error:', error);
      // Log upload error if conversion succeeded but upload failed
      if (conversionSucceeded) {
        logError(`Upload failed: ${error.message}`);
      }

      if (useWebSocket) {
        // Any WS failure (connect, mid-transfer, or waiting for DONE) gets one
        // clean HTTP retry. Previously only an initial connection failure fell
        // back, so a large file dying at 30–90% poisoned the whole folder batch.
        console.log('[Upload] WebSocket failed, retrying current file over HTTP:', error.message);
        useWebSocket = false;
        try {
          if (currentUploadWs && currentUploadWs.readyState <= WebSocket.OPEN) {
            try { currentUploadWs.close(); } catch (_) {}
          }
          currentUploadWs = null;
          await new Promise(r => setTimeout(r, 300)); // let ESP32 delete partial WS file
          progressFill.classList.add('no-transition');
          progressFill.style.width = '0%';
          setTimeout(() => progressFill.classList.remove('no-transition'), 30);
          progressText.textContent = `Retrying ${file.name} over HTTP...`;
          await uploadFileHTTP(file, onProgress, null, null, targetPath);
          onComplete();
        } catch (httpError) {
          onError(httpError.message);
        }
      } else {
        onError(error.message);
      }
    }
  }

  uploadNextFile();
}

function showFailedUploadsBanner() {
  const banner = document.getElementById('failedUploadsBanner');
  const filesList = document.getElementById('failedFilesList');

  filesList.innerHTML = '';

  failedUploadsGlobal.forEach((failedFile, index) => {
    const item = document.createElement('div');
    item.className = 'failed-file-item';
    item.innerHTML = `
      <div class="failed-file-info">
        <div class="failed-file-name">📄 ${escapeHtml(failedFile.name)}</div>
        <div class="failed-file-error">${escapeHtml(t('files.error_label'))} ${escapeHtml(failedFile.error)}</div>
      </div>
      <button class="retry-btn" onclick="retrySingleUpload(${index})">${escapeHtml(t('files.retry_btn'))}</button>
    `;
    filesList.appendChild(item);
  });

  // Ensure retry all button is visible
  const retryAllBtn = banner.querySelector('.retry-all-btn');
  if (retryAllBtn) retryAllBtn.style.display = '';

  banner.classList.add('show');
}

function dismissFailedUploads() {
  const banner = document.getElementById('failedUploadsBanner');
  banner.classList.remove('show');
  failedUploadsGlobal = [];
}

function retrySingleUpload(index) {
  const failedFile = failedUploadsGlobal[index];
  if (!failedFile) return;

  // Create a DataTransfer to set the file input
  const dt = new DataTransfer();
  dt.items.add(failedFile.file);

  const fileInput = document.getElementById('fileInput');
  fileInput.files = dt.files;

  // Remove this file from failed list
  failedUploadsGlobal.splice(index, 1);

  // If no more failed files, hide banner
  if (failedUploadsGlobal.length === 0) {
    dismissFailedUploads();
  }

  // Open modal and trigger upload
  openUploadModal();
  validateFile();
}

function retryAllFailedUploads() {
  if (failedUploadsGlobal.length === 0) return;

  // Preserve original File objects so Safari keeps webkitRelativePath on retry.
  folderPickedFiles = failedUploadsGlobal.map(failedFile => failedFile.file);
  try {
    const dt = new DataTransfer();
    folderPickedFiles.forEach(f => dt.items.add(f));
    document.getElementById('fileInput').files = dt.files;
  } catch (_) {}

  // Clear failed files list
  failedUploadsGlobal = [];
  dismissFailedUploads();

  // Open modal and trigger upload
  openUploadModal();
  validateFile();
}

  function createFolder() {
    const folderName = document.getElementById('folderName').value.trim();

    if (!folderName) {
      alert(t('files.please_enter_folder_name'));
      return;
    }

    // Validate folder name
    const validName = /^(?!\.{1,2}$)[^"*:<>?\/\\|]+$/.test(folderName);
    if (!validName) {
      alert(t('files.folder_name_invalid_chars'));
      return;
    }

    const formData = new FormData();
    formData.append('name', folderName);
    formData.append('path', currentPath);

    const xhr = new XMLHttpRequest();
    xhr.open('POST', '/mkdir', true);

    xhr.onload = function() {
      if (xhr.status === 200) {
        const u = new URL(window.location.href);
        u.searchParams.set('_', Date.now().toString());
        window.location.replace(u.toString());
      } else {
        alert(t('files.folder_create_failed', {msg: xhr.responseText}));
      }
    };

    xhr.onerror = function() {
      alert(t('files.folder_create_net_error'));
    };

    xhr.send(formData);
  }

  // Rename functions
  function openRenameModal(name, path, isFolder) {
    document.getElementById('renameItemName').textContent = (isFolder ? '📁 ' : '📄 ') + name;
    document.getElementById('renameItemPath').value = path;
    document.getElementById('renameNewName').value = name;
    document.getElementById('renameModal').classList.add('open');
    setTimeout(() => {
      const input = document.getElementById('renameNewName');
      input.focus();
      input.select();
    }, 50);
  }

  function closeRenameModal() {
    document.getElementById('renameModal').classList.remove('open');
  }

  function confirmRename() {
    const path = document.getElementById('renameItemPath').value;
    const newName = document.getElementById('renameNewName').value.trim();

    if (!newName) {
      alert(t('files.please_enter_new_name'));
      return;
    }
    if (newName.includes('/') || newName.includes('\\')) {
      alert(t('files.name_no_slashes'));
      return;
    }

    const formData = new FormData();
    formData.append('path', path);
    formData.append('name', newName);

    const xhr = new XMLHttpRequest();
    xhr.open('POST', '/rename', true);

    xhr.onload = function() {
      if (xhr.status === 200) {
        const u = new URL(window.location.href);
        u.searchParams.set('_', Date.now().toString());
        window.location.replace(u.toString());
      } else {
        alert(t('files.rename_failed', {msg: xhr.responseText}));
      }
      closeRenameModal();
    };

    xhr.onerror = function() {
      alert(t('files.rename_net_error'));
      closeRenameModal();
    };

    xhr.send(formData);
  }

  // Move functions
  function normalizePath(path) {
    if (!path) return '/';
    let normalized = path.trim();
    if (!normalized.startsWith('/')) normalized = '/' + normalized;
    if (normalized.length > 1 && normalized.endsWith('/')) {
      normalized = normalized.slice(0, -1);
    }
    return normalized;
  }

  function getParentPath(path) {
    const normalized = normalizePath(path);
    if (normalized === '/') return '/';
    const idx = normalized.lastIndexOf('/');
    return idx <= 0 ? '/' : normalized.slice(0, idx);
  }

  async function loadMoveFolderOptions() {
    const options = new Set();
    options.add('/');
    const parent = getParentPath(currentPath);
    if (parent) options.add(parent);

    async function fetchFolders(path) {
      try {
        const response = await fetch('/api/files?path=' + encodeURIComponent(path));
        if (!response.ok) return [];
        return await response.json();
      } catch (e) {
        return [];
      }
    }

    const rootFiles = await fetchFolders('/');
    rootFiles.forEach(file => {
      if (file.isDirectory) {
        options.add('/' + file.name);
      }
    });

    if (currentPath !== '/') {
      const currentFiles = await fetchFolders(currentPath);
      currentFiles.forEach(file => {
        if (file.isDirectory) {
          let folderPath = currentPath;
          if (!folderPath.endsWith('/')) folderPath += '/';
          folderPath += file.name;
          options.add(folderPath);
        }
      });
    }

    const dataList = document.getElementById('moveFolderOptions');
    dataList.innerHTML = '';
    Array.from(options).sort().forEach(path => {
      const option = document.createElement('option');
      option.value = path;
      dataList.appendChild(option);
    });
  }

  function openMoveModal(name, path) {
    document.getElementById('moveItemName').textContent = '📄 ' + name;
    document.getElementById('moveItemPath').value = path;
    document.getElementById('moveDestPath').value = currentPath === '/' ? '/' : currentPath;
    document.getElementById('moveModal').classList.add('open');
    loadMoveFolderOptions();
    setTimeout(() => {
      document.getElementById('moveDestPath').focus();
    }, 50);
  }

  function closeMoveModal() {
    document.getElementById('moveModal').classList.remove('open');
  }

  function confirmMove() {
    const path = document.getElementById('moveItemPath').value;
    const destPath = normalizePath(document.getElementById('moveDestPath').value);

    if (!destPath) {
      alert(t('files.please_enter_dest'));
      return;
    }

    const formData = new FormData();
    formData.append('path', path);
    formData.append('dest', destPath);

    const xhr = new XMLHttpRequest();
    xhr.open('POST', '/move', true);

    xhr.onload = function() {
      if (xhr.status === 200) {
        const u = new URL(window.location.href);
        u.searchParams.set('_', Date.now().toString());
        window.location.replace(u.toString());
      } else {
        alert(t('files.move_failed', {msg: xhr.responseText}));
      }
      closeMoveModal();
    };

    xhr.onerror = function() {
      alert(t('files.move_net_error'));
      closeMoveModal();
    };

    xhr.send(formData);
  }

  // Dictionary preparation -------------------------------------------------
  // The browser converts source dictionaries into two compact files:
  // dictionary.idx (sorted fixed-size hash records) and dictionary.dat
  // (headword/article payloads). The ESP32 only validates headers and performs
  // a binary search, avoiding expensive first-open parsing on the reader.
  const DICTIONARY_ROOT = '/.dictionaries';
  const DICTIONARY_INDEX_HEADER_SIZE = 16;
  const DICTIONARY_DATA_HEADER_SIZE = 12;
  const DICTIONARY_INDEX_RECORD_SIZE = 16;
  const DICTIONARY_MAX_HEADWORD_BYTES = 95;
  const DICTIONARY_MAX_NORMALIZED_BYTES = 127;
  const DICTIONARY_MAX_ARTICLE_BYTES = 8192;
  const DICTIONARY_MAX_SOURCE_ARTICLE_BYTES = 4 * 1024 * 1024;
  const DICTIONARY_DATA_CHUNK_SIZE = 256 * 1024;
  const DICTIONARY_INDEX_CHUNK_SIZE = 64 * 1024;
  const DICTIONARY_SORTED_CHUNK_SIZE = 1024 * 1024;
  const DICTIONARY_FNV_OFFSET = 2166136261;
  const DICTIONARY_FNV_PRIME = 16777619;
  const dictionaryEncoder = new TextEncoder();
  const dictionaryUtf8Decoder = new TextDecoder('utf-8');
  let dictionarySelectedFiles = [];
  let dictionaryBusy = false;

  function openDictionaryModal() {
    dictionaryBusy = false;
    dictionarySelectedFiles = [];
    document.getElementById('dictionaryFilesInput').value = '';
    document.getElementById('dictionaryFolderInput').value = '';
    document.getElementById('dictionarySelection').textContent = t('files.dictionary_none_selected');
    document.getElementById('prepareDictionaryBtn').disabled = true;
    document.getElementById('dictionaryProgress').classList.remove('visible');
    document.getElementById('dictionaryProgressFill').style.width = '0%';
    document.getElementById('dictionaryProgressText').textContent = '';
    document.getElementById('dictionaryModalClose').classList.remove('disabled');
    document.getElementById('dictionaryModal').classList.add('open');
  }

  function closeDictionaryModal() {
    if (dictionaryBusy) return;
    document.getElementById('dictionaryModal').classList.remove('open');
  }

  function handleDictionaryFilesPicked(folderMode) {
    const input = document.getElementById(folderMode ? 'dictionaryFolderInput' : 'dictionaryFilesInput');
    dictionarySelectedFiles = Array.from(input.files || []);
    document.getElementById(folderMode ? 'dictionaryFilesInput' : 'dictionaryFolderInput').value = '';
    document.getElementById('dictionarySelection').textContent = dictionarySelectedFiles.length
      ? t('files.dictionary_selected', {count: dictionarySelectedFiles.length})
      : t('files.dictionary_none_selected');
    document.getElementById('prepareDictionaryBtn').disabled = dictionarySelectedFiles.length === 0;
  }

  function setDictionaryProgress(percent, message) {
    const progress = document.getElementById('dictionaryProgress');
    progress.classList.add('visible');
    document.getElementById('dictionaryProgressFill').style.width = Math.max(0, Math.min(100, percent)) + '%';
    document.getElementById('dictionaryProgressText').textContent = message || '';
  }

  function setDictionaryBusy(busy) {
    dictionaryBusy = busy;
    document.getElementById('prepareDictionaryBtn').disabled = busy || dictionarySelectedFiles.length === 0;
    document.getElementById('cancelDictionaryBtn').disabled = busy;
    document.getElementById('dictionaryModalClose').classList.toggle('disabled', busy);
  }

  function dictionaryPathName(path) {
    return String(path || '').replace(/\\/g, '/').split('/').filter(Boolean).pop() || 'Dictionary';
  }

  function dictionaryBaseName(path) {
    return dictionaryPathName(path).replace(/\.(?:dict\.dz|idx\.gz|ifo|idx|dict|syn|dsl|tsv|csv|txt|zip|gz)$/i, '');
  }

  function dictionaryUtf8CodepointSize(codepoint) {
    if (codepoint <= 0x7f) return 1;
    if (codepoint <= 0x7ff) return 2;
    if (codepoint <= 0xffff) return 3;
    return 4;
  }

  function dictionaryTruncateUtf8(text, maxBytes) {
    let result = '';
    let bytes = 0;
    for (const ch of String(text || '')) {
      const size = dictionaryUtf8CodepointSize(ch.codePointAt(0));
      if (bytes + size > maxBytes) break;
      result += ch;
      bytes += size;
    }
    return result;
  }

  function dictionaryFoldCodepoint(cp) {
    if (cp >= 0x41 && cp <= 0x5a) return cp + 0x20;
    if ((cp >= 0x00c0 && cp <= 0x00d6) || (cp >= 0x00d8 && cp <= 0x00de)) return cp + 0x20;
    if (cp >= 0x0410 && cp <= 0x042f) return cp + 0x20;
    if (cp === 0x0401) return 0x0451;
    if (cp === 0x0404) return 0x0454;
    if (cp === 0x0406) return 0x0456;
    if (cp === 0x0407) return 0x0457;
    if (cp === 0x0490) return 0x0491;
    return cp;
  }

  function dictionaryIsLookupCodepoint(cp) {
    return (cp >= 0x30 && cp <= 0x39) || (cp >= 0x61 && cp <= 0x7a) ||
      (cp >= 0x00c0 && cp <= 0x02af) || (cp >= 0x0400 && cp <= 0x052f);
  }

  function normalizeDictionaryWord(input) {
    let output = '';
    let outputBytes = 0;
    let lastLookupLength = 0;
    let started = false;
    let pendingSpace = false;
    for (const rawChar of String(input || '').normalize('NFC')) {
      let cp = dictionaryFoldCodepoint(rawChar.codePointAt(0));
      if (cp === 0x20 || cp === 0x09 || cp === 0x0d || cp === 0x0a || cp === 0x00a0 || cp === 0x202f) {
        pendingSpace = started;
        continue;
      }
      const lookup = dictionaryIsLookupCodepoint(cp);
      const joiner = started && (cp === 0x2d || cp === 0x27 || cp === 0x2019);
      if (!lookup && !joiner) continue;
      if (pendingSpace && lookup) {
        if (outputBytes + 1 > DICTIONARY_MAX_NORMALIZED_BYTES) break;
        output += ' ';
        outputBytes += 1;
        pendingSpace = false;
      }
      const ch = String.fromCodePoint(cp);
      const encodedSize = dictionaryUtf8CodepointSize(cp);
      if (outputBytes + encodedSize > DICTIONARY_MAX_NORMALIZED_BYTES) break;
      output += ch;
      outputBytes += encodedSize;
      started = true;
      if (lookup) lastLookupLength = output.length;
    }
    return output.slice(0, lastLookupLength);
  }

  function dictionaryHash(normalized) {
    let hash = DICTIONARY_FNV_OFFSET >>> 0;
    for (const byte of dictionaryEncoder.encode(normalized)) {
      hash = Math.imul((hash ^ byte) >>> 0, DICTIONARY_FNV_PRIME) >>> 0;
    }
    return hash;
  }

  function dictionarySecondaryHash(normalized) {
    let hash = 5381 >>> 0;
    for (const byte of dictionaryEncoder.encode(normalized)) {
      hash = (Math.imul(hash, 33) ^ byte) >>> 0;
    }
    return hash;
  }

  function dictionaryDecodeText(bytes) {
    if (bytes.length >= 2 && bytes[0] === 0xff && bytes[1] === 0xfe) {
      return new TextDecoder('utf-16le').decode(bytes.subarray(2));
    }
    if (bytes.length >= 2 && bytes[0] === 0xfe && bytes[1] === 0xff) {
      const swapped = new Uint8Array(bytes.length - 2);
      for (let i = 2; i + 1 < bytes.length; i += 2) {
        swapped[i - 2] = bytes[i + 1];
        swapped[i - 1] = bytes[i];
      }
      return new TextDecoder('utf-16le').decode(swapped);
    }
    const utf8 = new TextDecoder('utf-8').decode(bytes);
    if (/^#SOURCE_CODE_PAGE\s+"(?:Cyrillic|Windows-1251)"/im.test(utf8) ||
        (utf8.match(/\ufffd/g) || []).length > Math.max(3, utf8.length / 200)) {
      try {
        return new TextDecoder('windows-1251').decode(bytes);
      } catch (e) {
        // UTF-8 remains the safe fallback on browsers without legacy decoders.
      }
    }
    return utf8.replace(/^\ufeff/, '');
  }

  function dictionaryDecodeEntities(text) {
    const decodeCodepoint = (value, radix) => {
      const codepoint = parseInt(value, radix);
      return Number.isInteger(codepoint) && codepoint >= 0 && codepoint <= 0x10ffff &&
        !(codepoint >= 0xd800 && codepoint <= 0xdfff) ? String.fromCodePoint(codepoint) : '';
    };
    return String(text || '')
      .replace(/&#x([0-9a-f]+);/gi, (_, hex) => decodeCodepoint(hex, 16))
      .replace(/&#([0-9]+);/g, (_, num) => decodeCodepoint(num, 10))
      .replace(/&nbsp;/gi, ' ')
      .replace(/&amp;/gi, '&')
      .replace(/&lt;/gi, '<')
      .replace(/&gt;/gi, '>')
      .replace(/&quot;/gi, '"')
      .replace(/&#39;|&apos;/gi, "'");
  }

  function dictionaryPlainArticle(text) {
    return dictionaryDecodeEntities(text)
      .replace(/\0/g, '')
      .replace(/<\s*br\s*\/?\s*>/gi, '\n')
      .replace(/<\s*\/\s*(?:p|div|li|h[1-6])\s*>/gi, '\n')
      .replace(/<[^>]*>/g, '')
      .replace(/\[(?:\/?(?:b|i|u|c|m|trn|ex|com|p|url|ref|sup|sub|lang|s)|[^\]]*=)[^\]]*\]/gi, '')
      .replace(/\{\{[\s\S]*?\}\}/g, '')
      .replace(/\r\n?/g, '\n')
      .replace(/[\t ]+\n/g, '\n')
      .replace(/[\t ]{2,}/g, ' ')
      .replace(/\n{3,}/g, '\n\n')
      .trim();
  }

  function dictionaryMakeSource(path, loader, streamFactory, size) {
    let bytesPromise = null;
    return {
      path: String(path || '').replace(/\\/g, '/'),
      name: dictionaryPathName(path),
      size: Math.max(0, Number(size) || 0),
      stream: typeof streamFactory === 'function' ? streamFactory : null,
      bytes: () => {
        if (!bytesPromise) bytesPromise = Promise.resolve(loader()).then(value => value instanceof Uint8Array ? value : new Uint8Array(value));
        return bytesPromise;
      }
    };
  }

  function dictionaryIsSupportedSource(path) {
    return /\.(?:ifo|idx|idx\.gz|dict|dict\.dz|syn|dsl|tsv|csv|txt)$/i.test(path || '');
  }

  async function dictionaryZipEntryStream(file, entry) {
    const localHeader = new Uint8Array(await file.slice(entry.localOffset, entry.localOffset + 30).arrayBuffer());
    if (localHeader.length !== 30 || new DataView(localHeader.buffer).getUint32(0, true) !== 0x04034b50) {
      throw new Error(t('files.dictionary_zip_unsupported'));
    }
    const localView = new DataView(localHeader.buffer);
    const dataOffset = entry.localOffset + 30 + localView.getUint16(26, true) + localView.getUint16(28, true);
    let stream = file.slice(dataOffset, dataOffset + entry.compressedSize).stream();
    if (entry.method === 0) return stream;
    if (entry.method !== 8 || typeof DecompressionStream === 'undefined') {
      throw new Error(t('files.dictionary_zip_unsupported'));
    }
    try {
      stream = stream.pipeThrough(new DecompressionStream('deflate-raw'));
    } catch (error) {
      throw new Error(t('files.dictionary_zip_unsupported'));
    }
    return stream;
  }

  async function dictionaryCollectZipSources(file) {
    // Read only the ZIP directory and lazily stream the selected entries. This
    // avoids JSZip retaining a phone-sized archive and its extracted files in RAM.
    const tailSize = Math.min(file.size, 65557);
    const tailOffset = file.size - tailSize;
    const tail = new Uint8Array(await file.slice(tailOffset).arrayBuffer());
    const tailView = new DataView(tail.buffer, tail.byteOffset, tail.byteLength);
    let eocd = -1;
    for (let offset = tail.length - 22; offset >= 0; --offset) {
      if (tailView.getUint32(offset, true) === 0x06054b50) {
        eocd = offset;
        break;
      }
    }
    if (eocd < 0) throw new Error(t('files.dictionary_zip_unsupported'));

    const entryCount = tailView.getUint16(eocd + 10, true);
    const directorySize = tailView.getUint32(eocd + 12, true);
    const directoryOffset = tailView.getUint32(eocd + 16, true);
    if (entryCount === 0xffff || directorySize === 0xffffffff || directoryOffset === 0xffffffff ||
        directoryOffset + directorySize > file.size) {
      throw new Error(t('files.dictionary_zip_unsupported'));
    }

    const directory = new Uint8Array(await file.slice(directoryOffset, directoryOffset + directorySize).arrayBuffer());
    const directoryView = new DataView(directory.buffer, directory.byteOffset, directory.byteLength);
    const sources = [];
    let cursor = 0;
    for (let index = 0; index < entryCount && cursor + 46 <= directory.length; ++index) {
      if (directoryView.getUint32(cursor, true) !== 0x02014b50) {
        throw new Error(t('files.dictionary_zip_unsupported'));
      }
      const flags = directoryView.getUint16(cursor + 8, true);
      const method = directoryView.getUint16(cursor + 10, true);
      const compressedSize = directoryView.getUint32(cursor + 20, true);
      const uncompressedSize = directoryView.getUint32(cursor + 24, true);
      const nameLength = directoryView.getUint16(cursor + 28, true);
      const extraLength = directoryView.getUint16(cursor + 30, true);
      const commentLength = directoryView.getUint16(cursor + 32, true);
      const localOffset = directoryView.getUint32(cursor + 42, true);
      const end = cursor + 46 + nameLength + extraLength + commentLength;
      if (end > directory.length || compressedSize === 0xffffffff || uncompressedSize === 0xffffffff ||
          localOffset === 0xffffffff) {
        throw new Error(t('files.dictionary_zip_unsupported'));
      }
      const path = dictionaryUtf8Decoder.decode(directory.subarray(cursor + 46, cursor + 46 + nameLength));
      if ((flags & 1) === 0 && !path.endsWith('/') && dictionaryIsSupportedSource(path)) {
        const entry = {method, compressedSize, uncompressedSize, localOffset};
        const streamFactory = () => dictionaryZipEntryStream(file, entry);
        sources.push(dictionaryMakeSource(path,
          async () => new Uint8Array(await new Response(await streamFactory()).arrayBuffer()),
          streamFactory, uncompressedSize));
      }
      cursor = end;
    }
    return sources;
  }

  async function dictionaryCollectSources(files) {
    const sources = [];
    for (const file of files) {
      const path = (file.webkitRelativePath || file.name || '').replace(/\\/g, '/');
      if (/\.zip$/i.test(path)) {
        sources.push(...await dictionaryCollectZipSources(file));
      } else if (dictionaryIsSupportedSource(path)) {
        sources.push(dictionaryMakeSource(path, () => file.arrayBuffer(), () => file.stream(), file.size));
      }
    }
    return sources;
  }

  async function dictionaryGunzip(bytes) {
    if (typeof DecompressionStream === 'undefined') {
      throw new Error(t('files.dictionary_gzip_unsupported'));
    }
    const stream = new Blob([bytes]).stream().pipeThrough(new DecompressionStream('gzip'));
    return new Uint8Array(await new Response(stream).arrayBuffer());
  }

  async function dictionarySourceBytes(source, compressed) {
    const bytes = await source.bytes();
    return compressed ? dictionaryGunzip(bytes) : bytes;
  }

  async function dictionarySourceStream(source, compressed) {
    let stream;
    if (source.stream) {
      stream = await source.stream();
    } else {
      stream = new Blob([await source.bytes()]).stream();
    }
    if (!compressed) return stream;
    if (typeof DecompressionStream === 'undefined') throw new Error(t('files.dictionary_gzip_unsupported'));
    return stream.pipeThrough(new DecompressionStream('gzip'));
  }

  function dictionaryParseIfo(text) {
    const info = {};
    for (const line of String(text || '').split(/\r?\n/)) {
      const separator = line.indexOf('=');
      if (separator > 0) info[line.slice(0, separator).trim().toLowerCase()] = line.slice(separator + 1).trim();
    }
    return info;
  }

  function dictionaryExtractStarDictText(raw, typeSequence) {
    const textTypes = new Set(['m', 'l', 'g', 'x', 'h', 't', 'y', 'k', 'w']);
    const fields = [];
    let cursor = 0;
    const sequence = String(typeSequence || '');

    function takeField(type, lastField) {
      let field;
      if (type >= 'A' && type <= 'Z') {
        if (cursor + 4 > raw.length) return false;
        const view = new DataView(raw.buffer, raw.byteOffset + cursor, 4);
        const length = view.getUint32(0, false);
        cursor += 4;
        if (cursor + length > raw.length) return false;
        field = raw.subarray(cursor, cursor + length);
        cursor += length;
      } else {
        let end = raw.indexOf(0, cursor);
        if (end < 0 || lastField) end = raw.length;
        field = raw.subarray(cursor, end);
        cursor = end < raw.length ? end + 1 : end;
      }
      if (textTypes.has(type.toLowerCase())) fields.push(dictionaryDecodeText(field));
      return true;
    }

    if (sequence) {
      for (let i = 0; i < sequence.length && cursor < raw.length; ++i) {
        if (!takeField(sequence[i], i + 1 === sequence.length)) break;
      }
    } else {
      while (cursor < raw.length) {
        const type = String.fromCharCode(raw[cursor++]);
        if (!/[A-Za-z]/.test(type) || !takeField(type, false)) break;
      }
    }
    if (fields.length === 0) fields.push(dictionaryDecodeText(raw));
    return dictionaryPlainArticle(fields.join('\n'));
  }

  async function dictionaryParseStarDict(group) {
    const ifo = dictionaryParseIfo(dictionaryDecodeText(await group.ifo.bytes()));
    if (String(ifo.idxoffsetbits || '32') === '64') throw new Error(t('files.dictionary_unsupported_64bit'));
    const canStream = typeof indexedDB !== 'undefined' && typeof BigUint64Array !== 'undefined' &&
      typeof BigInt !== 'undefined' && typeof ReadableStream !== 'undefined';
    if (canStream) {
      return {
        kind: 'stardict-stream',
        name: ifo.bookname || dictionaryBaseName(group.base),
        ifo,
        group,
        sourceEntryCount: Math.max(0, Number.parseInt(ifo.wordcount || '0', 10) || 0),
        synonymCount: Math.max(0, Number.parseInt(ifo.synwordcount || '0', 10) || 0)
      };
    }
    const sourceSize = (group.idx.size || 0) + (group.dict.size || 0) + (group.syn ? group.syn.size || 0 : 0);
    if (sourceSize > 64 * 1024 * 1024) throw new Error(t('files.dictionary_storage_unavailable'));
    const idxBytes = await dictionarySourceBytes(group.idx, /\.gz$/i.test(group.idx.path));
    const dictBytes = await dictionarySourceBytes(group.dict, /\.dz$/i.test(group.dict.path));
    const idxView = new DataView(idxBytes.buffer, idxBytes.byteOffset, idxBytes.byteLength);
    const entries = [];
    let cursor = 0;
    let sourceIndex = 0;
    while (cursor < idxBytes.length) {
      const wordStart = cursor;
      while (cursor < idxBytes.length && idxBytes[cursor] !== 0) ++cursor;
      if (cursor >= idxBytes.length) break;
      const headword = dictionaryDecodeText(idxBytes.subarray(wordStart, cursor));
      ++cursor;
      if (cursor + 8 > idxBytes.length) break;
      const offset = idxView.getUint32(cursor, false);
      const size = idxView.getUint32(cursor + 4, false);
      cursor += 8;
      if (size !== 0 && offset + size <= dictBytes.length) {
        const article = dictionaryExtractStarDictText(dictBytes.subarray(offset, offset + size), ifo.sametypesequence);
        if (headword && article) entries.push({headword, article, sourceIndex});
      }
      ++sourceIndex;
    }
    return {
      name: ifo.bookname || dictionaryBaseName(group.base),
      entries,
      sourceEntryCount: sourceIndex,
      synonymCount: Math.max(0, Number.parseInt(ifo.synwordcount || '0', 10) || 0),
      synonymSource: group.syn || null
    };
  }

  function dictionaryParseDsl(text, fallbackName) {
    const entries = [];
    let name = fallbackName;
    let headwords = [];
    let definition = [];

    function flush() {
      const article = dictionaryPlainArticle(definition.join('\n'));
      if (article) {
        for (const headword of headwords) entries.push({headword: headword.trim(), article});
      }
      headwords = [];
      definition = [];
    }

    for (const rawLine of String(text || '').split(/\r?\n/)) {
      const titleMatch = rawLine.match(/^#NAME\s+"([\s\S]*)"/i);
      if (titleMatch) {
        name = titleMatch[1].trim() || name;
        continue;
      }
      if (/^#/.test(rawLine) || /^\s*\/\//.test(rawLine)) continue;
      if (/^[\t ]/.test(rawLine)) {
        if (headwords.length) definition.push(rawLine.trim());
      } else if (rawLine.trim()) {
        if (definition.length) flush();
        headwords.push(rawLine.replace(/\\([{}|])/g, '$1').trim());
      }
    }
    flush();
    return {name, entries};
  }

  function dictionarySplitDelimitedLine(line, delimiter) {
    const fields = [];
    let value = '';
    let quoted = false;
    for (let i = 0; i < line.length; ++i) {
      const ch = line[i];
      if (ch === '"') {
        if (quoted && line[i + 1] === '"') {
          value += '"';
          ++i;
        } else {
          quoted = !quoted;
        }
      } else if (ch === delimiter && !quoted) {
        fields.push(value);
        value = '';
      } else {
        value += ch;
      }
    }
    fields.push(value);
    return fields;
  }

  function dictionaryParseDelimited(text, fallbackName, extension) {
    const entries = [];
    const lines = String(text || '').replace(/^\ufeff/, '').split(/\r?\n/);
    let delimiter = extension === 'tsv' ? '\t' : ',';
    const sample = lines.find(line => line.trim()) || '';
    if (sample.includes('\t')) delimiter = '\t';
    else if (!sample.includes(',') && sample.includes(';')) delimiter = ';';
    for (const line of lines) {
      if (!line.trim() || /^\s*#/.test(line)) continue;
      const fields = dictionarySplitDelimitedLine(line, delimiter);
      if (fields.length < 2) continue;
      const headword = fields.shift().trim();
      const article = dictionaryPlainArticle(fields.join(delimiter));
      if (headword && article) entries.push({headword, article});
    }
    return {name: fallbackName, entries};
  }

  async function dictionaryParseSources(sources) {
    const starDictGroups = new Map();
    const standalone = [];

    for (const source of sources) {
      const lower = source.path.toLowerCase();
      let base = '';
      let type = '';
      if (lower.endsWith('.dict.dz')) { base = lower.slice(0, -8); type = 'dict'; }
      else if (lower.endsWith('.idx.gz')) { base = lower.slice(0, -7); type = 'idx'; }
      else if (lower.endsWith('.ifo')) { base = lower.slice(0, -4); type = 'ifo'; }
      else if (lower.endsWith('.idx')) { base = lower.slice(0, -4); type = 'idx'; }
      else if (lower.endsWith('.dict')) { base = lower.slice(0, -5); type = 'dict'; }
      else if (lower.endsWith('.syn')) { base = lower.slice(0, -4); type = 'syn'; }
      if (type) {
        if (!starDictGroups.has(base)) starDictGroups.set(base, {base});
        const group = starDictGroups.get(base);
        if (!group[type] || (!/\.gz$|\.dz$/i.test(source.path) && /\.gz$|\.dz$/i.test(group[type].path))) {
          group[type] = source;
        }
      } else {
        standalone.push(source);
      }
    }

    const parsed = [];
    for (const group of starDictGroups.values()) {
      if (!group.ifo || !group.idx || !group.dict) continue;
      setDictionaryProgress(8, t('files.dictionary_preparing', {name: dictionaryBaseName(group.base)}));
      parsed.push(await dictionaryParseStarDict(group));
      await new Promise(resolve => setTimeout(resolve, 0));
    }
    for (const source of standalone) {
      const extension = source.path.toLowerCase().split('.').pop();
      setDictionaryProgress(8, t('files.dictionary_preparing', {name: source.name}));
      const text = dictionaryDecodeText(await source.bytes());
      let result;
      if (extension === 'dsl') {
        result = dictionaryParseDsl(text, dictionaryBaseName(source.path));
      } else {
        result = dictionaryParseDelimited(text, dictionaryBaseName(source.path), extension);
        if (extension === 'txt' && result.entries.length === 0) {
          result = dictionaryParseDsl(text, dictionaryBaseName(source.path));
        }
      }
      if (result.entries.length) parsed.push(result);
      await new Promise(resolve => setTimeout(resolve, 0));
    }
    return parsed;
  }

  function dictionarySanitizeName(name) {
    const clean = String(name || 'Dictionary')
      .replace(/[<>:"/\\|?*\x00-\x1f]/g, '_')
      .replace(/[. ]+$/g, '')
      .trim() || 'Dictionary';
    return dictionaryTruncateUtf8(clean, 50) || 'Dictionary';
  }

  function dictionaryUniqueName(name, usedNames) {
    const root = dictionarySanitizeName(name);
    let candidate = root;
    let suffix = 2;
    while (usedNames.has(candidate.toLocaleLowerCase())) {
      const ending = '-' + suffix++;
      candidate = dictionaryTruncateUtf8(root, 50 - dictionaryEncoder.encode(ending).length) + ending;
    }
    usedNames.add(candidate.toLocaleLowerCase());
    return candidate;
  }

  function dictionaryStorageError(error) {
    const name = String(error && error.name || '');
    if (name === 'QuotaExceededError' || name === 'NS_ERROR_DOM_QUOTA_REACHED') {
      return new Error(t('files.dictionary_storage_full'));
    }
    return error instanceof Error ? error : new Error(t('files.dictionary_storage_unavailable'));
  }

  async function dictionaryCleanupAbandonedStores() {
    if (typeof indexedDB === 'undefined' || typeof indexedDB.databases !== 'function') return;
    try {
      const databases = await indexedDB.databases();
      const staleNames = databases.map(database => database.name || '')
        .filter(name => name.startsWith('inkmod-dictionary-'));
      for (const name of staleNames) {
        await new Promise(resolve => {
          const request = indexedDB.deleteDatabase(name);
          request.onsuccess = request.onerror = request.onblocked = () => resolve();
        });
      }
    } catch (error) {
      // Browsers that do not allow database enumeration can still use a fresh store.
    }
  }

  class DictionaryChunkStore {
    constructor(name, database) {
      this.name = name;
      this.database = database;
    }

    static async open() {
      if (typeof indexedDB === 'undefined' || typeof IDBKeyRange === 'undefined') {
        throw new Error(t('files.dictionary_storage_unavailable'));
      }
      const name = 'inkmod-dictionary-' + Date.now() + '-' + Math.random().toString(16).slice(2);
      try {
        const database = await new Promise((resolve, reject) => {
          const request = indexedDB.open(name, 1);
          request.onupgradeneeded = () => request.result.createObjectStore('chunks');
          request.onsuccess = () => resolve(request.result);
          request.onerror = () => reject(request.error);
          request.onblocked = () => reject(new Error(t('files.dictionary_storage_unavailable')));
        });
        return new DictionaryChunkStore(name, database);
      } catch (error) {
        throw dictionaryStorageError(error);
      }
    }

    async put(key, blob) {
      try {
        await new Promise((resolve, reject) => {
          const transaction = this.database.transaction('chunks', 'readwrite');
          transaction.objectStore('chunks').put(blob, key);
          transaction.oncomplete = () => resolve();
          transaction.onerror = () => reject(transaction.error);
          transaction.onabort = () => reject(transaction.error);
        });
      } catch (error) {
        throw dictionaryStorageError(error);
      }
    }

    async getPrefix(prefix) {
      try {
        return await new Promise((resolve, reject) => {
          const values = [];
          const transaction = this.database.transaction('chunks', 'readonly');
          const range = IDBKeyRange.bound(prefix, prefix + '\uffff');
          const request = transaction.objectStore('chunks').openCursor(range);
          request.onsuccess = () => {
            const cursor = request.result;
            if (!cursor) return;
            values.push(cursor.value);
            cursor.continue();
          };
          request.onerror = () => reject(request.error);
          transaction.oncomplete = () => resolve(values);
          transaction.onerror = () => reject(transaction.error);
          transaction.onabort = () => reject(transaction.error);
        });
      } catch (error) {
        throw dictionaryStorageError(error);
      }
    }

    async deletePrefix(prefix) {
      try {
        await new Promise((resolve, reject) => {
          const transaction = this.database.transaction('chunks', 'readwrite');
          const range = IDBKeyRange.bound(prefix, prefix + '\uffff');
          const request = transaction.objectStore('chunks').openCursor(range);
          request.onsuccess = () => {
            const cursor = request.result;
            if (!cursor) return;
            cursor.delete();
            cursor.continue();
          };
          request.onerror = () => reject(request.error);
          transaction.oncomplete = () => resolve();
          transaction.onerror = () => reject(transaction.error);
          transaction.onabort = () => reject(transaction.error);
        });
      } catch (error) {
        throw dictionaryStorageError(error);
      }
    }

    async cleanup() {
      if (this.database) {
        this.database.close();
        this.database = null;
      }
      await new Promise(resolve => {
        const request = indexedDB.deleteDatabase(this.name);
        request.onsuccess = request.onerror = request.onblocked = () => resolve();
      });
    }
  }

  class DictionaryChunkWriter {
    constructor(store, prefix, chunkSize, initialPosition) {
      this.store = store;
      this.prefix = prefix;
      this.buffer = new Uint8Array(chunkSize);
      this.length = 0;
      this.sequence = 0;
      this.position = initialPosition || 0;
    }

    flush() {
      if (this.length === 0) return null;
      const key = this.prefix + (this.sequence++).toString(16).padStart(8, '0');
      const blob = new Blob([this.buffer.slice(0, this.length)], {type: 'application/octet-stream'});
      this.length = 0;
      return this.store.put(key, blob);
    }

    append(bytes) {
      let cursor = 0;
      let pending = null;
      while (cursor < bytes.length) {
        const copied = Math.min(bytes.length - cursor, this.buffer.length - this.length);
        this.buffer.set(bytes.subarray(cursor, cursor + copied), this.length);
        this.length += copied;
        this.position += copied;
        cursor += copied;
        if (this.length === this.buffer.length) pending = this.flush();
      }
      return pending;
    }

    async close() {
      const pending = this.flush();
      if (pending) await pending;
    }
  }

  class DictionaryBucketWriter extends DictionaryChunkWriter {
    constructor(store, bucket) {
      const bucketName = bucket.toString(16).padStart(2, '0');
      super(store, 'bucket:' + bucketName + ':', DICTIONARY_INDEX_CHUNK_SIZE, 0);
      this.recordCount = 0;
    }

    appendRecord(primaryHash, secondaryHash, dataOffset, dataLength) {
      let pending = null;
      if (this.buffer.length - this.length < DICTIONARY_INDEX_RECORD_SIZE) pending = this.flush();
      const view = new DataView(this.buffer.buffer, this.length, DICTIONARY_INDEX_RECORD_SIZE);
      view.setUint32(0, primaryHash, true);
      view.setUint32(4, secondaryHash, true);
      view.setUint32(8, dataOffset, true);
      view.setUint32(12, dataLength, true);
      this.length += DICTIONARY_INDEX_RECORD_SIZE;
      this.position += DICTIONARY_INDEX_RECORD_SIZE;
      ++this.recordCount;
      if (this.length === this.buffer.length) pending = this.flush();
      return pending;
    }
  }

  class DictionaryStreamReader {
    constructor(stream) {
      this.reader = stream.getReader();
      this.chunk = null;
      this.chunkOffset = 0;
      this.position = 0;
      this.finished = false;
    }

    async nextChunk() {
      while (!this.finished && (!this.chunk || this.chunkOffset >= this.chunk.length)) {
        const result = await this.reader.read();
        if (result.done) {
          this.finished = true;
          this.chunk = null;
          return false;
        }
        this.chunk = result.value instanceof Uint8Array ? result.value : new Uint8Array(result.value);
        this.chunkOffset = 0;
      }
      return !this.finished;
    }

    async readExact(size) {
      const output = new Uint8Array(size);
      let written = 0;
      while (written < size) {
        if (!await this.nextChunk()) throw new Error(t('files.dictionary_stream_error'));
        const copied = Math.min(size - written, this.chunk.length - this.chunkOffset);
        output.set(this.chunk.subarray(this.chunkOffset, this.chunkOffset + copied), written);
        this.chunkOffset += copied;
        this.position += copied;
        written += copied;
      }
      return output;
    }

    async skip(size) {
      let skipped = 0;
      while (skipped < size) {
        if (!await this.nextChunk()) throw new Error(t('files.dictionary_stream_error'));
        const copied = Math.min(size - skipped, this.chunk.length - this.chunkOffset);
        this.chunkOffset += copied;
        this.position += copied;
        skipped += copied;
      }
    }

    async cancel() {
      try {
        await this.reader.cancel();
      } catch (error) {
        // The useful part of the decompressed stream has already been consumed.
      }
    }
  }

  function dictionaryParseStarDictIndex(bytes) {
    let count = 0;
    let cursor = 0;
    while (cursor < bytes.length) {
      const wordEnd = bytes.indexOf(0, cursor);
      if (wordEnd < 0 || wordEnd + 9 > bytes.length) break;
      cursor = wordEnd + 9;
      ++count;
    }
    if (count === 0 || cursor !== bytes.length || count > 0xffffffff) {
      throw new Error(t('files.dictionary_stream_error'));
    }

    let wordStarts;
    let wordLengths;
    let dataOffsets;
    let dataLengths;
    let order;
    try {
      wordStarts = new Uint32Array(count);
      wordLengths = new Uint32Array(count);
      dataOffsets = new Uint32Array(count);
      dataLengths = new Uint32Array(count);
      order = new BigUint64Array(count);
    } catch (error) {
      throw new Error(t('files.dictionary_memory'));
    }

    const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
    cursor = 0;
    for (let index = 0; index < count; ++index) {
      const wordEnd = bytes.indexOf(0, cursor);
      wordStarts[index] = cursor;
      wordLengths[index] = wordEnd - cursor;
      const offset = view.getUint32(wordEnd + 1, false);
      wordLengths[index] = wordEnd - cursor;
      dataOffsets[index] = offset;
      dataLengths[index] = view.getUint32(wordEnd + 5, false);
      order[index] = (BigInt(offset) << 32n) | BigInt(index);
      cursor = wordEnd + 9;
    }
    order.sort();
    return {count, wordStarts, wordLengths, dataOffsets, dataLengths, order};
  }

  function dictionaryEncodeDataEntry(headword, article) {
    const headwordBytes = dictionaryEncoder.encode(headword);
    const articleBytes = dictionaryEncoder.encode(article);
    const bytes = new Uint8Array(6 + headwordBytes.length + articleBytes.length);
    const view = new DataView(bytes.buffer);
    view.setUint16(0, headwordBytes.length, true);
    view.setUint32(2, articleBytes.length, true);
    bytes.set(headwordBytes, 6);
    bytes.set(articleBytes, 6 + headwordBytes.length);
    return bytes;
  }

  function dictionaryAppendHashedRecord(bucketWriters, normalized, dataOffset, dataLength) {
    let primaryHash = DICTIONARY_FNV_OFFSET >>> 0;
    let secondaryHash = 5381 >>> 0;
    for (const byte of dictionaryEncoder.encode(normalized)) {
      primaryHash = Math.imul((primaryHash ^ byte) >>> 0, DICTIONARY_FNV_PRIME) >>> 0;
      secondaryHash = (Math.imul(secondaryHash, 33) ^ byte) >>> 0;
    }
    const writer = bucketWriters[primaryHash >>> 28];
    return writer.appendRecord(primaryHash, secondaryHash, dataOffset, dataLength);
  }

  async function dictionaryProcessStarDictSynonyms(parsed, sourceOffsets, sourceLengths, bucketWriters) {
    const source = parsed.group.syn;
    if (!source) return 0;
    const stream = await dictionarySourceStream(source, false);
    const reader = stream.getReader();
    let carry = new Uint8Array(0);
    let parsedCount = 0;
    let appendedCount = 0;
    let nextYield = 0x1ffff;
    try {
      while (true) {
        const result = await reader.read();
        if (result.done) break;
        const chunk = result.value instanceof Uint8Array ? result.value : new Uint8Array(result.value);
        let buffer;
        if (carry.length === 0) {
          buffer = chunk;
        } else {
          buffer = new Uint8Array(carry.length + chunk.length);
          buffer.set(carry);
          buffer.set(chunk, carry.length);
        }
        const view = new DataView(buffer.buffer, buffer.byteOffset, buffer.byteLength);
        let cursor = 0;
        while (cursor < buffer.length) {
          const wordEnd = buffer.indexOf(0, cursor);
          if (wordEnd < 0 || wordEnd + 5 > buffer.length) break;
          const sourceIndex = view.getUint32(wordEnd + 1, false);
          if (sourceIndex < sourceOffsets.length && sourceOffsets[sourceIndex] !== 0xffffffff) {
            const rawAlias = dictionaryUtf8Decoder.decode(buffer.subarray(cursor, wordEnd));
            const alias = rawAlias.replace(/^(?:\s*\([^)]*\))+\s*/, '').trim();
            const normalized = normalizeDictionaryWord(alias);
            if (normalized && !normalized.includes(' ')) {
              const pending = dictionaryAppendHashedRecord(bucketWriters, normalized,
                sourceOffsets[sourceIndex], sourceLengths[sourceIndex]);
              if (pending) await pending;
              ++appendedCount;
            }
          }
          cursor = wordEnd + 5;
          ++parsedCount;
          if (parsedCount >= nextYield) {
            const expected = parsed.synonymCount || parsedCount;
            setDictionaryProgress(28 + Math.min(1, parsedCount / expected) * 20,
              t('files.dictionary_preparing', {name: parsed.name}));
            nextYield += 0x20000;
            await new Promise(resolve => setTimeout(resolve, 0));
          }
        }
        carry = cursor < buffer.length ? buffer.slice(cursor) : new Uint8Array(0);
        if (carry.length > 1024 * 1024) throw new Error(t('files.dictionary_stream_error'));
      }
    } finally {
      try {
        await reader.cancel();
      } catch (error) {
        // Ignore cancellation after a complete input stream.
      }
    }
    return appendedCount;
  }

  async function dictionarySortIndexBucket(store, bucket) {
    const bucketName = bucket.toString(16).padStart(2, '0');
    const sourcePrefix = 'bucket:' + bucketName + ':';
    const blobs = await store.getPrefix(sourcePrefix);
    if (blobs.length === 0) return 0;
    let bytes;
    let keys;
    let sorted;
    try {
      bytes = new Uint8Array(await new Blob(blobs).arrayBuffer());
      if (bytes.length % DICTIONARY_INDEX_RECORD_SIZE !== 0) {
        throw new Error(t('files.dictionary_stream_error'));
      }
      const count = bytes.length / DICTIONARY_INDEX_RECORD_SIZE;
      keys = new BigUint64Array(count);
      const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
      for (let index = 0; index < count; ++index) {
        const primaryHash = view.getUint32(index * DICTIONARY_INDEX_RECORD_SIZE, true);
        keys[index] = (BigInt(primaryHash) << 32n) | BigInt(index);
      }
      keys.sort();
      sorted = new Uint8Array(bytes.length);
      let outputCount = 0;
      let previousPrimary = -1;
      const seenSecondary = new Set();
      for (let index = 0; index < count; ++index) {
        const sourceIndex = Number(keys[index] & 0xffffffffn);
        const sourceOffset = sourceIndex * DICTIONARY_INDEX_RECORD_SIZE;
        const primaryHash = view.getUint32(sourceOffset, true);
        if (primaryHash !== previousPrimary) {
          previousPrimary = primaryHash;
          seenSecondary.clear();
        }
        const secondaryHash = view.getUint32(sourceOffset + 4, true);
        if (seenSecondary.has(secondaryHash)) continue;
        seenSecondary.add(secondaryHash);
        sorted.set(bytes.subarray(sourceOffset, sourceOffset + DICTIONARY_INDEX_RECORD_SIZE),
          outputCount * DICTIONARY_INDEX_RECORD_SIZE);
        ++outputCount;
      }
      const outputLength = outputCount * DICTIONARY_INDEX_RECORD_SIZE;
      for (let offset = 0, sequence = 0; offset < outputLength;
           offset += DICTIONARY_SORTED_CHUNK_SIZE, ++sequence) {
        const key = 'sorted:' + bucketName + ':' + sequence.toString(16).padStart(8, '0');
        await store.put(key, new Blob([sorted.slice(offset, Math.min(outputLength,
          offset + DICTIONARY_SORTED_CHUNK_SIZE))],
          {type: 'application/octet-stream'}));
      }
      await store.deletePrefix(sourcePrefix);
      return outputCount;
    } catch (error) {
      throw dictionaryStorageError(error);
    }
  }

  function dictionaryIndexHeader(entryCount) {
    const bytes = new Uint8Array(DICTIONARY_INDEX_HEADER_SIZE);
    const view = new DataView(bytes.buffer);
    bytes.set(dictionaryEncoder.encode('IMDX'));
    view.setUint16(4, 2, true);
    view.setUint16(6, DICTIONARY_INDEX_RECORD_SIZE, true);
    view.setUint32(8, entryCount, true);
    return bytes;
  }

  function dictionaryDataHeader(entryCount) {
    const bytes = new Uint8Array(DICTIONARY_DATA_HEADER_SIZE);
    const view = new DataView(bytes.buffer);
    bytes.set(dictionaryEncoder.encode('IMDD'));
    view.setUint16(4, 2, true);
    view.setUint32(8, entryCount, true);
    return bytes;
  }

  async function dictionaryPrepareStarDictStreaming(parsed, usedNames) {
    const store = await DictionaryChunkStore.open();
    const dataWriter = new DictionaryChunkWriter(store, 'data:', DICTIONARY_DATA_CHUNK_SIZE,
      DICTIONARY_DATA_HEADER_SIZE);
    const bucketWriters = Array.from({length: 16}, (_, bucket) => new DictionaryBucketWriter(store, bucket));
    let streamReader = null;
    try {
      setDictionaryProgress(8, t('files.dictionary_preparing', {name: parsed.name}));
      const idxBytes = await dictionarySourceBytes(parsed.group.idx, /\.gz$/i.test(parsed.group.idx.path));
      const index = dictionaryParseStarDictIndex(idxBytes);
      const sourceOffsets = new Uint32Array(index.count);
      const sourceLengths = new Uint32Array(index.count);
      sourceOffsets.fill(0xffffffff);

      const dictStream = await dictionarySourceStream(parsed.group.dict, /\.dz$/i.test(parsed.group.dict.path));
      streamReader = new DictionaryStreamReader(dictStream);
      let canonicalCount = 0;
      let cachedOffset = 0;
      let cachedBytes = new Uint8Array(0);
      for (let sortedIndex = 0; sortedIndex < index.count;) {
        const sourceIndex = Number(index.order[sortedIndex] & 0xffffffffn);
        const dataOffset = index.dataOffsets[sourceIndex];
        let groupEnd = sortedIndex + 1;
        let maximumLength = index.dataLengths[sourceIndex];
        while (groupEnd < index.count) {
          const groupedSource = Number(index.order[groupEnd] & 0xffffffffn);
          if (index.dataOffsets[groupedSource] !== dataOffset) break;
          maximumLength = Math.max(maximumLength, index.dataLengths[groupedSource]);
          ++groupEnd;
        }

        let raw = null;
        if (dataOffset >= streamReader.position) {
          await streamReader.skip(dataOffset - streamReader.position);
          const readableLength = Math.min(maximumLength, DICTIONARY_MAX_SOURCE_ARTICLE_BYTES);
          raw = await streamReader.readExact(readableLength);
          await streamReader.skip(maximumLength - readableLength);
          cachedOffset = dataOffset;
          cachedBytes = raw;
        } else if (dataOffset >= cachedOffset && dataOffset + maximumLength <= cachedOffset + cachedBytes.length) {
          raw = cachedBytes.subarray(dataOffset - cachedOffset, dataOffset - cachedOffset + maximumLength);
        }

        if (raw) {
          for (let position = sortedIndex; position < groupEnd; ++position) {
            const currentSource = Number(index.order[position] & 0xffffffffn);
            const rawLength = index.dataLengths[currentSource];
            if (rawLength === 0 || rawLength > raw.length) continue;
            const sourceHeadword = dictionaryDecodeText(idxBytes.subarray(index.wordStarts[currentSource],
              index.wordStarts[currentSource] + index.wordLengths[currentSource])).trim().normalize('NFC');
            const headword = dictionaryTruncateUtf8(sourceHeadword, DICTIONARY_MAX_HEADWORD_BYTES);
            const normalized = normalizeDictionaryWord(headword);
            if (!normalized) continue;
            const article = dictionaryTruncateUtf8(
              dictionaryExtractStarDictText(raw.subarray(0, rawLength), parsed.ifo.sametypesequence),
              DICTIONARY_MAX_ARTICLE_BYTES);
            if (!article) continue;
            const entryBytes = dictionaryEncodeDataEntry(headword, article);
            if (dataWriter.position + entryBytes.length > 0xffffffff) {
              throw new Error(t('files.dictionary_too_large'));
            }
            const targetOffset = dataWriter.position;
            const pending = dataWriter.append(entryBytes);
            if (pending) await pending;
            sourceOffsets[currentSource] = targetOffset;
            sourceLengths[currentSource] = entryBytes.length;
            const indexPending = dictionaryAppendHashedRecord(bucketWriters, normalized, targetOffset,
              entryBytes.length);
            if (indexPending) await indexPending;
            ++canonicalCount;
          }
        }

        sortedIndex = groupEnd;
        if ((sortedIndex & 0x1fff) === 0 || sortedIndex === index.count) {
          setDictionaryProgress(8 + (sortedIndex / index.count) * 20,
            t('files.dictionary_preparing', {name: parsed.name}));
          await new Promise(resolve => setTimeout(resolve, 0));
        }
      }
      await streamReader.cancel();
      streamReader = null;
      if (canonicalCount === 0) throw new Error(t('files.dictionary_no_sources'));

      const synonymCount = await dictionaryProcessStarDictSynonyms(parsed, sourceOffsets, sourceLengths,
        bucketWriters);
      await dataWriter.close();
      await Promise.all(bucketWriters.map(writer => writer.close()));
      const sourceRecordCount = canonicalCount + synonymCount;
      if (sourceRecordCount > 0xffffffff) throw new Error(t('files.dictionary_too_large'));

      let sortedRecords = 0;
      for (let bucket = 0; bucket < 16; ++bucket) {
        setDictionaryProgress(48 + bucket / 16 * 20,
          t('files.dictionary_preparing', {name: parsed.name}));
        sortedRecords += await dictionarySortIndexBucket(store, bucket);
        await new Promise(resolve => setTimeout(resolve, 0));
      }
      if (sortedRecords === 0 || sortedRecords > sourceRecordCount) {
        throw new Error(t('files.dictionary_stream_error'));
      }

      const indexBlobs = await store.getPrefix('sorted:');
      const dataBlobs = await store.getPrefix('data:');
      const name = dictionaryUniqueName(parsed.name, usedNames);
      return {
        name,
        entryCount: sortedRecords,
        files: [
          new File([dictionaryIndexHeader(sortedRecords), ...indexBlobs], 'dictionary.idx',
            {type: 'application/octet-stream'}),
          new File([dictionaryDataHeader(sortedRecords), ...dataBlobs], 'dictionary.dat',
            {type: 'application/octet-stream'})
        ],
        cleanup: () => store.cleanup()
      };
    } catch (error) {
      if (streamReader) await streamReader.cancel();
      await store.cleanup();
      throw dictionaryStorageError(error);
    }
  }

  function dictionaryCountSynonymRecords(bytes) {
    let count = 0;
    let cursor = 0;
    while (cursor < bytes.length) {
      const end = bytes.indexOf(0, cursor);
      if (end < 0 || end + 5 > bytes.length) break;
      cursor = end + 5;
      ++count;
    }
    return count;
  }

  async function dictionaryPrepareBinary(parsed, usedNames) {
    if (parsed.kind === 'stardict-stream') {
      return dictionaryPrepareStarDictStreaming(parsed, usedNames);
    }
    const uniqueByNormalized = new Map();
    const entries = [];
    const sourceEntryCount = Math.max(0, parsed.sourceEntryCount || 0);
    const sourceToEntry = sourceEntryCount ? new Uint32Array(sourceEntryCount) : null;
    if (sourceToEntry) sourceToEntry.fill(0xffffffff);

    for (const sourceEntry of parsed.entries) {
      const sourceHeadword = String(sourceEntry.headword || '').trim().normalize('NFC');
      const headword = dictionaryTruncateUtf8(sourceHeadword, DICTIONARY_MAX_HEADWORD_BYTES);
      const normalized = normalizeDictionaryWord(headword);
      if (!normalized) continue;

      let entryIndex = uniqueByNormalized.get(normalized);
      if (entryIndex === undefined) {
        const article = dictionaryTruncateUtf8(dictionaryPlainArticle(sourceEntry.article), DICTIONARY_MAX_ARTICLE_BYTES);
        if (!headword || !article) continue;
        entryIndex = entries.length;
        uniqueByNormalized.set(normalized, entryIndex);
        entries.push({headword, normalized, article});
      }
      if (sourceToEntry && Number.isInteger(sourceEntry.sourceIndex) && sourceEntry.sourceIndex >= 0 &&
          sourceEntry.sourceIndex < sourceToEntry.length) {
        sourceToEntry[sourceEntry.sourceIndex] = entryIndex;
      }
    }
    if (!entries.length) return null;

    const encoded = entries.map(entry => ({
      ...entry,
      headwordBytes: dictionaryEncoder.encode(entry.headword),
      articleBytes: dictionaryEncoder.encode(entry.article)
    }));
    let dataSize = DICTIONARY_DATA_HEADER_SIZE;
    for (const entry of encoded) dataSize += 6 + entry.headwordBytes.length + entry.articleBytes.length;
    if (dataSize > 0xffffffff) throw new Error(t('files.dictionary_too_large'));

    const dataBytes = new Uint8Array(dataSize);
    const dataView = new DataView(dataBytes.buffer);
    dataBytes.set(dictionaryEncoder.encode('IMDD'), 0);
    dataView.setUint16(4, 2, true);
    dataView.setUint16(6, 0, true);
    let dataOffset = DICTIONARY_DATA_HEADER_SIZE;
    for (const entry of encoded) {
      entry.dataOffset = dataOffset;
      entry.dataLength = 6 + entry.headwordBytes.length + entry.articleBytes.length;
      dataView.setUint16(dataOffset, entry.headwordBytes.length, true);
      dataView.setUint32(dataOffset + 2, entry.articleBytes.length, true);
      dataBytes.set(entry.headwordBytes, dataOffset + 6);
      dataBytes.set(entry.articleBytes, dataOffset + 6 + entry.headwordBytes.length);
      dataOffset += entry.dataLength;
    }

    let synonymBytes = null;
    let synonymCapacity = 0;
    if (parsed.synonymSource && sourceToEntry) {
      synonymBytes = await parsed.synonymSource.bytes();
      synonymCapacity = Math.max(0, parsed.synonymCount || 0) || dictionaryCountSynonymRecords(synonymBytes);
    }
    const recordCapacity = encoded.length + synonymCapacity;
    const maximumIndexSize = DICTIONARY_INDEX_HEADER_SIZE + recordCapacity * DICTIONARY_INDEX_RECORD_SIZE;
    if (recordCapacity > 0xffffffff || maximumIndexSize > 0xffffffff) {
      throw new Error(t('files.dictionary_too_large'));
    }
    if (typeof BigUint64Array === 'undefined' || typeof BigInt === 'undefined') {
      throw new Error(t('files.dictionary_browser_too_old'));
    }

    let sortKeys;
    let secondaryHashes;
    let targetOffsets;
    let targetLengths;
    try {
      sortKeys = new BigUint64Array(recordCapacity);
      secondaryHashes = new Uint32Array(recordCapacity);
      targetOffsets = new Uint32Array(recordCapacity);
      targetLengths = new Uint32Array(recordCapacity);
    } catch (error) {
      throw new Error(t('files.dictionary_memory'));
    }

    let recordCount = 0;
    const appendIndexRecord = (normalized, target) => {
      if (recordCount >= recordCapacity) return false;
      const ordinal = recordCount;
      const hash = dictionaryHash(normalized);
      sortKeys[ordinal] = (BigInt(hash) << 32n) | BigInt(ordinal);
      secondaryHashes[ordinal] = dictionarySecondaryHash(normalized);
      targetOffsets[ordinal] = target.dataOffset;
      targetLengths[ordinal] = target.dataLength;
      ++recordCount;
      return true;
    };

    for (const entry of encoded) appendIndexRecord(entry.normalized, entry);

    if (synonymBytes) {
      const synonymView = new DataView(synonymBytes.buffer, synonymBytes.byteOffset, synonymBytes.byteLength);
      let cursor = 0;
      let parsedSynonyms = 0;
      while (cursor < synonymBytes.length && parsedSynonyms < synonymCapacity) {
        const wordEnd = synonymBytes.indexOf(0, cursor);
        if (wordEnd < 0 || wordEnd + 5 > synonymBytes.length) break;
        const sourceIndex = synonymView.getUint32(wordEnd + 1, false);
        if (sourceIndex < sourceToEntry.length) {
          const entryIndex = sourceToEntry[sourceIndex];
          if (entryIndex !== 0xffffffff) {
            const rawAlias = dictionaryUtf8Decoder.decode(synonymBytes.subarray(cursor, wordEnd));
            const alias = rawAlias.replace(/^(?:\s*\([^)]*\))+\s*/, '').trim();
            const normalized = normalizeDictionaryWord(alias);
            if (normalized && !normalized.includes(' ') && normalized !== encoded[entryIndex].normalized) {
              appendIndexRecord(normalized, encoded[entryIndex]);
            }
          }
        }
        cursor = wordEnd + 5;
        ++parsedSynonyms;
        if ((parsedSynonyms & 0x1ffff) === 0) {
          const percent = synonymCapacity ? parsedSynonyms / synonymCapacity : 1;
          setDictionaryProgress(8 + percent * 18, t('files.dictionary_preparing', {name: parsed.name}));
          await new Promise(resolve => setTimeout(resolve, 0));
        }
      }
    }

    const sortedKeys = sortKeys.subarray(0, recordCount);
    sortedKeys.sort();
    const indexSize = DICTIONARY_INDEX_HEADER_SIZE + recordCount * DICTIONARY_INDEX_RECORD_SIZE;
    const indexBytes = new Uint8Array(indexSize);
    const indexView = new DataView(indexBytes.buffer);
    indexBytes.set(dictionaryEncoder.encode('IMDX'), 0);
    indexView.setUint16(4, 2, true);
    indexView.setUint16(6, DICTIONARY_INDEX_RECORD_SIZE, true);
    indexView.setUint32(8, recordCount, true);
    indexView.setUint32(12, 0, true);
    dataView.setUint32(8, recordCount, true);

    for (let index = 0; index < recordCount; ++index) {
      const key = sortedKeys[index];
      const ordinal = Number(key & 0xffffffffn);
      const offset = DICTIONARY_INDEX_HEADER_SIZE + index * DICTIONARY_INDEX_RECORD_SIZE;
      indexView.setUint32(offset, Number(key >> 32n), true);
      indexView.setUint32(offset + 4, secondaryHashes[ordinal], true);
      indexView.setUint32(offset + 8, targetOffsets[ordinal], true);
      indexView.setUint32(offset + 12, targetLengths[ordinal], true);
    }

    return {
      name: dictionaryUniqueName(parsed.name, usedNames),
      entryCount: recordCount,
      files: [
        new File([indexBytes], 'dictionary.idx', {type: 'application/octet-stream'}),
        new File([dataBytes], 'dictionary.dat', {type: 'application/octet-stream'})
      ]
    };
  }

  async function dictionaryUploadFile(file, targetPath, onProgress) {
    try {
      await uploadFileWebSocket(file, onProgress, null, null, targetPath);
    } catch (error) {
      if (!/connection failed/i.test(String(error && error.message))) throw error;
      await uploadFileHTTP(file, onProgress, null, null, targetPath);
    }
  }

  async function prepareAndUploadDictionaries() {
    if (dictionaryBusy || dictionarySelectedFiles.length === 0) return;
    setDictionaryBusy(true);
    const prepared = [];
    try {
      await dictionaryCleanupAbandonedStores();
      setDictionaryProgress(2, t('files.dictionary_preparing', {name: dictionarySelectedFiles[0].name}));
      const sources = await dictionaryCollectSources(dictionarySelectedFiles);
      const parsed = await dictionaryParseSources(sources);
      if (!parsed.length) throw new Error(t('files.dictionary_no_sources'));

      const usedNames = new Set();
      for (const item of parsed) {
        const dictionary = await dictionaryPrepareBinary(item, usedNames);
        if (dictionary) prepared.push(dictionary);
      }
      if (!prepared.length) throw new Error(t('files.dictionary_no_sources'));
      setDictionaryProgress(70, t('files.dictionary_preparing', {name: prepared[0].name}));

      folderExistsCache = new Set();
      const uploads = [];
      for (const dictionary of prepared) {
        for (const file of dictionary.files) uploads.push({dictionary, file});
      }
      let completedBytes = 0;
      const totalBytes = uploads.reduce((sum, upload) => sum + upload.file.size, 0);
      for (let i = 0; i < uploads.length; ++i) {
        const upload = uploads[i];
        const targetPath = joinRemotePath(DICTIONARY_ROOT, upload.dictionary.name);
        await ensureRemoteFolderPath(targetPath);
        setDictionaryProgress(70 + (completedBytes / totalBytes) * 29,
          t('files.dictionary_uploading', {name: upload.dictionary.name, current: i + 1, total: uploads.length}));
        await dictionaryUploadFile(upload.file, targetPath, (sent, size) => {
          setDictionaryProgress(70 + ((completedBytes + sent) / totalBytes) * 29,
            t('files.dictionary_uploading', {name: upload.dictionary.name, current: i + 1, total: uploads.length}));
        });
        completedBytes += upload.file.size;
      }

      const entryCount = prepared.reduce((sum, dictionary) => sum + dictionary.entryCount, 0);
      setDictionaryProgress(100, t('files.dictionary_complete', {count: prepared.length, entries: entryCount}));
      showNotification(t('files.dictionary_complete', {count: prepared.length, entries: entryCount}), 'success');
      dictionarySelectedFiles = [];
      document.getElementById('prepareDictionaryBtn').disabled = true;
      if (currentPath === DICTIONARY_ROOT || currentPath.startsWith(DICTIONARY_ROOT + '/')) hydrate();
    } catch (error) {
      const message = error && error.message ? error.message : String(error);
      setDictionaryProgress(0, t('files.dictionary_error', {msg: message}));
      showNotification(t('files.dictionary_error', {msg: message}), 'error');
    } finally {
      for (const dictionary of prepared) {
        if (dictionary.cleanup) {
          try {
            await dictionary.cleanup();
          } catch (error) {
            console.warn('[Dictionary] Temporary storage cleanup failed:', error);
          }
        }
      }
      setDictionaryBusy(false);
    }
  }
  hydrate();
