if exists("b:did_indent")
  finish
endif
let b:did_indent = 1

setlocal indentexpr=GetIdiomIndent()
setlocal indentkeys=0),0],0},!^F,o,O,0=end,0=else,0=rescue,0=ensure

if exists("*GetIdiomIndent")
  finish
endif

function! GetIdiomIndent()
  let prev = prevnonblank(v:lnum - 1)
  if prev == 0
    return 0
  endif
  let ind = indent(prev)
  let prevline = getline(prev)
  if prevline =~# '\%(^\|\s\)\%(do\|else\|rescue\|ensure\)\s*$' || prevline =~# '->\s*$' || prevline =~# '[([{]\s*$'
    let ind += shiftwidth()
  endif
  let curline = getline(v:lnum)
  if curline =~# '^\s*\%(end\>\|else\>\|rescue\>\|ensure\>\|[)\]}]\)'
    let ind -= shiftwidth()
  endif
  return max([ind, 0])
endfunction
