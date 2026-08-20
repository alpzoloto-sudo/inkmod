Import('env')

from pathlib import Path

project_dir = Path(env.subst('$PROJECT_DIR'))
files_js = project_dir / 'web' / 'pages' / 'files.js'
text = files_js.read_text(encoding='utf-8')

folder_start = text.find('  function createFolder() {')
if folder_start < 0:
    raise RuntimeError('createFolder() not found in web/pages/files.js')

folder_end = text.find('\n  // Rename functions', folder_start)
if folder_end < 0:
    raise RuntimeError('createFolder() end marker not found in web/pages/files.js')

block = text[folder_start:folder_end]
changed = False

old_name = "const folderName = document.getElementById('folderName').value.trim();"
new_name = "const folderName = document.getElementById('folderName').value.trim().normalize('NFC');"
if old_name in block:
    block = block.replace(old_name, new_name, 1)
    changed = True

old_success = """if (xhr.status === 200) {\n        window.location.reload();\n      } else {"""
new_success = """if (xhr.status === 200) {\n        // Safari/iOS can restore the just-rendered /files document from its\n        // page cache after a mkdir. Force a new navigation token so the\n        // subsequent hydrate() always sees the freshly-created directory.\n        closeFolderModal();\n        const refreshUrl = new URL(window.location.href);\n        refreshUrl.searchParams.set('_refresh', String(Date.now()));\n        window.location.replace(refreshUrl.pathname + refreshUrl.search + refreshUrl.hash);\n      } else {"""

already_patched = (
    "refreshUrl.searchParams.set('_refresh', String(Date.now()))" in block
    and 'window.location.replace(refreshUrl.pathname + refreshUrl.search + refreshUrl.hash)' in block
)

if old_success in block:
    block = block.replace(old_success, new_success, 1)
    changed = True
elif not already_patched:
    raise RuntimeError('createFolder() success block not found in web/pages/files.js')

if changed:
    text = text[:folder_start] + block + text[folder_end:]
    files_js.write_text(text, encoding='utf-8')
    print('[inkMOD] Applied Apple/Safari folder refresh compatibility patch')
else:
    print('[inkMOD] Apple/Safari folder refresh compatibility patch already applied')
