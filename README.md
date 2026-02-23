# img-converter

Command-line image format converter. Reads an image in one format and writes it in another.

## Build

```
make
make install
```

Installs to `~/.local/bin/`.

## Dependencies

**Required:**
- libpng (`libpng-dev`)
- libjpeg (`libjpeg-dev`)

**Optional** (auto-detected via pkg-config):
- libtiff (`libtiff-dev`) -- TIFF support
- libwebp (`libwebp-dev`) -- WebP support
- libavif (`libavif-dev`) -- AVIF support
- libheif (`libheif-dev`) -- HEIF/HEIC support
- libjxl (`libjxl-dev`) -- JPEG XL support

## Usage

```
img-converter [OPTIONS] INPUT
```

An output file must be specified with `-o`. The output format is detected from the file extension, or can be set explicitly with `-f`.

### Options

| Option | Description |
|---|---|
| `-f, --format FORMAT` | Output format (png, jpg, bmp, qoi, tiff, webp, avif, heic, jxl) |
| `-q, --quality N` | Lossy quality, 1-100 (default: 85). Applies to JPEG, WebP, AVIF, HEIF, JXL. |
| `-o, --output FILE` | Output file (required) |
| `-m, --max-pixels N` | Reject images exceeding N total pixels (default: 100000000; 0 = unlimited) |
| `-B, --max-bytes N` | Reject input files exceeding N bytes (default: 268435456; 0 = unlimited) |

### Examples

```
img-converter photo.png -o photo.jpg
img-converter photo.png -o photo.webp -q 90
img-converter input.bmp -f png -o output.png
```

## Supported Formats

| Format | Extensions | Type | Transparency | Library |
|---|---|---|---|---|
| PNG | .png | Lossless | Yes | libpng (built-in) |
| JPEG | .jpg, .jpeg | Lossy | No | libjpeg (built-in) |
| BMP | .bmp | Lossless | No | None (built-in) |
| QOI | .qoi | Lossless | Yes | None (built-in) |
| TIFF | .tiff, .tif | Lossless | Yes | libtiff (optional) |
| WebP | .webp | Lossy | Yes | libwebp (optional) |
| AVIF | .avif | Lossy | Yes | libavif (optional) |
| HEIF/HEIC | .heif, .heic | Lossy | Yes | libheif (optional) |
| JPEG XL | .jxl | Lossy/Lossless | Yes | libjxl (optional) |
