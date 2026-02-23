CC ?= clang
CFLAGS := -O3 -ffast-math

BIN := img-converter
SRC := src/img-converter.c

# Required: libpng
HAVE_PNG := $(shell pkg-config --exists libpng 2>/dev/null && echo 1)
ifneq ($(HAVE_PNG),1)
  $(error libpng not found (required); install libpng-dev)
endif
LDFLAGS += -lpng

# Required: libjpeg
HAVE_JPEG := $(shell pkg-config --exists libjpeg 2>/dev/null && echo 1)
ifneq ($(HAVE_JPEG),1)
  $(error libjpeg not found (required); install libjpeg-dev)
endif
LDFLAGS += -ljpeg

# Optional: libtiff
HAVE_TIFF := $(shell pkg-config --exists libtiff-4 2>/dev/null && echo 1)
ifeq ($(HAVE_TIFF),1)
  CFLAGS += -DHAVE_TIFF
  LDFLAGS += -ltiff
else
  $(info NOTE: libtiff not found; img-converter compiled without TIFF support)
endif

# Optional: libwebp
HAVE_WEBP := $(shell pkg-config --exists libwebp 2>/dev/null && echo 1)
ifeq ($(HAVE_WEBP),1)
  CFLAGS += -DHAVE_WEBP
  LDFLAGS += -lwebp
else
  $(info NOTE: libwebp not found; img-converter compiled without WebP support)
endif

# Optional: libavif
HAVE_AVIF := $(shell pkg-config --exists libavif 2>/dev/null && echo 1)
ifeq ($(HAVE_AVIF),1)
  CFLAGS += -DHAVE_AVIF
  LDFLAGS += -lavif
else
  $(info NOTE: libavif not found; img-converter compiled without AVIF support)
endif

# Optional: libheif
HAVE_HEIF := $(shell pkg-config --exists libheif 2>/dev/null && echo 1)
ifeq ($(HAVE_HEIF),1)
  CFLAGS += -DHAVE_HEIF
  LDFLAGS += -lheif
else
  $(info NOTE: libheif not found; img-converter compiled without HEIC support)
endif

# Optional: libjxl
HAVE_JXL := $(shell pkg-config --exists libjxl 2>/dev/null && echo 1)
ifeq ($(HAVE_JXL),1)
  CFLAGS += -DHAVE_JXL
  LDFLAGS += -ljxl -ljxl_threads
else
  $(info NOTE: libjxl not found; img-converter compiled without JPEG XL support)
endif

all: $(BIN)

$(BIN): $(SRC) $(wildcard src/lib/*.h)
	$(CC) $(CFLAGS) $(SRC) -o $@ $(LDFLAGS)
	strip $@

install: $(BIN)
	install -d ~/.local/bin
	install -m 755 $(BIN) ~/.local/bin/

clean:
	rm -f $(BIN)

.PHONY: all install clean
