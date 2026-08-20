Import('env')

from pathlib import Path

project_dir = Path(env.subst('$PROJECT_DIR'))
source = project_dir / 'src' / 'network' / 'OtaUpdater.cpp'
text = source.read_text(encoding='utf-8')

old_timeout = '.timeout_ms = 15000,'
new_timeout = '.timeout_ms = 45000,'
if old_timeout not in text:
    raise RuntimeError('OTA timeout marker not found')
text = text.replace(old_timeout, new_timeout, 1)

old_delay = 'delay(100);  // TODO: should we replace this with something better?'
new_delay = 'delay(25);  // Keep OTA pump responsive on slow GitHub/CDN links.'
if old_delay in text:
    text = text.replace(old_delay, new_delay, 1)

source.write_text(text, encoding='utf-8')
print('[inkMOD] Applied OTA HTTP robustness patch: timeout=45s, pump delay=25ms')
