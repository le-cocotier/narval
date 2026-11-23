;; ─── Themes ─────────────────────────────────────────────────────────────────
;; (add-package "themes") — CTRL-x c t lists every theme package
;; (theme-dark, theme-nord, ... and any theme-* of your own) and applies
;; each one as you move through the list, so you see your code in it
;; before choosing. ENTER keeps it, and narval opens in it next time;
;; ESC puts back the colors you had.
;;
;;   (theme-pick)             open the list
;;   (theme-apply "nord")     switch for this session, no list
;;
;; Load it after your theme line: the theme you pick here wins over it.
;; A theme is any package named theme-<name> whose init.lisp calls
;; (editor-set-theme-color role r g b); see packages/theme-dark.

(define (themes--join dir name) (string-append dir "/" name))

(define (themes--prefixed? name)
  (and (> (string-length name) 6) (string=? (substring name 0 6) "theme-")))

;; (name . directory) for each theme-* package; one found in your packages
;; folder hides a bundled one of the same name.
(define (themes--in dir)
  (if (not (string? dir))
      '()
      (let ((entries (list-directory dir)))
        (if (not (pair? entries))
            '()
            (map (lambda (entry) (cons (substring (car entry) 6 (string-length (car entry)))
                                       (themes--join dir (car entry))))
                 (filter (lambda (entry) (and (cdr entry) (themes--prefixed? (car entry))))
                         entries))))))

(define (themes--all)
  (let* ((own (themes--in (package-directory)))
         (bundled (filter (lambda (t) (not (assoc (car t) own)))
                          (themes--in (bundled-package-directory)))))
    (sort (append own bundled) (lambda (a b) (string<? (car a) (car b))))))

;; the description in a theme's package.lisp, "" without one
(define (themes--description dir)
  (let ((forms (read-data-file (themes--join dir "package.lisp"))))
    (if (not (pair? forms))
        ""
        (let ((desc (filter (lambda (f) (and (pair? f) (eq? (car f) 'description)))
                            (if (pair? (car forms)) (cdr (cdr (car forms))) '()))))
          (if (and (pair? desc) (pair? (cdr (car desc))) (string? (car (cdr (car desc)))))
              (car (cdr (car desc)))
              "")))))

(define (themes--pad s width)
  (if (< (string-length s) width) (themes--pad (string-append s " ") width) s))

;; The theme in use: the one picked last time, else the first theme package
;; the config loaded.
(define (themes--saved-path) (themes--join (narval-directory) "theme"))

(define (themes--saved)
  (let ((forms (read-data-file (themes--saved-path))))
    (if (and (pair? forms) (string? (car forms))) (car forms) #f)))

(define themes--current
  (let ((loaded (filter (lambda (t) (package-loaded? (string-append "theme-" (car t))))
                        (themes--all))))
    (if (pair? loaded) (car (car loaded)) #f)))

(define (theme-apply name)
  (let ((theme (assoc name (themes--all))))
    (if (not theme)
        (begin (editor-message (string-append "No theme package theme-" name)) #f)
        (begin (editor-load-file (themes--join (cdr theme) "init.lisp"))
               (set! themes--current name)
               #t))))

(define (theme-pick)
  (let* ((before (editor-theme-colors))
         (current themes--current)
         (themes (themes--all))
         ;; the theme in use first, so opening the list changes nothing
         (ordered (if (and current (assoc current themes))
                      (cons (assoc current themes)
                            (filter (lambda (t) (not (string=? (car t) current))) themes))
                      themes))
         (items (map (lambda (t)
                       (cons (string-append (themes--pad (car t) 17)
                                            (themes--description (cdr t)))
                             (car t)))
                     ordered)))
    (editor-list-popup
     "Theme"
     items
     (lambda (name)
       (theme-apply name)
       (write-data-file (themes--saved-path) (list name)
                        ";; The theme picked with CTRL-x c t (the themes package).")
       (editor-message (string-append "Theme: " name " (narval opens in it from now on)")))
     (lambda ()
       (for-each (lambda (c) (editor-set-theme-color (car c) (car (cdr c))
                                                     (car (cdr (cdr c)))
                                                     (car (cdr (cdr (cdr c))))))
                 before)
       (set! themes--current current))
     #t
     theme-apply)))

;; What was picked last time wins over the config's theme line.
(let ((saved (themes--saved)))
  (if (and saved (not (equal? saved themes--current)))
      (theme-apply saved)))

(map-key "t" theme-pick "config")
