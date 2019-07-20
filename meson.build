project('makron', 'c', default_options : ['buildtype=debugoptimized'])

xcb = dependency('xcb')
sulfur = dependency('sulfur')
iniparser = dependency('iniparser')
executable('makron', 'src/main.c', dependencies : [xcb, sulfur, iniparser], install : true)
executable('makron-reload', 'src/makutil.c', dependencies : [xcb, sulfur, iniparser], install : true)


run_target('run', command : 'test.sh')
