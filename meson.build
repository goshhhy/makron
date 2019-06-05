project('makron', 'c', default_options : ['buildtype=debugoptimized'])

xcb = dependency('xcb')
sulfur = dependency('sulfur')
executable('makron', 'src/main.c', dependencies : [xcb, sulfur], install : true)

run_target('run', command : 'test.sh')
