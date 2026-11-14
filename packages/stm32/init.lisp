;; ─── STM32 ──────────────────────────────────────────────────────────────────
;; (add-package "stm32") — build, flash, debug and talk to an STM32 board
;; from narval. Loads the "c" package (clangd, gdb) first.
;;
;;   CTRL-x E b   build                     CTRL-x E s   serial monitor
;;   CTRL-x E f   build, then flash         CTRL-x E c   set up clangd
;;   CTRL-x E d   build, flash, debug       CTRL-x E r   reset the board
;;   CTRL-x E a   debug what's running      CTRL-x E i   what was detected
;;                (no build, no reset)      CTRL-x E p   per-project settings
;;
;; THE PROJECT is the current one (CTRL-x p): its root holds a
;; CMakeLists.txt (with or without CMakePresets.json: STM32CubeMX's CMake
;; output has both), a Makefile (CubeMX's Makefile output), or STM32CubeIDE's
;; .cproject with Debug/makefile. The MCU comes from the CubeMX .ioc, else
;; from the STM32xxxxxx define in the build files; it picks openocd's target.
;;
;; BUILD runs in the *stm32* pane, without taking the keys: keep editing.
;; Flash and debug only go on when the build succeeded. The .elf is the
;; newest one in the build directory (build/Debug for CubeMX's presets).
;;
;; FLASH with the first of openocd, st-flash (stlink tools) or
;; STM32_Programmer_CLI that's installed.
;;
;; DEBUG is narval's own debugger (CTRL-x B: breakpoints, stepping, the
;; panel, watches, CTRL-x B g for gdb's own commands such as "monitor reset
;; halt" or "info registers"), with arm-none-eabi-gdb 15 or later (or
;; gdb-multiarch) in its DAP mode. With openocd, gdb starts it itself
;; through a pipe: nothing is left running when the session ends, and the
;; board is reset and halted as gdb connects, so the breakpoints you set
;; before are hit from the start. With st-util, it runs in a *gdbserver*
;; pane. With stm32-gdb-server "remote", gdb connects to stm32-gdb-remote, a
;; server you run yourself (J-Link's on :2331, pyocd's on :3333, ...).
;;
;; SERIAL runs tio, picocom, pyserial's miniterm or screen (the first
;; installed) in a *serial* pane, on the port you pick (the USB ones are
;; listed on Linux and macOS). ESC gives the editor its keys back; CTRL-x k
;; there closes it.
;;
;; CLANGD needs a compile_commands.json (CMake writes it; for a Makefile,
;; `bear` makes one) and the cross compiler's own headers: CTRL-x E c
;; writes a .clangd at the root with --target=arm-none-eabi and
;; arm-none-eabi-gcc's include directories. Open a file again (or restart)
;; for clangd to read it.
;;
;; SETTINGS: define any of these in personal-config, or for one project in
;; its .narval/init.lisp (CTRL-x E p adds them there, commented out):

(define stm32-build-type "Debug")    ; CMake build type / preset, CubeIDE configuration
(define stm32-build-command #f)      ; "shell command" or '("program" "arg" ...); #f: from the project
(define stm32-build-dir #f)          ; relative to the root; #f: build/<type>, build, or <type>
(define stm32-elf #f)                ; relative to the root; #f: the newest .elf in the build dir
(define stm32-toolchain-prefix "arm-none-eabi-")
(define stm32-gdb #f)                ; #f: <prefix>gdb, else gdb-multiarch
(define stm32-flasher #f)            ; "openocd" "st-flash" "cubeprog", or a shell command
                                     ; where {elf} is the .elf; #f: the first installed
(define stm32-gdb-server #f)         ; "openocd" "st-util" "remote"; #f: openocd, else st-util
(define stm32-gdb-remote "localhost:3333") ; where gdb connects with "remote"
(define stm32-openocd-interface "interface/stlink.cfg")
(define stm32-openocd-target #f)     ; e.g. "target/stm32f4x.cfg"; #f: from the MCU
(define stm32-flash-before-debug #t)
(define stm32-serial-port #f)        ; "/dev/ttyACM0", "COM3"; #f: asks
(define stm32-serial-baud 115200)
(define stm32-serial-tool #f)        ; "tio" "picocom" "miniterm" "screen"; #f: the first installed
(define stm32-make-jobs 8)

(define stm32-terminal "*stm32*")

;; ─── small helpers ──────────────────────────────────────────────────────────

;; A setting as the current project has it (its .narval/init.lisp), else
;; as defined above or in personal-config.
(define (stm32--get name) (project-setting name))

(define (stm32--2nd l) (car (cdr l)))
(define (stm32--3rd l) (car (cdr (cdr l))))

(define (stm32--join a b) (string-append a "/" b))

(define (stm32--exists? path) (if (file-attributes path) #t #f))

(define (stm32--starts-with? s prefix)
  (and (>= (string-length s) (string-length prefix))
       (string=? (substring s 0 (string-length prefix)) prefix)))

(define (stm32--ends-with? s suffix)
  (let ((n (string-length s)) (m (string-length suffix)))
    (and (>= n m) (string=? (substring s (- n m) n) suffix))))

(define (stm32--contains? s needle) (pair? (cdr (string-split s needle 2))))

;; The text between the first `needle` and the end of that line, or #f.
(define (stm32--value-after s needle)
  (let ((parts (string-split s needle 2)))
    (if (pair? (cdr parts))
        (stm32--trim (car (string-split (stm32--2nd parts) "\n" 1)))
        #f)))

(define (stm32--trim-left s)
  (if (and (> (string-length s) 0) (char-whitespace? (string-ref s 0)))
      (stm32--trim-left (substring s 1 (string-length s)))
      s))

(define (stm32--trim-right s)
  (let ((n (string-length s)))
    (if (and (> n 0) (char-whitespace? (string-ref s (- n 1))))
        (stm32--trim-right (substring s 0 (- n 1)))
        s)))

(define (stm32--trim s) (stm32--trim-right (stm32--trim-left s)))

;; Each occurrence of `from` in `s` replaced by `to`.
(define (stm32--replace s from to)
  (stm32--join-with (string-split s from) to))

(define (stm32--join-with parts sep)
  (if (null? parts)
      ""
      (fold-left (lambda (acc p) (string-append acc sep p)) (car parts) (cdr parts))))

(define (stm32--number->string v) (if (number? v) (number->string v) v))

(define (stm32--windows?) (string=? (system-type) "windows"))

(define (stm32--root)
  (let ((root (project-root)))
    (if (string=? root "") (expand-path ".") root)))

;; A path from a setting: relative ones start at the root.
(define (stm32--resolve path root) (expand-path path root))

;; `path` relative to `root` when it's under it ("." for root itself).
(define (stm32--relative path root)
  (cond ((string=? path root) ".")
        ((stm32--starts-with? path (string-append root "/"))
         (substring path (+ (string-length root) 1) (string-length path)))
        (else path)))

(define (stm32--files-ending dir suffix)
  (map (lambda (entry) (stm32--join dir (car entry)))
       (filter (lambda (entry) (and (not (cdr entry)) (stm32--ends-with? (car entry) suffix)))
               (list-directory dir))))

(define (stm32--mtime path)
  (let ((attrs (file-attributes path)))
    (if attrs (list-ref attrs 3) 0)))

(define (stm32--newest paths)
  (if (null? paths)
      #f
      (fold-left (lambda (best p) (if (> (stm32--mtime p) (stm32--mtime best)) p best))
                 (car paths) (cdr paths))))

;; ─── the project ────────────────────────────────────────────────────────────

;; "cmake-presets", "cmake", "make", "cubeide", or #f.
(define (stm32--kind root)
  (cond ((stm32--exists? (stm32--join root "CMakeLists.txt"))
         (if (stm32--exists? (stm32--join root "CMakePresets.json")) "cmake-presets" "cmake"))
        ((stm32--exists? (stm32--join root "Makefile")) "make")
        ((stm32--exists? (stm32--join root "makefile")) "make")
        ((stm32--exists? (stm32--join root ".cproject")) "cubeide")
        (else #f)))

(define (stm32--ioc root) (stm32--newest (stm32--files-ending root ".ioc")))

;; ("STM32F407VGTx" . "F4"): the MCU and its family's letters, from the
;; .ioc, else from a define such as STM32F407xx in the build files. #f when
;; neither says.
(define (stm32--mcu root)
  (let* ((ioc (stm32--ioc root))
         (text (if ioc (read-file ioc) #f))
         (family (if text (stm32--value-after text "Mcu.Family=") #f))
         (name (if text (or (stm32--value-after text "Mcu.UserName=")
                            (stm32--value-after text "Mcu.Name=")) #f)))
    (if (and family (stm32--starts-with? family "STM32"))
        (cons (or name family) (substring family 5 (string-length family)))
        (stm32--mcu-from-defines root
                                 (list "cmake/stm32cubemx/CMakeLists.txt" "CMakeLists.txt"
                                       "Makefile" "makefile" ".cproject")))))

(define (stm32--mcu-from-defines root files)
  (if (null? files)
      #f
      (let* ((text (read-file (stm32--join root (car files)) 1048576))
             (found (if text (stm32--define-in (cdr (string-split text "STM32" 400))) #f)))
        (or found (stm32--mcu-from-defines root (cdr files))))))

;; The pieces after each "STM32": the first like "F407xx ..." is a device
;; define, the family the two letters after STM32.
(define (stm32--define-in pieces)
  (cond ((null? pieces) #f)
        ((stm32--device-define? (car pieces))
         (let ((p (car pieces)))
           (cons (string-append "STM32" (stm32--word p 0))
                 (string-upcase (substring p 0 2)))))
        (else (stm32--define-in (cdr pieces)))))

;; `s` from `i` up to the first character that isn't a letter or digit.
(define (stm32--word s i)
  (if (and (< i (string-length s)) (char-alphanumeric? (string-ref s i)))
      (stm32--word s (+ i 1))
      (substring s 0 i)))

(define (stm32--device-define? p)
  (and (>= (string-length p) 6)
       (char-alphabetic? (string-ref p 0))
       (char-upper-case? (string-ref p 0))
       (char-numeric? (string-ref p 2))
       (stm32--contains? (substring p 0 (min 9 (string-length p))) "x")))

(define stm32--openocd-targets
  '(("C0" . "stm32c0x") ("F0" . "stm32f0x") ("F1" . "stm32f1x") ("F2" . "stm32f2x")
    ("F3" . "stm32f3x") ("F4" . "stm32f4x") ("F7" . "stm32f7x") ("G0" . "stm32g0x")
    ("G4" . "stm32g4x") ("H5" . "stm32h5x") ("H7" . "stm32h7x") ("L0" . "stm32l0")
    ("L1" . "stm32l1") ("L4" . "stm32l4x") ("L5" . "stm32l5x") ("U0" . "stm32u0x")
    ("U5" . "stm32u5x") ("WB" . "stm32wbx") ("WL" . "stm32wlx")))

(define (stm32--openocd-target mcu)
  (or (stm32--get "stm32-openocd-target")
      (and mcu
           (let ((hit (assoc (cdr mcu) stm32--openocd-targets)))
             (and hit (string-append "target/" (cdr hit) ".cfg"))))))

(define (stm32--build-dir root kind)
  (let ((set (stm32--get "stm32-build-dir"))
        (type (stm32--get "stm32-build-type")))
    (cond (set (stm32--resolve set root))
          ((equal? kind "cmake-presets") (stm32--join root (string-append "build/" type)))
          ((equal? kind "cubeide") (stm32--join root type))
          (else (stm32--join root "build")))))

(define (stm32--elf root kind)
  (let ((set (stm32--get "stm32-elf")))
    (if set
        (stm32--resolve set root)
        (stm32--newest (append (stm32--files-ending (stm32--build-dir root kind) ".elf")
                               (stm32--files-ending (stm32--join root "build") ".elf"))))))

;; ─── running things one after the other ─────────────────────────────────────
;; A step is (run "what" command): command (a shell string or an argv
;; list) in the *stm32* pane, the next step only once it exits with 0;
;; (server "pane" argv): started in a pane of its own, left running;
;; (delay seconds); (call thunk). A new run replaces the one going on.

(define stm32--steps '())
(define stm32--waiting #f)    ; a (run ...) step is going on in *stm32*
(define stm32--current "")    ; its "what"
(define stm32--gen 0)

(define (stm32--run steps)
  (set! stm32--gen (+ stm32--gen 1))
  (set! stm32--steps steps)
  (set! stm32--waiting #f)
  (stm32--next))

(define (stm32--fail message)
  (set! stm32--steps '())
  (set! stm32--waiting #f)
  (editor-message message))

(define (stm32--next)
  (if (pair? stm32--steps)
      (let ((step (car stm32--steps)))
        (set! stm32--steps (cdr stm32--steps))
        (stm32--do step))))

(define (stm32--do step)
  (let ((kind (car step)))
    (cond ((eq? kind 'run)
           (set! stm32--current (stm32--2nd step))
           (editor-message (string-append (stm32--2nd step) "..."))
           (if (terminal-run stm32-terminal (stm32--3rd step) (stm32--root) #f)
               (set! stm32--waiting #t)
               (stm32--fail (string-append "Couldn't start " (stm32--command-name (stm32--3rd step))))))
          ((eq? kind 'server)
           (if (terminal-run (stm32--2nd step) (stm32--3rd step) (stm32--root) #f)
               (stm32--next)
               (stm32--fail (string-append "Couldn't start " (stm32--command-name (stm32--3rd step))))))
          ((eq? kind 'delay) (stm32--delay (stm32--2nd step)))
          ((eq? kind 'call) ((stm32--2nd step)) (stm32--next)))))

(define (stm32--command-name command)
  (if (string? command) command (car command)))

(define (stm32--delay seconds)
  (let ((gen stm32--gen))
    (apply process-start
           (cons (lambda (result) (if (= gen stm32--gen) (stm32--next)))
                 (if (stm32--windows?)
                     (list "powershell" "-NoProfile" "-Command"
                           (string-append "Start-Sleep -Seconds " (number->string seconds)))
                     (list "sleep" (number->string seconds)))))))

(add-hook "terminal-exited"
          (lambda (name code)
            (if (and stm32--waiting (string=? name stm32-terminal))
                (begin
                  (set! stm32--waiting #f)
                  (cond ((not (= code 0))
                         (stm32--fail (string-append stm32--current " failed (exit code "
                                                     (number->string code) "): see "
                                                     stm32-terminal)))
                        ((pair? stm32--steps) (stm32--next))
                        (else (editor-message (string-append stm32--current ": done"))))))))

;; ─── build ──────────────────────────────────────────────────────────────────

(define (stm32--jobs) (stm32--number->string (stm32--get "stm32-make-jobs")))

;; The build's steps, or #f (and says why) when there's nothing to build.
(define (stm32--build-steps root kind)
  (let ((custom (stm32--get "stm32-build-command"))
        (type (stm32--get "stm32-build-type"))
        (dir (stm32--build-dir root kind)))
    (cond (custom (list (list 'run "Build" custom)))
          ((equal? kind "cmake-presets")
           (append (if (stm32--exists? (stm32--join dir "CMakeCache.txt"))
                       '()
                       (list (list 'run "Configure" (list "cmake" "--preset" type))))
                   (list (list 'run "Build" (list "cmake" "--build" "--preset" type)))))
          ((equal? kind "cmake")
           (append (if (stm32--exists? (stm32--join dir "CMakeCache.txt"))
                       '()
                       (list (list 'run "Configure" (stm32--cmake-configure root dir type))))
                   (list (list 'run "Build" (list "cmake" "--build" dir "-j" (stm32--jobs))))))
          ((equal? kind "make")
           (list (list 'run "Build" (list "make" (string-append "-j" (stm32--jobs))))))
          ((equal? kind "cubeide")
           (if (stm32--exists? (stm32--join root (string-append type "/makefile")))
               (list (list 'run "Build" (list "make" (string-append "-j" (stm32--jobs))
                                              "-C" type "all")))
               (begin (editor-message (string-append "No " type "/makefile: build once in STM32CubeIDE, or set stm32-build-command"))
                      #f)))
          (else
           (editor-message (string-append "No CMakeLists.txt, Makefile or .cproject in " root
                                          ": set stm32-build-command"))
           #f))))

(define (stm32--cmake-configure root dir type)
  (let ((toolchain (stm32--join root "cmake/gcc-arm-none-eabi.cmake")))
    (append (list "cmake" "-S" root "-B" dir
                  (string-append "-DCMAKE_BUILD_TYPE=" type)
                  "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON")
            (if (find-program "ninja") (list "-G" "Ninja") '())
            (if (stm32--exists? toolchain)
                (list (string-append "-DCMAKE_TOOLCHAIN_FILE=" toolchain))
                '()))))

(define (stm32-build)
  (let* ((root (stm32--root))
         (steps (stm32--build-steps root (stm32--kind root))))
    (if steps (stm32--run steps))))

;; ─── flash ──────────────────────────────────────────────────────────────────

(define (stm32--flasher)
  (or (stm32--get "stm32-flasher")
      (cond ((find-program "openocd") "openocd")
            ((find-program "st-flash") "st-flash")
            ((find-program "STM32_Programmer_CLI") "cubeprog")
            (else #f))))

(define (stm32--openocd-config mcu)
  (let ((target (stm32--openocd-target mcu)))
    (if target
        (list "-f" (stm32--get "stm32-openocd-interface") "-f" target)
        #f)))

(define (stm32--tool prefix-less) (string-append (stm32--get "stm32-toolchain-prefix") prefix-less))

;; The flash steps for the .elf the build will have made (looked up then,
;; not now: there may be none yet). #f and a message when it can't.
(define (stm32--flash-steps root kind mcu)
  (let ((flasher (stm32--flasher)))
    (cond ((not flasher)
           (editor-message "Nothing to flash with: install openocd, stlink (st-flash) or STM32CubeProgrammer, or set stm32-flasher")
           #f)
          ((and (string=? flasher "openocd") (not (stm32--openocd-config mcu)))
           (editor-message "Which STM32 is it? Set stm32-openocd-target, e.g. \"target/stm32f4x.cfg\"")
           #f)
          (else
           (list (list 'call (lambda () (stm32--flash-with flasher root kind mcu))))))))

;; Queues the commands that flash the .elf found now, in front of what's
;; left of the run.
(define (stm32--flash-with flasher root kind mcu)
  (let ((elf (stm32--elf root kind)))
    (if (not elf)
        (stm32--fail (string-append "No .elf in " (stm32--build-dir root kind) ": set stm32-elf"))
        (set! stm32--steps (append (stm32--flash-commands flasher elf mcu) stm32--steps)))))

(define (stm32--flash-commands flasher elf mcu)
  (cond ((string=? flasher "openocd")
         (list (list 'run "Flash"
                     (append (list "openocd") (stm32--openocd-config mcu)
                             (list "-c" (string-append "program {" elf "} verify reset exit"))))))
        ((string=? flasher "st-flash")
         (let ((bin (string-append (substring elf 0 (- (string-length elf) 4)) ".bin")))
           (list (list 'run "Convert" (list (stm32--tool "objcopy") "-O" "binary" elf bin))
                 (list 'run "Flash" (list "st-flash" "--reset" "write" bin "0x08000000")))))
        ((string=? flasher "cubeprog")
         (list (list 'run "Flash" (list "STM32_Programmer_CLI" "-c" "port=SWD" "-w" elf "-v" "-rst"))))
        (else (list (list 'run "Flash" (stm32--replace flasher "{elf}" elf))))))

(define (stm32-flash)
  (let* ((root (stm32--root))
         (kind (stm32--kind root))
         (mcu (stm32--mcu root))
         (build (stm32--build-steps root kind))
         (flash (if build (stm32--flash-steps root kind mcu) #f)))
    (if flash (stm32--run (append build flash)))))

;; ─── debug ──────────────────────────────────────────────────────────────────

(define (stm32--gdb)
  (or (stm32--get "stm32-gdb")
      (find-program (stm32--tool "gdb"))
      (find-program "gdb-multiarch")))

(define (stm32--gdb-server)
  (or (stm32--get "stm32-gdb-server")
      (cond ((find-program "openocd") "openocd")
            ((find-program "st-util") "st-util")
            (else "remote"))))

;; One adapter per gdb: (dap-adapter ...) keeps the first registration of
;; a name, so a project with another gdb gets a name of its own.
(define stm32--adapters '())

(define (stm32--adapter gdb)
  (let ((hit (assoc gdb stm32--adapters)))
    (if hit
        (cdr hit)
        (let ((name (string-append "stm32-" (number->string (+ 1 (length stm32--adapters))))))
          (dap-adapter name gdb "-q" "-i" "dap")
          (set! stm32--adapters (cons (cons gdb name) stm32--adapters))
          name))))

;; gdb's `target remote | openocd ...`: openocd talks to gdb on its stdin
;; and stdout, and ends with the session. `reset?` resets and halts the
;; board as gdb connects.
(define (stm32--openocd-pipe mcu log reset?)
  (string-append "| openocd -c \"gdb_port pipe\""
                 (if (and log (not (stm32--contains? log " ")))
                     (string-append " -c \"log_output " log "\"")
                     "")
                 " -f " (stm32--get "stm32-openocd-interface")
                 " -f " (stm32--openocd-target mcu)
                 (if reset?
                     " -c \"[target current] configure -event gdb-attach {reset halt}\""
                     "")))

;; The steps that start the gdb server (if it's ours to start) and attach.
(define (stm32--attach-steps root kind mcu reset?)
  (let ((gdb (stm32--gdb))
        (server (stm32--gdb-server)))
    (cond ((not gdb)
           (editor-message (string-append "No " (stm32--tool "gdb") " (15 or later) or gdb-multiarch: install the Arm GNU Toolchain, or set stm32-gdb"))
           #f)
          ((and (string=? server "openocd") (not (stm32--openocd-target mcu)))
           (editor-message "Which STM32 is it? Set stm32-openocd-target, e.g. \"target/stm32f4x.cfg\"")
           #f)
          ((string=? server "openocd")
           (list (list 'call (lambda ()
                               (stm32--attach gdb root kind
                                              (stm32--openocd-pipe
                                               mcu (stm32--join (stm32--build-dir root kind) "openocd.log")
                                               reset?))))))
          ((string=? server "st-util")
           (list (list 'server "*gdbserver*" (list "st-util" "-p" "4242"))
                 (list 'delay 2)
                 (list 'call (lambda () (stm32--attach gdb root kind "localhost:4242")))))
          (else
           (list (list 'call (lambda () (stm32--attach gdb root kind (stm32--get "stm32-gdb-remote")))))))))

(define (stm32--attach gdb root kind target)
  (let ((elf (stm32--elf root kind)))
    (if (not elf)
        (stm32--fail (string-append "No .elf in " (stm32--build-dir root kind) ": build first, or set stm32-elf"))
        (begin
          (editor-message (string-append "Debugging " (stm32--relative elf root) " through " target))
          (debug-attach (list (cons "adapter" (stm32--adapter gdb))
                              (cons "program" elf)
                              (cons "target" target)))))))

(define (stm32--session-free?)
  (if (string=? (debug-state) "off")
      #t
      (begin (editor-message "A debug session is running: CTRL-x B q stops it") #f)))

(define (stm32-debug)
  (if (stm32--session-free?) (stm32--debug)))

(define (stm32--debug)
  (let* ((root (stm32--root))
         (kind (stm32--kind root))
         (mcu (stm32--mcu root))
         (attach (stm32--attach-steps root kind mcu #t))
         (build (if attach (stm32--build-steps root kind) #f))
         (flash (if (and build (stm32--get "stm32-flash-before-debug"))
                    (stm32--flash-steps root kind mcu)
                    '())))
    (if (and attach build flash) (stm32--run (append build flash attach)))))

;; Attaches to the program as it runs: no build, no flash, no reset.
(define (stm32-attach)
  (if (stm32--session-free?) (stm32--attach-running)))

(define (stm32--attach-running)
  (let* ((root (stm32--root))
         (kind (stm32--kind root))
         (attach (stm32--attach-steps root kind (stm32--mcu root) #f)))
    (if attach (stm32--run attach))))

(define (stm32-reset)
  (let* ((root (stm32--root))
         (mcu (stm32--mcu root))
         (flasher (stm32--flasher)))
    (cond ((not (string=? (debug-state) "off"))
           (debug-command "monitor reset halt"))
          ((and (equal? flasher "openocd") (stm32--openocd-config mcu))
           (stm32--run (list (list 'run "Reset"
                                   (append (list "openocd") (stm32--openocd-config mcu)
                                           (list "-c" "init" "-c" "reset run" "-c" "exit"))))))
          ((equal? flasher "st-flash") (stm32--run (list (list 'run "Reset" (list "st-flash" "reset")))))
          ((equal? flasher "cubeprog")
           (stm32--run (list (list 'run "Reset" (list "STM32_Programmer_CLI" "-c" "port=SWD" "-rst")))))
          (else (editor-message "Can't reset: no openocd, st-flash or STM32_Programmer_CLI")))))

;; ─── serial monitor ─────────────────────────────────────────────────────────

(define (stm32--serial-ports)
  (let ((system (system-type)))
    (cond ((string=? system "linux")
           (stm32--dev-matching (list "ttyACM" "ttyUSB")))
          ((string=? system "darwin")
           (stm32--dev-matching (list "cu.usbmodem" "cu.usbserial" "cu.SLAB" "cu.wchusbserial")))
          (else '()))))

(define (stm32--dev-matching prefixes)
  (sort (map (lambda (entry) (string-append "/dev/" (car entry)))
             (filter (lambda (entry)
                       (some? (lambda (p) (stm32--starts-with? (car entry) p)) prefixes))
                     (list-directory "/dev")))
        string<?))

(define (stm32--python-with-pyserial)
  (let ((python (or (find-program "python3") (find-program "python"))))
    (if (and python (= 0 (car (process-run python "-c" "import serial.tools.miniterm"))))
        python
        #f)))

(define (stm32--serial-command port)
  (let* ((baud (stm32--number->string (stm32--get "stm32-serial-baud")))
         (tool (or (stm32--get "stm32-serial-tool")
                   (cond ((find-program "tio") "tio")
                         ((find-program "picocom") "picocom")
                         ((stm32--python-with-pyserial) "miniterm")
                         ((find-program "screen") "screen")
                         (else #f)))))
    (cond ((not tool) #f)
          ((string=? tool "tio") (cons "tio -- CTRL-t q quits" (list "tio" "-b" baud port)))
          ((string=? tool "picocom") (cons "picocom -- CTRL-a CTRL-x quits" (list "picocom" "-b" baud port)))
          ((string=? tool "miniterm")
           (let ((python (stm32--python-with-pyserial)))
             (if python
                 (cons "miniterm -- CTRL-] quits" (list python "-m" "serial.tools.miniterm" port baud))
                 #f)))
          ((string=? tool "screen") (cons "screen -- CTRL-a k quits" (list "screen" port baud)))
          (else (cons tool (list tool port baud))))))

(define (stm32--open-serial port)
  (let ((command (stm32--serial-command port)))
    (if (not command)
        (editor-message "No serial monitor: install tio, picocom or pyserial (pip install pyserial)")
        (if (terminal-run "*serial*" (cdr command) (stm32--root) #t)
            (editor-message (string-append port " at " (stm32--number->string (stm32--get "stm32-serial-baud"))
                                           " baud in " (car command) "; ESC for the editor's keys"))
            (editor-message (string-append "Couldn't start " (stm32--2nd command)))))))

(define (stm32-serial)
  (let ((port (stm32--get "stm32-serial-port"))
        (ports (stm32--serial-ports)))
    (cond (port (stm32--open-serial port))
          ((and (pair? ports) (null? (cdr ports))) (stm32--open-serial (car ports)))
          ((pair? ports)
           (editor-list-popup "Serial port" (map (lambda (p) (cons p p)) ports) stm32--open-serial))
          (else
           (let ((typed (editor-prompt "Serial port (e.g. COM3 or /dev/ttyACM0): %u" "" #f)))
             (if (and typed (not (string=? typed ""))) (stm32--open-serial typed)))))))

;; ─── clangd ─────────────────────────────────────────────────────────────────

(define stm32--clangd-marker "# Written by narval's stm32 package (CTRL-x E c)")

;; The cross compiler's own include directories: what clangd's
;; --query-driver would ask it for.
(define (stm32--gcc-includes)
  (let* ((gcc (find-program (stm32--tool "gcc")))
         (result (if gcc (process-run gcc "-xc" "-E" "-v" "-") #f))
         (after (if result (string-split (cdr result) "#include <...> search starts here:" 2) '())))
    (if (and (pair? after) (pair? (cdr after)))
        (filter (lambda (line) (not (string=? line "")))
                (map stm32--trim
                     (string-split (car (string-split (stm32--2nd after) "End of search list." 1)) "\n")))
        #f)))

(define (stm32--yaml-quote s) (string-append "'" (stm32--replace s "'" "''") "'"))

(define (stm32--clangd-text db-dir includes)
  (string-append
   stm32--clangd-marker "\n"
   "# Cross-compiling for arm-none-eabi. CTRL-x E c writes it again; delete\n"
   "# the line above to keep your own changes.\n"
   "CompileFlags:\n"
   "  CompilationDatabase: " (stm32--yaml-quote db-dir) "\n"
   "  Add:\n"
   "    - --target=arm-none-eabi\n"
   (apply string-append
          (map (lambda (dir) (string-append "    - " (stm32--yaml-quote (string-append "-isystem" dir)) "\n"))
               includes))
   "  Remove:\n"
   "    - -fcyclomatic-complexity\n"
   "    - -fstack-usage\n"
   "    - -fcallgraph-info*\n"
   "    - -fno-tree-loop-distribute-patterns\n"
   "    - -mthumb-interwork\n"
   "    - -specs=*\n"
   "    - --specs=*\n"
   "Diagnostics:\n"
   "  Suppress: [unknown-warning-option, unused-command-line-argument]\n"))

(define (stm32--write-clangd root db-dir)
  (let* ((path (stm32--join root ".clangd"))
         (old (read-file path))
         (includes (stm32--gcc-includes)))
    (cond ((not (stm32--exists? (stm32--join db-dir "compile_commands.json")))
           (editor-message (string-append "No compile_commands.json in " db-dir)))
          ((not includes)
           (editor-message (string-append "Couldn't ask " (stm32--tool "gcc") " for its include directories: is it on PATH?")))
          ((and old (not (stm32--starts-with? old stm32--clangd-marker)))
           (editor-message ".clangd is yours (no stm32 marker line): left as it is"))
          ((write-file path (stm32--clangd-text (stm32--relative db-dir root) includes))
           (editor-message ".clangd written: open a file again (or restart narval) for clangd to use it"))
          (else (editor-message (string-append "Couldn't write " path))))))

(define (stm32-clangd-setup)
  (let* ((root (stm32--root))
         (kind (stm32--kind root))
         (cmake? (or (equal? kind "cmake") (equal? kind "cmake-presets")))
         (db-dir (if cmake? (stm32--build-dir root kind) root))
         (write (list 'call (lambda () (stm32--write-clangd root db-dir)))))
    (cond ((stm32--exists? (stm32--join db-dir "compile_commands.json"))
           (stm32--write-clangd root db-dir))
          (cmake?
           ;; Configuring writes it (CMAKE_EXPORT_COMPILE_COMMANDS).
           (let ((build (stm32--build-steps root kind)))
             (if build (stm32--run (append build (list write))))))
          ((not (find-program "bear"))
           (editor-message "A Makefile project needs compile_commands.json: install bear, then CTRL-x E c again"))
          ((equal? kind "cubeide")
           (stm32--run (list (list 'run "Compile database"
                                   (list "bear" "--" "make" "-B" (string-append "-j" (stm32--jobs))
                                         "-C" (stm32--get "stm32-build-type") "all"))
                             write)))
          (else
           (stm32--run (list (list 'run "Compile database"
                                   (list "bear" "--" "make" "-B" (string-append "-j" (stm32--jobs))))
                             write))))))

;; ─── what was detected ──────────────────────────────────────────────────────

(define (stm32--or-none v) (if v v "(none)"))

(define (stm32-info)
  (let* ((root (stm32--root))
         (kind (stm32--kind root))
         (mcu (stm32--mcu root))
         (elf (stm32--elf root kind))
         (id (editor-scratch-open "*stm32-info*")))
    (editor-scratch-set-lines
     id
     (list (string-append "Project       " root)
           (string-append "Build system  " (stm32--or-none kind))
           (string-append "MCU           " (if mcu (string-append (car mcu) " (STM32" (cdr mcu) ")") "(unknown: set stm32-openocd-target)"))
           (string-append "Build dir     " (stm32--build-dir root kind))
           (string-append ".elf          " (if elf (stm32--relative elf root) "(none yet)"))
           (string-append "Flasher       " (stm32--or-none (stm32--flasher)))
           (string-append "GDB server    " (stm32--gdb-server))
           (string-append "gdb           " (stm32--or-none (stm32--gdb)))
           (string-append "openocd       " (stm32--or-none (stm32--openocd-target mcu))
                          " with " (stm32--get "stm32-openocd-interface"))
           (string-append "Serial ports  " (stm32--join-with (stm32--serial-ports) " "))
           ""
           "CTRL-x E b build, f flash, d debug, a attach, s serial, c clangd, r reset, p settings"))
    (editor-switch-to-buffer id)))

;; ─── per-project settings ───────────────────────────────────────────────────

(define stm32--project-block
  (string-append
   "\n;; stm32 (CTRL-x E): uncomment what this board needs.\n"
   ";; (define stm32-openocd-target \"target/stm32f4x.cfg\")\n"
   ";; (define stm32-openocd-interface \"interface/stlink.cfg\")\n"
   ";; (define stm32-flasher \"openocd\")        ; \"st-flash\" \"cubeprog\", or a command with {elf}\n"
   ";; (define stm32-gdb-server \"openocd\")     ; \"st-util\" \"remote\"\n"
   ";; (define stm32-gdb-remote \"localhost:2331\")\n"
   ";; (define stm32-build-type \"Debug\")\n"
   ";; (define stm32-build-command \"make -j8\")\n"
   ";; (define stm32-elf \"build/Debug/firmware.elf\")\n"
   ";; (define stm32-serial-port \"/dev/ttyACM0\")\n"
   ";; (define stm32-serial-baud 115200)\n"))

(define (stm32-project-config)
  (let* ((root (stm32--root))
         (dir (stm32--join root ".narval"))
         (path (stm32--join dir "init.lisp"))
         (old (or (read-file path) "")))
    (if (not (stm32--contains? old "stm32-"))
        (begin
          (make-directories dir)
          (write-file path (string-append old stm32--project-block))))
    (project-open-config)
    (editor-message "Uncomment what this board needs, save, then CTRL-x p t loads it")))

;; ─── keys ───────────────────────────────────────────────────────────────────

(add-prefix "stm32")
(map-key "E" '(editor-set-prefix "stm32") "user")
(map-key "b" stm32-build "stm32")
(map-key "f" stm32-flash "stm32")
(map-key "d" stm32-debug "stm32")
(map-key "a" stm32-attach "stm32")
(map-key "s" stm32-serial "stm32")
(map-key "c" stm32-clangd-setup "stm32")
(map-key "r" stm32-reset "stm32")
(map-key "i" stm32-info "stm32")
(map-key "p" stm32-project-config "stm32")
