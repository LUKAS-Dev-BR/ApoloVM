# apolovm-main/meson.build
# Sistema de build Meson da ApoloVM V2 (Meson é o build system oficial do
# QEMU). Gera um único binário `apolovmd` unindo: C (codec AGP + cliente VNC),
# Assembly (NEON), Rust (libapolographics) e C++ (servidor WS/HTTP).
project('apolovm', ['c', 'cpp'], version: '2.0.0',
        default_options: ['c_std=c11', 'cpp_std=c++17', 'warning_level=2', 'buildtype=release'])

cc = meson.get_compiler('c')
cpp = meson.get_compiler('cpp')

# ----- busca rustc -----
rustc = find_program('rustc', required: get_option('with_rust'))
if rustc.found()
  message('rustc: @0@'.format(rustc.version()))
endif

# ----- núcleo C -----
agp_c = files('src/agp.c')
vnc_c = files('src/vnc.c')
neon_s = files('asm/bgra_rgba_a64.S')

# ----- lib Rust (staticlib compilada via rustc, linkada no binário) -----
rust_lib = []
if rustc.found()
  rust_lib = custom_target('libapolographics',
    output: 'libapolographics.a',
    command: [rustc, '--crate-type', 'staticlib', '-O', '-o', '@OUTPUT@',
              files('rust/src/lib.rs')],
    build_by_default: true,
  )
endif

# ----- daemon -----
extra = []
if not rustc.found()
  extra += '-DAPG_EMBED_DELTA_CPP'
endif

apolovmd = executable('apolovmd',
  ['src/daemon.cpp', 'src/ws.cpp', agp_c, vnc_c, neon_s, rust_lib],
  c_args: [],
  cpp_args: extra,
  include_directories: include_directories('src'),
  install: true,
)

# estatística dos fontes por linguagem (para o relatório)
source_summary = {'C': agp_c, 'C++': ['src/daemon.cpp', 'src/ws.cpp'],
                  'Assembly': neon_s}
if rustc.found()
  source_summary += {'Rust': ['rust/src/lib.rs']}
endif
foreach lan : source_summary.keys()
  message('@0@: @1@ arquivo(s)'.format(lan, source_summary.get(lan).length()))
endforeach

# ----- testes -----
test('des-fips', executable('des-test', 'tests/c/des_test.c',
     c_args: ['-D_GNU_SOURCE', '-std=c11'], install: false))

neon_test = executable('neon-test', 'tests/c/neon_test.c', neon_s, install: false)
test('neon-swizzle', neon_test)