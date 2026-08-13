# Re-publish inkMOD v1.1.3

The repository workflow already supports both tag builds and manual `workflow_dispatch` builds.

## Recommended: replace the bad v1.1.3 without changing the version

1. Push the corrected source to the default branch.
2. Open **GitHub → Actions → Build and publish release firmware**.
3. Choose **Run workflow**.
4. Enter version: `1.1.3`.
5. Run it.

The workflow embeds `1.1.3`, builds the `tiny` firmware and publishes it against tag/release `v1.1.3`.

This is the cleanest option when you only need to replace the release asset and keep the same version.

## If you want to completely delete the old release and tag

Delete the GitHub Release first, then remove the tag locally/remotely:

```bash
git tag -d v1.1.3
git push origin :refs/tags/v1.1.3
```

Push the corrected source, then recreate the tag:

```bash
git add .
git commit -m "Fix v1.1.3 post-release issues"
git push origin main

git tag -a v1.1.3 -m "inkMOD v1.1.3"
git push origin v1.1.3
```

The tag push triggers the release workflow again.

## Before publishing

Developer smoke test:

```bash
pio run -e developer -t upload
```

Release build/flash test:

```bash
pio run -e release -t upload
```

At minimum test:

- large FB2 (`Колесо Времени.fb2` class of books)
- FB2.ZIP
- dictionary assigned to Back / Confirm / front buttons
- create/remove clipping
- XTC fast page turns
- USB unplug/replug on X4
- sleep cover and custom PNG sleep overlay
- Lyra Carousel with 7 icons
- web portal / EPUBKIT / wallpaper generator
