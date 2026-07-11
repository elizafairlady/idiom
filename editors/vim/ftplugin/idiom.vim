if exists("b:did_ftplugin")
  finish
endif
let b:did_ftplugin = 1

setlocal commentstring=#\ %s
setlocal comments=:#
setlocal iskeyword=@,48-57,_,-,?,!,=,47,60,62
setlocal suffixesadd=.id
setlocal shiftwidth=2 softtabstop=2 expandtab

let b:undo_ftplugin = "setlocal commentstring< comments< iskeyword< suffixesadd< shiftwidth< softtabstop< expandtab<"
