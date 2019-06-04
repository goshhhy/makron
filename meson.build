project('makron', 'c')

xcb = dependency('xcb')
sulfur = dependency('sulfur')
executable('makron', 'src/main.c', dependencies : [xcb, sulfur], install : true)

run_target('nest', command : 'test.sh')
