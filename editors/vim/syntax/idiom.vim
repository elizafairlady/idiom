if exists("b:current_syntax")
  finish
endif

syn iskeyword @,48-57,_,-,?,!,=,47,60,62

syn keyword idiomDeclaration package use import export protocol activate grammar type record trait implement method spec info operator form reader-form core-form core-reader-form core-operator core-grammar
syn keyword idiomDefine defn defmacro def nextgroup=idiomFunctionName skipwhite
syn match idiomFunctionName "\k\+" contained
syn keyword idiomKeyword do end else rescue ensure fn receive try match case unless cond if and or when
syn keyword idiomException raise error
syn keyword idiomSurface implements? protocol-info explain

syn match idiomType "\<[A-Z]\k*"

syn match idiomNumber "\<[0-9]\+\>"
syn match idiomNumber "[+-]\?\<\d\+[eE][+-]\?\d\+\>"
syn match idiomNumber "[+-]\?\<\d\+\.\d\+\%([eE][+-]\?\d\+\)\?\>"

syn match idiomAtom ":[^ \t\r\n()[\]{};#\"'`,]\+"
syn match idiomKeywordArg "\<[A-Za-z_]\k*:\%([^ \t\r\n()[\]{};\"'`,<>|=]\)\@="
syn match idiomKeywordArg "\<[A-Za-z_]\k*:\%(\s\|$\)\@="

syn match idiomOperator "[-><=|+*/%!&@?]\+"
syn match idiomOperator "::"

syn match idiomFnRef "&[A-Za-z_]\k*\%(\.[A-Za-z_]\k*\)*"

syn match idiomQuote "%'\|%`\|%,@\|%,\|,@\|['`,^]"
syn match idiomDictDelim "%{"

syn region idiomBitstring matchgroup=idiomDictDelim start="%<" end=">" contains=idiomNumber,idiomAtom,idiomOperator,idiomString,idiomQuote

syn match idiomEscape "\\." contained
syn region idiomInterp matchgroup=idiomDictDelim start="#{" end="}" contained contains=TOP
syn region idiomString start=+"+ skip=+\\.+ end=+"+ contains=idiomInterp,idiomEscape
syn region idiomRegex start=+r"+ skip=+\\.+ end=+"+ contains=idiomEscape

syn match idiomComment "#.*$" contains=@Spell

hi def link idiomComment Comment
hi def link idiomString String
hi def link idiomRegex String
hi def link idiomEscape SpecialChar
hi def link idiomNumber Number
hi def link idiomAtom Constant
hi def link idiomKeywordArg Constant
hi def link idiomFnRef Function
hi def link idiomFunctionName Function
hi def link idiomDeclaration Define
hi def link idiomDefine Define
hi def link idiomKeyword Keyword
hi def link idiomException Exception
hi def link idiomSurface Keyword
hi def link idiomType Type
hi def link idiomOperator Operator
hi def link idiomQuote Macro
hi def link idiomDictDelim Special

let b:current_syntax = "idiom"
