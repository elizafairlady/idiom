;;; idiom-mode.el --- Major mode for the Idiom language -*- lexical-binding: t; -*-

;; Keywords: languages
;; URL: https://github.com/eliza/idiom

;;; Commentary:

;; Syntax highlighting, indentation, comments, and imenu for Idiom
;; (.id files).  Keyword sets mirror the compiler's tables: BODY_DECLS
;; and SURFACE_KEYWORDS in the expander, the kernel's control macros,
;; and the bootstrap grammar's token vocabulary.

;;; Code:

(defgroup idiom nil
  "Major mode for the Idiom language."
  :group 'languages)

(defcustom idiom-indent-offset 2
  "Indentation width for Idiom blocks."
  :type 'integer
  :group 'idiom)

(defconst idiom--declaration-keywords
  '("package" "use" "import" "export" "protocol" "activate" "grammar"
    "type" "record" "trait" "implement" "method" "spec" "info"
    "operator" "form" "reader-form"
    "core-form" "core-reader-form" "core-operator" "core-grammar"))

(defconst idiom--control-keywords
  '("do" "end" "else" "rescue" "ensure" "fn" "receive" "try" "match"
    "case" "unless" "cond" "if" "and" "or" "when"
    "implements?" "protocol-info" "explain"))

(defconst idiom--definition-keywords '("defn" "defmacro" "def"))

(defconst idiom--exception-keywords '("raise" "error"))

(defvar idiom-mode-syntax-table
  (let ((table (make-syntax-table)))
    (modify-syntax-entry ?# "<" table)
    (modify-syntax-entry ?\n ">" table)
    (modify-syntax-entry ?\" "\"" table)
    (modify-syntax-entry ?\\ "\\" table)
    (dolist (c '(?- ?? ?! ?_ ?/ ?< ?>))
      (modify-syntax-entry c "_" table))
    (modify-syntax-entry ?' "'" table)
    (modify-syntax-entry ?` "'" table)
    (modify-syntax-entry ?, "'" table)
    (modify-syntax-entry ?^ "'" table)
    (modify-syntax-entry ?% "." table)
    (modify-syntax-entry ?& "." table)
    (modify-syntax-entry ?| "." table)
    table)
  "Syntax table for `idiom-mode'.")

(defun idiom--match-interpolation (limit)
  "Find the next \"#{...}\" fragment inside a string, up to LIMIT."
  (let (found)
    (while (and (not found) (re-search-forward "#{[^}\n]*}" limit t))
      (when (save-match-data
              (save-excursion (nth 3 (syntax-ppss (match-beginning 0)))))
        (setq found t)))
    found))

(defconst idiom--font-lock-keywords
  `((,(concat (regexp-opt idiom--definition-keywords 'symbols)
              "\\s-+\\(\\(?:\\sw\\|\\s_\\)+\\)")
     (1 font-lock-keyword-face) (2 font-lock-function-name-face))
    (,(regexp-opt idiom--declaration-keywords 'symbols)
     . font-lock-builtin-face)
    (,(regexp-opt idiom--control-keywords 'symbols)
     . font-lock-keyword-face)
    (,(regexp-opt idiom--exception-keywords 'symbols)
     . font-lock-warning-face)
    ("&[A-Za-z_]\\(?:\\sw\\|\\s_\\)*\\(?:\\.[A-Za-z_]\\(?:\\sw\\|\\s_\\)*\\)*"
     . font-lock-function-name-face)
    (":\\(?:\\sw\\|\\s_\\|[.*=|+&@]\\)+" . font-lock-constant-face)
    ("\\_<[A-Za-z_]\\(?:\\sw\\|\\s_\\)*:" . font-lock-constant-face)
    ("\\_<[A-Z]\\(?:\\sw\\|\\s_\\)*" . font-lock-type-face)
    ("%'\\|%`\\|%,@\\|%,\\|,@\\|%{\\|%<" . font-lock-preprocessor-face)
    (idiom--match-interpolation (0 font-lock-preprocessor-face prepend)))
  "Font-lock rules for `idiom-mode'.")

(defun idiom--closing-line-p ()
  "Whether the current line begins with a block or bracket closer."
  (save-excursion
    (back-to-indentation)
    (looking-at "\\(?:end\\_>\\|else\\_>\\|rescue\\_>\\|ensure\\_>\\|[)}\"]\\|\\]\\)")))

(defun idiom--opening-line-p ()
  "Whether the current line ends by opening a block, clause, or bracket."
  (save-excursion
    (end-of-line)
    (skip-chars-backward " \t")
    (or (looking-back "\\(?:\\_<do\\|\\_<else\\|\\_<rescue\\|\\_<ensure\\|->\\)" (line-beginning-position))
        (memq (char-before) '(?\( ?\[ ?{)))))

(defun idiom-indent-line ()
  "Indent the current line relative to the previous non-blank line."
  (interactive)
  (let ((indent 0))
    (save-excursion
      (beginning-of-line)
      (when (re-search-backward "^[ \t]*[^ \t\n]" nil t)
        (setq indent (current-indentation))
        (when (idiom--opening-line-p)
          (setq indent (+ indent idiom-indent-offset)))))
    (save-excursion
      (beginning-of-line)
      (when (idiom--closing-line-p)
        (setq indent (- indent idiom-indent-offset))))
    (indent-line-to (max indent 0))))

(defconst idiom--imenu-generic-expression
  '((nil "^\\s-*\\(?:export\\s-+\\)?defn\\s-+\\(\\(?:\\sw\\|\\s_\\)+\\)" 1)
    ("Macros" "^\\s-*\\(?:export\\s-+\\)?defmacro\\s-+\\(\\(?:\\sw\\|\\s_\\)+\\)" 1)
    ("Types" "^\\s-*\\(?:export\\s-+\\)?\\(?:type\\|record\\)\\s-+\\(\\(?:\\sw\\|\\s_\\)+\\)" 1)
    ("Traits" "^\\s-*\\(?:export\\s-+\\)?trait\\s-+\\(\\(?:\\sw\\|\\s_\\)+\\)" 1)
    ("Protocols" "^\\s-*\\(?:export\\s-+\\)?protocol\\s-+\\(\\(?:\\sw\\|\\s_\\)+\\)" 1)))

;;;###autoload
(define-derived-mode idiom-mode prog-mode "Idiom"
  "Major mode for editing Idiom source."
  :syntax-table idiom-mode-syntax-table
  (setq-local comment-start "# ")
  (setq-local comment-start-skip "#+\\s-*")
  (setq-local comment-end "")
  (setq-local font-lock-defaults '(idiom--font-lock-keywords))
  (setq-local indent-line-function #'idiom-indent-line)
  (setq-local electric-indent-chars
              (append "d]})" electric-indent-chars))
  (setq-local imenu-generic-expression idiom--imenu-generic-expression)
  (setq-local tab-width idiom-indent-offset))

;;;###autoload
(add-to-list 'auto-mode-alist '("\\.id\\'" . idiom-mode))

(provide 'idiom-mode)
;;; idiom-mode.el ends here
