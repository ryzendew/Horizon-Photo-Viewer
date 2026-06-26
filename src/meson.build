inc = include_directories('.', '../third_party')

sources = files(
  'main.cpp',
  'features/theme/theme.cpp',
  'features/zoom/zoom.cpp',
  'features/crop/crop.cpp',
  'features/markup/markup.cpp',
  'features/file_save/save.cpp',
  'features/navigation/navigation.cpp',
  'features/prefetch/prefetch.cpp',
  'features/drag_drop/drag_drop.cpp',
  'core/viewer/app.cpp',
  'common/config/config.cpp',
  'common/file_utils/trash.cpp',
  'cache/thumbnail/thumb_cache.cpp',
  'thread/pool/decode_pool.cpp',
  'wayland/core/connection.cpp',
  'wayland/core/seat.cpp',
  'wayland/buffer/shm_buffer.cpp',
  'wayland/surface/surface_extensions.cpp',
  'render/text/text_renderer.cpp',
  'dbus/portal/portal_file_dialog.cpp',
  'decode/core/decoder.cpp',
  'decode/core/registry.cpp',
  'decode/common/svg_doc.cpp',
  'decode/common/svg_render.hpp',
  'decode/common/color.cpp',
  'decode/formats/svg/svg_decoder.cpp',
  'decode/formats/wuffs/wuffs_decoder.cpp',
  'ui/overlay/overlay.cpp',
  'ui/overlay/toolbar.cpp',
  'ui/overlay/settings.cpp',
  'ui/overlay/menu.cpp',
  'ui/overlay/sidebar.cpp',
  'ui/overlay/crop_render.cpp',
  'ui/overlay/markup_render.cpp',
  'ui/thumbnail_strip/thumbnail_strip.cpp',
)

sources += files(
  '../third_party/tinyspline/tinyspline.c',
  '../third_party/tinyspline/tinysplinecxx.cxx',
  '../third_party/tinyspline/parson.c',
)

deps = [
  protocol_dep,
  wl_client,
  xkbcommon,
  dbus_1,
  cairo,
  threads,
]

jpeg = dependency('libjpeg', required: get_option('jpeg'))
if jpeg.found()
  sources += files('decode/formats/jpeg/jpeg_decoder.cpp')
  deps += jpeg
  add_project_arguments('-DHAVE_LIBJPEG=1', language: 'cpp')
endif

webp = dependency('libwebp', required: get_option('webp'))
if webp.found()
  sources += files('decode/formats/webp/webp_decoder.cpp')
  deps += webp
  add_project_arguments('-DHAVE_LIBWEBP=1', language: 'cpp')
endif

heif = dependency('libheif', required: get_option('heif'))
if heif.found()
  sources += files('decode/formats/heif/heif_decoder.cpp')
  deps += heif
  add_project_arguments('-DHAVE_LIBHEIF=1', language: 'cpp')
endif

avif = dependency('libavif', required: get_option('avif'))
if avif.found()
  sources += files('decode/formats/avif/avif_decoder.cpp')
  deps += avif
  add_project_arguments('-DHAVE_LIBAVIF=1', language: 'cpp')
endif

raw = dependency('libraw', required: get_option('raw'))
if raw.found()
  sources += files('decode/formats/raw/raw_decoder.cpp')
  deps += raw
  add_project_arguments('-DHAVE_LIBRAW=1', language: 'cpp')
endif

jxl = dependency('libjxl', required: get_option('jxl'))
if jxl.found()
  sources += files('decode/formats/jxl/jxl_decoder.cpp')
  deps += jxl
  add_project_arguments('-DHAVE_LIBJXL=1', language: 'cpp')
endif

metadata = dependency('exiv2', required: get_option('metadata'))
if metadata.found()
  sources += files('decode/common/exif.cpp')
  deps += metadata
  add_project_arguments('-DHAVE_METADATA=1', language: 'cpp')
endif

lcms2 = dependency('lcms2', required: get_option('color_management'))
if lcms2.found()
  sources += files('decode/common/color.cpp')
  deps += lcms2
  add_project_arguments('-DHAVE_LCMS2=1', language: 'cpp')
endif

librsvg = dependency('librsvg-2.0', required: get_option('librsvg'))
if librsvg.found()
  deps += librsvg
  add_project_arguments('-DHAVE_LIBRSVG=1', language: 'cpp')
endif

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
