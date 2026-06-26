inc = include_directories('.', '../third_party')

# Core source files (always compiled)
sources = files(
  'main.cpp',
  'core/app.cpp',
  'core/config.cpp',
  'core/decode_pool.cpp',
  'core/thumb_cache.cpp',
  'core/trash.cpp',
  'wayland/connection.cpp',
  'wayland/seat.cpp',
  'wayland/surface_extensions.cpp',
  'wayland/shm_buffer.cpp',
  'render/text_renderer.cpp',
  'render/vulkan_context.cpp',
  'render/vulkan_surface.cpp',
  'dbus/portal_file_dialog.cpp',
  'decode/decoder.cpp',
  'decode/wuffs_decoder.cpp',
  'ui/overlay.cpp',
  'ui/thumbnail_strip.cpp',
)

# Dependencies list for the executable
deps = [
  protocol_dep,
  wl_client,
  xkbcommon,
  vulkan,
  dbus_1,
  cairo,
  threads,
]

# Optional JPEG decoder
jpeg = dependency('libjpeg', required: get_option('jpeg'))
if jpeg.found()
  sources += files('decode/jpeg_decoder.cpp')
  deps += jpeg
  add_project_arguments('-DHAVE_LIBJPEG=1', language: 'cpp')
endif

# Optional WebP decoder
webp = dependency('libwebp', required: get_option('webp'))
if webp.found()
  sources += files('decode/webp_decoder.cpp')
  deps += webp
  add_project_arguments('-DHAVE_LIBWEBP=1', language: 'cpp')
endif

# Optional HEIC decoder
heif = dependency('libheif', required: get_option('heif'))
if heif.found()
  sources += files('decode/heif_decoder.cpp')
  deps += heif
  add_project_arguments('-DHAVE_LIBHEIF=1', language: 'cpp')
endif

# Optional AVIF decoder
avif = dependency('libavif', required: get_option('avif'))
if avif.found()
  sources += files('decode/avif_decoder.cpp')
  deps += avif
  add_project_arguments('-DHAVE_LIBAVIF=1', language: 'cpp')
endif

# Optional RAW decoder
raw = dependency('libraw', required: get_option('raw'))
if raw.found()
  sources += files('decode/raw_decoder.cpp')
  deps += raw
  add_project_arguments('-DHAVE_LIBRAW=1', language: 'cpp')
endif

# Optional JPEG-XL decoder
jxl = dependency('libjxl', required: get_option('jxl'))
if jxl.found()
  sources += files('decode/jxl_decoder.cpp')
  deps += jxl
  add_project_arguments('-DHAVE_LIBJXL=1', language: 'cpp')
endif

# Optional metadata
metadata = dependency('exiv2', required: get_option('metadata'))
if metadata.found()
  sources += files('decode/exif.cpp')
  deps += metadata
  add_project_arguments('-DHAVE_METADATA=1', language: 'cpp')
endif

# Optional color management
lcms2 = dependency('lcms2', required: get_option('color_management'))
if lcms2.found()
  sources += files('decode/color.cpp')
  deps += lcms2
  add_project_arguments('-DHAVE_LCMS2=1', language: 'cpp')
endif

# Icon font path for system-wide install look-up
icon_font_system = get_option('prefix') / get_option('datadir') / 'horizon-photo-viewer' / 'fonts' / 'MaterialSymbolsRounded.ttf'
add_project_arguments('-DICON_FONT_PATH_SYSTEM="' + icon_font_system + '"', language: 'cpp')

crop_svg_system = get_option('prefix') / get_option('datadir') / 'horizon-photo-viewer' / 'crop.svg'
add_project_arguments('-DCROP_SVG_PATH_SYSTEM="' + crop_svg_system + '"', language: 'cpp')

executable('horizon-photo-viewer',
  sources,
  dependencies: deps,
  include_directories: inc,
  install: true,
)
