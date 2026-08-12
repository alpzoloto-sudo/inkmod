#!/usr/bin/env bash
# Usage: ./scripts/release.sh 1.2.7
# Bumps inkmod_version in platformio.ini, commits, and creates the git tag.
set -euo pipefail

VERSION="${1:-}"
if [[ -z "$VERSION" ]]; then
  echo "Usage: $0 <version>  (e.g. 1.2.7)" >&2
  exit 1
fi

if sed --version >/dev/null 2>&1; then
  # GNU sed (Linux)
  sed -i "s/inkmod_version = .*/inkmod_version = $VERSION/" platformio.ini
else
  # BSD sed (macOS)
  sed -i '' "s/inkmod_version = .*/inkmod_version = $VERSION/" platformio.ini
fi
git add platformio.ini
if git diff --cached --quiet; then
  echo "platformio.ini is already at version $VERSION - nothing to commit, tagging as-is."
else
  git commit -m "Update inkmod_version to $VERSION"
fi

if git rev-parse "v$VERSION" >/dev/null 2>&1; then
  echo "Tag v$VERSION already exists locally - not re-tagging." >&2
else
  git tag "v$VERSION"
fi
echo "Tagged v$VERSION — push with:"
echo "  git push"
echo "  git push origin v$VERSION"
