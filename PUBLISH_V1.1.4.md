# Publish inkMOD v1.1.4

This tree is prepared as the next release instead of rewriting v1.1.3 again.

## Local verification on Windows

```bat
cd /d D:\system\Desktop\inkmod-github
pio run -e developer -t upload
pio run -e release
```

Hardware smoke test: large EPUB, ~46.5 MiB FB2, FB2.ZIP, XTC rapid paging,
dictionary/clippings, manual sleep and timeout sleep.

## Publish

```bat
git add .
git commit -m "Release inkMOD v1.1.4"
git push origin main

git tag -a v1.1.4 -m "inkMOD v1.1.4"
git push origin v1.1.4
```

The tag triggers `.github/workflows/release.yml`, which now builds the real
`release` environment and uploads:

`firmware-release-v1.1.4.bin`

Do not create a `firmware-tiny` asset for this release: release OTA clients
search for `firmware-release*.bin`.
