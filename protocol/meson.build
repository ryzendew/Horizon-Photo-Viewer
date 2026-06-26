# List of protocol XML files to generate headers + code for
protocol_xmls = [
  'xdg-shell.xml',
  'viewporter.xml',
  'fractional-scale-v1.xml',
  'tearing-control-v1.xml',
  'color-management-v1.xml',
  'idle-inhibit-unstable-v1.xml',
  'keyboard-shortcuts-inhibit-unstable-v1.xml',
  'linux-dmabuf-v1.xml',
  'single-pixel-buffer-v1.xml',
  'xdg-output-unstable-v1.xml',
]

# Generate public headers (usable by external code)
protocol_headers = []
foreach p : protocol_xmls
  xml = join_paths(meson.current_source_dir(), p)
  header = custom_target(p + '-header',
    input: xml,
    output: '@BASENAME@-client-protocol.h',
    command: [wl_scanner, 'client-header', '@INPUT@', '@OUTPUT@'])
  protocol_headers += header
endforeach

# Generate private code (compiled into our library)
protocol_code = []
foreach p : protocol_xmls
  xml = join_paths(meson.current_source_dir(), p)
  code = custom_target(p + '-code',
    input: xml,
    output: '@BASENAME@-protocol.c',
    command: [wl_scanner, 'private-code', '@INPUT@', '@OUTPUT@'])
  protocol_code += code
endforeach

# Bundle into a dependency for src/
protocol_dep = declare_dependency(
  sources: protocol_headers + protocol_code,
  include_directories: include_directories('.'),
)
