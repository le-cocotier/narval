;; ─── Theme: dark ────────────────────────────────────────────────────────────
;; (add-package "theme-dark") — narval's original default palette, now
;; just an ordinary package instead of a hardcoded C if/else. Fork this
;; file (or write your own from scratch) to build a custom theme: a list of
;; (editor-set-theme-color role r g b) calls is the whole interface.
;;
;; Roles: "background", "line-cursor-background" (the cursor's own line,
;; a subtle tint against "background"), "keyword", "type", "string",
;; "comment", "number", "default" (everything else — plain text,
;; punctuation, identifiers), "added"/"removed" (diff lines),
;; "debug-line-background" (the line the debugger stopped on), and the
;; status bar: "status-bar" / "status-bar-background" for the bar, and
;; "status-bar-accent" / "status-bar-accent-background" for the active
;; buffer's block on its left.

(editor-set-theme-color "background" 40 44 52)
(editor-set-theme-color "line-cursor-background" 60 64 72)
(editor-set-theme-color "keyword" 198 120 221)
(editor-set-theme-color "type" 97 175 239)
(editor-set-theme-color "string" 152 195 121)
(editor-set-theme-color "comment" 92 99 112)
(editor-set-theme-color "number" 209 154 102)
(editor-set-theme-color "default" 171 178 191)
(editor-set-theme-color "added" 152 195 121)
(editor-set-theme-color "removed" 224 108 117)
(editor-set-theme-color "debug-line-background" 77 68 38)
(editor-set-theme-color "status-bar" 171 178 191)
(editor-set-theme-color "status-bar-background" 33 37 43)
(editor-set-theme-color "status-bar-accent" 40 44 52)
(editor-set-theme-color "status-bar-accent-background" 97 175 239)
