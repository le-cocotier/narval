;; ─── Dashboard ──────────────────────────────────────────────────────────────
;; (add-package "dashboard") — a start screen for the current project, in a
;; read-only *dashboard* buffer. It opens by itself when narval starts inside
;; a project with no file to open (dashboard-on-start), when a project is
;; picked with project-switch, CTRL-x p p (dashboard-on-switch, in find-file's
;; place), and with CTRL-x D.
;;
;; From top to bottom (dashboard-sections):
;;
;;   project    name, root, whether it's trusted and has a .narval/init.lisp
;;   git        branch, ahead/behind, the changed files
;;   commands   build / test / run, from dashboard-commands or found from the
;;              project's files (meson, cmake, make, cargo, npm, go, pytest)
;;   recent     the files of this project opened last
;;   todos      the first TODO / FIXME lines (dashboard-todo-patterns)
;;   status     language servers running, and the debugger
;;   projects   the other projects, most recent first
;;
;; In *dashboard* (sticky "dashboard" prefix):
;;
;;   arrows, n / p   move               ENTER   open the file / run / switch
;;   TAB / ] / [     next / previous section
;;   1 ... 9         run that command   b t r c  run the command with that key
;;   g               refresh            q        close the dashboard
;;
;; Commands run in a *run* terminal pane at the project root, keys staying
;; in the dashboard.
;;
;; SETTINGS. Each one is read with project-setting, so a project's own
;; .narval/init.lisp can change it for that project alone:
;;
;;   (define dashboard-commands
;;     '(("b" "Build" "make -j8")
;;       ("t" "Test" "make test")
;;       ("f" "Flash" "make flash")))   ; #f: found from the project's files
;;   (define dashboard-on-start #f)      ; don't replace the start screen
;;   (define dashboard-on-switch #f)     ; project-switch opens find-file
;;   (define dashboard-sections '("project" "git" "commands" "todos"))
;;   (define dashboard-recent-count 10)
;;   (define dashboard-todo-count 5)     ; 0 hides the TODOs
;;   (define dashboard-todo-patterns '("TODO" "FIXME" "XXX"))
;;
;; PACKAGES add a section of their own with
;;
;;   (dashboard-add-section "name" "Title" (lambda (root) rows))
;;
;; called with the project's root ("" outside any project) each time the
;; dashboard is drawn. It returns a list of rows, '() to leave the section
;; out. A row is a string (just text), or (label . action) where action is
;;   "path"            open that file
;;   ("path" line)     open it at that line (1-based)
;;   a procedure       called with no arguments, e.g. (lambda () (stm32-flash))
;; The new section goes at the end of dashboard-sections; define that list
;; to choose where it goes.

;; ─── settings ─────────────────────────────────────────────────────────────

(define dashboard-on-start #t)
(define dashboard-on-switch #t)
(define dashboard-sections '("project" "git" "commands" "recent" "todos" "status" "projects"))
(define dashboard-commands #f)
(define dashboard-recent-count 8)
(define dashboard-todo-count 8)
(define dashboard-todo-patterns '("TODO" "FIXME"))
(define dashboard-git-count 8)
(define dashboard-project-count 5)

(define dashboard-buffer-name "*dashboard*")
(define dashboard-run-terminal "*run*")
(define dashboard-recent-max 200)  ; files remembered, all projects together

;; ─── small helpers ────────────────────────────────────────────────────────

(define (dashboard--setting name) (project-setting name))

(define (dashboard--starts-with? s prefix)
  (and (>= (string-length s) (string-length prefix))
       (string=? (substring s 0 (string-length prefix)) prefix)))

(define (dashboard--ends-with? s suffix)
  (and (>= (string-length s) (string-length suffix))
       (string=? (substring s (- (string-length s) (string-length suffix)) (string-length s))
                 suffix)))

(define (dashboard--join a b) (string-append a "/" b))

(define (dashboard--exists? path) (if (file-attributes path) #t #f))

(define (dashboard--take lst n)
  (if (or (null? lst) (<= n 0))
      '()
      (cons (car lst) (dashboard--take (cdr lst) (- n 1)))))

(define (dashboard--lines s)
  (if (string=? s "") '() (string-split s "\n")))

(define (dashboard--trim-right s)
  (let ((n (string-length s)))
    (if (and (> n 0) (char-whitespace? (string-ref s (- n 1))))
        (dashboard--trim-right (substring s 0 (- n 1)))
        s)))

(define (dashboard--pad s width)
  (if (< (string-length s) width)
      (dashboard--pad (string-append s " ") width)
      s))

(define (dashboard--clip s width)
  (if (> (string-length s) width)
      (string-append (substring s 0 (- width 1)) "…")
      s))

(define (dashboard--basename path)
  (let ((parts (string-split path "/")))
    (car (last-pair parts))))

;; `path` relative to `root` when it's under it.
(define (dashboard--relative path root)
  (if (dashboard--starts-with? path (string-append root "/"))
      (substring path (+ (string-length root) 1) (string-length path))
      path))

;; Under ~, as ~/...
(define (dashboard--tilde path)
  (let ((home (expand-path "~")))
    (if (and (not (string=? home "")) (dashboard--starts-with? path (string-append home "/")))
        (string-append "~" (substring path (string-length home) (string-length path)))
        path)))

;; ─── the sections ─────────────────────────────────────────────────────────
;; Each is (name title . function); dashboard-add-section adds more.

(define dashboard--registry '())

(define (dashboard-add-section name title fn)
  (let ((entry (assoc name dashboard--registry)))
    (if entry
        (set-cdr! entry (cons title fn))
        (set! dashboard--registry (append dashboard--registry (list (cons name (cons title fn)))))))
  (if (not (member name dashboard-sections))
      (set! dashboard-sections (append dashboard-sections (list name))))
  name)

;; project ------------------------------------------------------------------

(define (dashboard--section-project root)
  (if (string=? root "")
      (list "Not in a project: open a file inside one, or add one (CTRL-x p n)."
            (cons "Pick a project (CTRL-x p p)" (lambda () (project-switch))))
      (let* ((config (dashboard--join root ".narval/init.lisp"))
             (has-config (dashboard--exists? config))
             (trusted (project-trusted?)))
        (list (string-append "Root      " (dashboard--tilde root))
              (cond ((and has-config trusted)
                     (cons "Config    .narval/init.lisp (trusted)" config))
                    (has-config
                     (cons "Config    .narval/init.lisp, not loaded: ENTER trusts the project"
                           (lambda () (project-trust) (dashboard-refresh))))
                    (else
                     (cons "Config    none yet: ENTER creates .narval/init.lisp"
                           (lambda () (project-open-config)))))))))

;; git ----------------------------------------------------------------------

;; "## main...origin/main [ahead 1, behind 2]" -> "main ⇄ origin/main [ahead 1, behind 2]"
(define (dashboard--branch-line line)
  (let* ((s (substring line 3 (string-length line)))
         (parts (string-split s "..." 2)))
    (if (pair? (cdr parts))
        (string-append (car parts) " ⇄ " (car (cdr parts)))
        s)))

;; "XY path" or "R  old -> new" -> the path now
(define (dashboard--status-path line)
  (let* ((path (substring line 3 (string-length line)))
         (renamed (string-split path " -> " 2)))
    (if (pair? (cdr renamed)) (car (cdr renamed)) path)))

(define (dashboard--git-status-command)
  (if (package-loaded? "git")
      (list (cons "Open git status (CTRL-x g)" (lambda () (git-status))))
      '()))

(define (dashboard--section-git root)
  (if (or (string=? root "") (not (find-program "git")))
      '()
      (let ((result (process-run "git" "-C" root "-c" "core.quotePath=false"
                                 "status" "--porcelain=v1" "-b")))
        (if (not (= (car result) 0))
            '()
            (let* ((lines (dashboard--lines (cdr result)))
                   (branch (if (and (pair? lines) (dashboard--starts-with? (car lines) "## "))
                               (dashboard--branch-line (car lines))
                               "?"))
                   (changes (if (and (pair? lines) (dashboard--starts-with? (car lines) "## "))
                                (cdr lines)
                                lines))
                   (count (length changes))
                   (shown (dashboard--take changes dashboard-git-count))
                   (rows (map (lambda (line)
                                (let ((path (dashboard--status-path line)))
                                  (cons (string-append (substring line 0 2) "  " path)
                                        (if (dashboard--starts-with? line " D")
                                            #f
                                            (dashboard--join root path)))))
                              shown)))
              (append
               (list (string-append "Branch    " branch)
                     (string-append "Changes   "
                                    (if (= count 0)
                                        "none, the tree is clean"
                                        (string-append (number->string count)
                                                       (if (= count 1) " file" " files")))))
               rows
               (if (> count (length shown))
                   (list (string-append "… " (number->string (- count (length shown))) " more"))
                   '())
               (dashboard--git-status-command)))))))

;; commands -----------------------------------------------------------------

(define (dashboard--has? root name) (dashboard--exists? (dashboard--join root name)))

;; What the project's own files say it's built with.
(define (dashboard--detect-commands root)
  (cond ((dashboard--has? root "meson.build")
         (if (dashboard--has? root "build/build.ninja")
             '(("b" "Build" "meson compile -C build")
               ("t" "Test" "meson test -C build"))
             '(("b" "Configure" "meson setup build")
               ("t" "Test" "meson test -C build"))))
        ((dashboard--has? root "CMakeLists.txt")
         (if (dashboard--has? root "build/CMakeCache.txt")
             '(("b" "Build" "cmake --build build")
               ("t" "Test" "ctest --test-dir build --output-on-failure"))
             '(("b" "Configure" "cmake -S . -B build")
               ("t" "Test" "ctest --test-dir build --output-on-failure"))))
        ((or (dashboard--has? root "Makefile") (dashboard--has? root "makefile")
             (dashboard--has? root "GNUmakefile"))
         '(("b" "Build" "make")
           ("t" "Test" "make test")
           ("c" "Clean" "make clean")))
        ((dashboard--has? root "Cargo.toml")
         '(("b" "Build" "cargo build")
           ("t" "Test" "cargo test")
           ("r" "Run" "cargo run")))
        ((dashboard--has? root "go.mod")
         '(("b" "Build" "go build ./...")
           ("t" "Test" "go test ./...")))
        ((dashboard--has? root "package.json")
         '(("b" "Build" "npm run build")
           ("t" "Test" "npm test")
           ("r" "Run" "npm start")))
        ((or (dashboard--has? root "pyproject.toml") (dashboard--has? root "setup.py"))
         '(("t" "Test" "python -m pytest")))
        (else '())))

(define (dashboard--commands root)
  (if (string=? root "")
      '()
      (let ((set (dashboard--setting "dashboard-commands")))
        (if (pair? set) set (dashboard--detect-commands root)))))

(define (dashboard-run command)
  (let ((root (project-root)))
    (editor-message (string-append "Running " command))
    (if (not (terminal-run dashboard-run-terminal command
                           (if (string=? root "") #f root) #f))
        (editor-message (string-append "Couldn't start " command)))))

;; The command bound to `key` in the current project, by its key or (1-9)
;; by its place in the list.
(define (dashboard-run-key key)
  (let* ((commands (dashboard--commands (project-root)))
         (entry (assoc key commands))
         (n (string->number key)))
    (cond (entry (dashboard-run (car (cdr (cdr entry)))))
          ((and n (> n 0) (<= n (length commands)))
           (dashboard-run (car (cdr (cdr (list-ref commands (- n 1)))))))
          (else (editor-message (string-append "No command on " key))))))

(define (dashboard--section-commands root)
  (let ((commands (dashboard--commands root))
        (i 0))
    (if (null? commands)
        (if (string=? root "")
            '()
            (list "Nothing found: (define dashboard-commands '((\"b\" \"Build\" \"make\")))"))
        (map (lambda (entry)
               (set! i (+ i 1))
               (let ((key (car entry))
                     (label (car (cdr entry)))
                     (command (car (cdr (cdr entry)))))
                 (cons (string-append (number->string i) " " key "  "
                                      (dashboard--pad label 12) command)
                       (lambda () (dashboard-run command)))))
             commands))))

;; recent files -------------------------------------------------------------
;; Every file opened, most recent first, in <narval-directory>/recent-files;
;; the dashboard shows the current project's.

(define dashboard--recent #f)  ; read on first use

(define (dashboard--recent-path) (dashboard--join (narval-directory) "recent-files"))

(define (dashboard--recent-files)
  (if (not dashboard--recent)
      (let ((forms (read-data-file (dashboard--recent-path))))
        (set! dashboard--recent (if (pair? forms) (filter string? forms) '()))))
  dashboard--recent)

(define (dashboard--remember path)
  (let ((others (filter (lambda (p) (not (string=? p path))) (dashboard--recent-files))))
    (set! dashboard--recent (dashboard--take (cons path others) dashboard-recent-max))
    (write-data-file (dashboard--recent-path) dashboard--recent
                     ";; Files narval opened, most recent first (the dashboard package).")))

(define (dashboard--splash? path)
  (or (dashboard--ends-with? path "assets/narval.txt")
      (dashboard--ends-with? path "assets\\narval.txt")))

(add-hook "buffer-opened"
          (lambda (path id)
            (if (and (string? path) (not (string=? path "")) (not (dashboard--splash? path)))
                (dashboard--remember path))))

(define (dashboard--section-recent root)
  (if (string=? root "")
      '()
      (let* ((mine (filter (lambda (p) (dashboard--starts-with? p (string-append root "/")))
                           (dashboard--recent-files)))
             (shown (dashboard--take (filter dashboard--exists? mine)
                                     (dashboard--setting "dashboard-recent-count"))))
        (if (null? shown)
            (list "None yet")
            (map (lambda (p) (cons (dashboard--relative p root) p)) shown)))))

;; TODOs --------------------------------------------------------------------

(define (dashboard--todo-hits root patterns n)
  (if (or (null? patterns) (<= n 0))
      '()
      (let ((hits (project-grep (car patterns) root n)))
        (append hits (dashboard--todo-hits root (cdr patterns) (- n (length hits)))))))

(define (dashboard--section-todos root)
  (let ((n (dashboard--setting "dashboard-todo-count"))
        (patterns (dashboard--setting "dashboard-todo-patterns")))
    (if (or (string=? root "") (not (number? n)) (<= n 0) (not (pair? patterns)))
        '()
        (let ((hits (dashboard--todo-hits root patterns (+ n 1))))
          (if (null? hits)
              (list "None")
              (append
               (map (lambda (hit)
                      (let ((path (car hit)) (line (car (cdr hit))) (text (car (cdr (cdr hit)))))
                        (cons (string-append (dashboard--relative path root) ":"
                                             (number->string line) "  " (dashboard--clip text 70))
                              (list path line))))
                    (dashboard--take hits n))
               (if (> (length hits) n)
                   (list (cons (string-append "… every " (car patterns) " in *search*")
                               (lambda () (project-search (car patterns)))))
                   '())))))))

;; status -------------------------------------------------------------------

(define (dashboard--section-status root)
  (let ((servers (lsp-clients))
        (debug (debug-state)))
    (append
     (if (null? servers)
         (list "Language  no server running yet (one starts with a file of its language)")
         (map (lambda (s)
                (string-append "Language  " (car s) " (" (car (cdr s)) "): " (car (cdr (cdr s)))))
              servers))
     (list (if (string=? debug "off")
               "Debugger  off (CTRL-x B)"
               (cons (string-append "Debugger  " debug ": ENTER shows the panel")
                     (lambda () (debug-panel))))))))

;; projects -----------------------------------------------------------------

(define (dashboard--section-projects root)
  (let ((others (dashboard--take (filter (lambda (p) (not (string=? p root))) (project-list))
                                 dashboard-project-count)))
    (map (lambda (p)
           (cons (string-append (dashboard--pad (dashboard--basename p) 20) (dashboard--tilde p))
                 (lambda () (project-activate p) (dashboard))))
         others)))

(dashboard-add-section "project" "Project" dashboard--section-project)
(dashboard-add-section "git" "Git" dashboard--section-git)
(dashboard-add-section "commands" "Commands" dashboard--section-commands)
(dashboard-add-section "recent" "Recent files" dashboard--section-recent)
(dashboard-add-section "todos" "TODOs" dashboard--section-todos)
(dashboard-add-section "status" "Status" dashboard--section-status)
(dashboard-add-section "projects" "Other projects" dashboard--section-projects)

;; ─── drawing ──────────────────────────────────────────────────────────────

(define dashboard-buffer-id -1)
(define dashboard--actions (list->vector '()))  ; per row: the action, or #f
(define dashboard--headers '())                 ; rows of the section titles

;; Headers and the title are coloured by their first characters.
(syntax-define-language "dashboard" "" "" "" "")
(syntax-add-line-prefix "dashboard" "◆ " "type")
(syntax-add-line-prefix "dashboard" "▸ " "keyword")
(syntax-add-line-prefix "dashboard" "  keys " "comment")

(define (dashboard--row row)
  (if (pair? row)
      (cons (string-append "  " (car row)) (cdr row))
      (cons (string-append "  " row) #f)))

;; The section's rows, or '() to leave it out.
(define (dashboard--build name root)
  (let ((entry (assoc name dashboard--registry)))
    (if (not entry)
        '()
        (let ((rows ((cdr (cdr entry)) root)))
          (if (pair? rows)
              (cons (cons (string-append "▸ " (car (cdr entry))) #f)
                    (map dashboard--row rows))
              '())))))

(define (dashboard--all-rows root)
  (let ((sections (dashboard--setting "dashboard-sections"))
        (title (if (string=? root "") "narval" (project-name))))
    (append
     (list (cons (string-append "◆ " title) #f)
           (cons "  keys  ENTER open/run · 1-9 run · TAB section · g refresh · q close" #f))
     (apply append
            (map (lambda (name)
                   (let ((rows (dashboard--build name root)))
                     (if (null? rows) '() (cons (cons "" #f) rows))))
                 (if (pair? sections) sections '()))))))

(define (dashboard--first-action actions i)
  (cond ((>= i (vector-length actions)) 0)
        ((vector-ref actions i) i)
        (else (dashboard--first-action actions (+ i 1)))))

(define (dashboard--header-rows rows i)
  (cond ((null? rows) '())
        ((dashboard--starts-with? (car (car rows)) "▸ ")
         (cons i (dashboard--header-rows (cdr rows) (+ i 1))))
        (else (dashboard--header-rows (cdr rows) (+ i 1)))))

;; Fills *dashboard*; the cursor stays on its row (`keep-row` #f: lands on
;; the first row that does something).
(define (dashboard--draw keep-row)
  (let* ((root (project-root))
         (rows (dashboard--all-rows root)))
    (set! dashboard--actions (list->vector (map cdr rows)))
    (set! dashboard--headers (dashboard--header-rows rows 0))
    (editor-scratch-set-lines dashboard-buffer-id (map car rows))
    (editor-goto-line (if (and keep-row (< keep-row (length rows)))
                          keep-row
                          (dashboard--first-action dashboard--actions 0)))))

(define (dashboard--live?)
  (and (>= dashboard-buffer-id 0) (= (current-buffer-id) dashboard-buffer-id)))

;; Opens the dashboard of the current project in the active pane.
(define (dashboard)
  (set! dashboard-buffer-id (editor-scratch-open dashboard-buffer-name))
  (buffer-set-syntax dashboard-buffer-id "dashboard")
  (editor-switch-to-buffer dashboard-buffer-id)
  (editor-scratch-set-prefix dashboard-buffer-id "dashboard")
  (dashboard--draw #f))

(define (dashboard-refresh)
  (if (dashboard--live?)
      (dashboard--draw (cursor-row))
      (dashboard)))

(define (dashboard-close)
  (if (>= dashboard-buffer-id 0)
      (editor-kill-buffer dashboard-buffer-id)))

(add-hook "buffer-killed"
          (lambda (name id)
            (if (= id dashboard-buffer-id) (set! dashboard-buffer-id -1))))

;; ─── acting ───────────────────────────────────────────────────────────────

(define (dashboard--open path line)
  (editor-set-prefix "no-prefix")
  (editor-open-file-path path)
  (if line (editor-goto-line (- line 1))))

(define (dashboard-activate)
  (let* ((row (cursor-row))
         (action (if (< row (vector-length dashboard--actions))
                     (vector-ref dashboard--actions row)
                     #f)))
    (cond ((not action) '())
          ((string? action) (dashboard--open action #f))
          ((pair? action) (dashboard--open (car action) (car (cdr action))))
          (else (action)))))

(define (dashboard--next-header headers row)
  (cond ((null? headers) #f)
        ((> (car headers) row) (car headers))
        (else (dashboard--next-header (cdr headers) row))))

(define (dashboard--prev-header headers row best)
  (cond ((null? headers) best)
        ((< (car headers) row) (dashboard--prev-header (cdr headers) row (car headers)))
        (else best)))

;; The first row after the section title (the title itself if it's last).
(define (dashboard--goto-section header)
  (if header
      (editor-goto-line (if (< (+ header 1) (vector-length dashboard--actions))
                            (+ header 1)
                            header))))

(define (dashboard-next-section)
  (let ((next (dashboard--next-header dashboard--headers (cursor-row))))
    (dashboard--goto-section (if next next (if (pair? dashboard--headers) (car dashboard--headers) #f)))))

(define (dashboard-previous-section)
  ;; From inside a section, its own title is the one before the cursor:
  ;; skip it, so [ goes to the section above.
  (let* ((row (cursor-row))
         (own (dashboard--prev-header dashboard--headers row #f))
         (prev (if own (dashboard--prev-header dashboard--headers own #f) #f)))
    (dashboard--goto-section (if prev prev own))))

;; ─── keys ─────────────────────────────────────────────────────────────────

(add-prefix "dashboard" #t)
(map-key "ENTER" dashboard-activate "dashboard")
(map-key "ARROW-UP" '(move-cursor "up") "dashboard")
(map-key "ARROW-DOWN" '(move-cursor "down") "dashboard")
(map-key "p" '(move-cursor "up") "dashboard")
(map-key "n" '(move-cursor "down") "dashboard")
(map-key "PAGE-UP" move-cursor-page-up "dashboard")
(map-key "PAGE-DOWN" move-cursor-page-down "dashboard")
(map-key "TAB" dashboard-next-section "dashboard")
(map-key "]" dashboard-next-section "dashboard")
(map-key "[" dashboard-previous-section "dashboard")
(map-key "g" dashboard-refresh "dashboard")
(map-key "q" dashboard-close "dashboard")
(map-key "1" '(dashboard-run-key "1") "dashboard")
(map-key "2" '(dashboard-run-key "2") "dashboard")
(map-key "3" '(dashboard-run-key "3") "dashboard")
(map-key "4" '(dashboard-run-key "4") "dashboard")
(map-key "5" '(dashboard-run-key "5") "dashboard")
(map-key "6" '(dashboard-run-key "6") "dashboard")
(map-key "7" '(dashboard-run-key "7") "dashboard")
(map-key "8" '(dashboard-run-key "8") "dashboard")
(map-key "9" '(dashboard-run-key "9") "dashboard")
(map-key "b" '(dashboard-run-key "b") "dashboard")
(map-key "t" '(dashboard-run-key "t") "dashboard")
(map-key "r" '(dashboard-run-key "r") "dashboard")
(map-key "c" '(dashboard-run-key "c") "dashboard")
(map-key "ESCAPE" '(editor-set-prefix "no-prefix") "dashboard")
(map-key "CTRL-x" '(editor-set-prefix "user") "dashboard")
(map-key "D" dashboard "user")

;; ─── at startup ───────────────────────────────────────────────────────────
;; Started inside a project with no file to open, the dashboard takes the
;; start screen's place.

(add-hook "after-init"
          (lambda ()
            (if (and (dashboard--setting "dashboard-on-start")
                     (not (string=? (project-root) ""))
                     (dashboard--splash? (buffer-filename)))
                (dashboard))))

;; ─── on project-switch ────────────────────────────────────────────────────
;; A project picked with project-switch opens on its dashboard rather than
;; find-file. Read once the project is current, so its own
;; .narval/init.lisp can turn it off.

(add-hook "project-switch"
          (lambda (root)
            (if (dashboard--setting "dashboard-on-switch")
                (dashboard))))
