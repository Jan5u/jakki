project('jakki', 'cpp', version : '0.1', default_options : ['warning_level=1', 'cpp_std=c++20'])

qt6 = import('qt6')
qt6_dep = dependency('qt6', modules: ['Core', 'Gui', 'Widgets'])
openssl_dep = dependency('openssl')
opus_dep = dependency('opus')
pipewire_dep = dependency('libpipewire-0.3')
libavcodec_dep = dependency('libavcodec')
libavformat_dep = dependency('libavformat')
thread_dep = dependency('threads')

json_dep = dependency('nlohmann_json', required: true)

sources = files(
    'src/main.cpp',
    'src/gui.cpp',
    'src/gui.hpp',
    'src/audio.cpp',
    'src/audio.hpp',
    'src/network.cpp',
    'src/network.hpp',
)

sources += qt6.preprocess(ui_files : ['ui/main.ui', 'ui/connect.ui'])

executable('jakki', sources, dependencies : [qt6_dep, openssl_dep, opus_dep, pipewire_dep, libavcodec_dep, libavformat_dep, thread_dep, json_dep])