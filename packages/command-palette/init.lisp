;; ─── Command palette ────────────────────────────────────────────────────────
;; (add-package "command-palette") — ALT-p lists every command, each with
;; the keys that run it, filtered as you type: the letters of the query in
;; order ("ff" finds find-file, "sv" editor-save). ENTER runs the command,
;; ESC closes the list. Like M-x, but you can see what's there, and learn
;; the shortcut on the way.
;;
;;   (command-palette)        open it
;;
;; The key it opens on is an ordinary binding; change it in your config:
;;   (map-key "ALT-p" command-palette "no-prefix")

;; Small list and string helpers (the core Lisp has none of these).
(define (command-palette--keep ok? lst)
  (cond ((null? lst) '())
        ((ok? (car lst)) (cons (car lst) (command-palette--keep ok? (cdr lst))))
        (else (command-palette--keep ok? (cdr lst)))))

;; #t when `s` contains "--" (a package's internal helper)
(define (command-palette--internal-from? s i)
  (cond ((> (+ i 2) (string-length s)) #f)
        ((string=? (substring s i (+ i 2)) "--") #t)
        (else (command-palette--internal-from? s (+ i 1)))))

(define (command-palette--internal? s) (command-palette--internal-from? s 0))

;; buffer-modified? and friends answer a question; they don't do anything
(define (command-palette--predicate? s)
  (let ((n (string-length s)))
    (and (> n 0) (string=? (substring s (- n 1) n) "?"))))

;; The keys that set `prefix`, as typed from no prefix: "CTRL-x p" for
;; "project". #f when nothing reaches it. Several keys can set one prefix
;; (CTRL-x sets "user" from no prefix and from the debugger): the shortest
;; route wins.
(define (command-palette--prefix-keys prefix bindings depth)
  (cond ((string=? prefix "no-prefix") "")
        ((> depth 4) #f)
        (else
         (command-palette--shortest
          (map (lambda (entry)
                 (let ((outer (command-palette--prefix-keys (car entry) bindings (+ depth 1))))
                   (cond ((not outer) #f)
                         ((string=? outer "") (car (cdr entry)))
                         (else (string-append outer " " (car (cdr entry)))))))
               (command-palette--keep
                (lambda (b)
                  (and (not (string=? (car b) "*"))
                       (string=? (car (cdr (cdr b)))
                                 (string-append "editor-set-prefix " prefix))))
                bindings))))))

;; the shortest string in `routes`, skipping #f; #f when there's none
(define (command-palette--shortest routes)
  (cond ((null? routes) #f)
        ((not (car routes)) (command-palette--shortest (cdr routes)))
        (else
         (let ((rest (command-palette--shortest (cdr routes))))
           (if (and rest (< (string-length rest) (string-length (car routes))))
               rest
               (car routes))))))

;; command name -> the shortest full key sequence bound to it
(define (command-palette--keys bindings)
  (let ((table '()))
    (for-each
     (lambda (b)
       (let* ((prefix (car b))
              (key (car (cdr b)))
              (command (car (cdr (cdr b))))
              (outer (if (string=? prefix "*") #f
                         (command-palette--prefix-keys prefix bindings 0)))
              (full (cond ((not outer) #f)
                          ((string=? outer "") key)
                          (else (string-append outer " " key))))
              (known (assoc command table)))
         (if full
             (cond ((not known) (set! table (cons (cons command full) table)))
                   ((< (string-length full) (string-length (cdr known)))
                    (set-cdr! known full))))))
     bindings)
    table))

(define (command-palette--run name)
  (let ((f (eval (string->symbol name))))
    (if (procedure? f)
        (f)
        (editor-message (string-append "Not a command: " name)))))

(define (command-palette)
  (let* ((keys (command-palette--keys (key-bindings)))
         (items (map (lambda (name)
                       (let ((bound (assoc name keys)))
                         (cons (if bound (string-append name "\t" (cdr bound)) name)
                               name)))
                     ;; the palette's own internals aren't commands
                     (command-palette--keep (lambda (name)
                                              (not (or (command-palette--internal? name)
                                                       (command-palette--predicate? name))))
                             (command-names)))))
    (editor-list-popup "Command" items command-palette--run '() "fuzzy")))

(map-key "ALT-p" command-palette "no-prefix")
