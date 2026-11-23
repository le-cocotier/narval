;; ─── Theme: light ───────────────────────────────────────────────────────────
;; (add-package "theme-light") — narval's original light palette, now
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

(editor-set-theme-color "background" 250 251 252)
(editor-set-theme-color "line-cursor-background" 235 236 238)
(editor-set-theme-color "keyword" 166 38 164)
(editor-set-theme-color "type" 4 90 180)
(editor-set-theme-color "string" 80 161 79)
(editor-set-theme-color "comment" 152 152 152)
(editor-set-theme-color "number" 206 102 54)
(editor-set-theme-color "default" 86 89 90)
(editor-set-theme-color "added" 80 161 79)
(editor-set-theme-color "removed" 228 86 73)
(editor-set-theme-color "debug-line-background" 255 243 196)
(editor-set-theme-color "status-bar" 86 89 90)
(editor-set-theme-color "status-bar-background" 225 227 230)
(editor-set-theme-color "status-bar-accent" 250 251 252)
(editor-set-theme-color "status-bar-accent-background" 4 90 180)
