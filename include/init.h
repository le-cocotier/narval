#ifndef INIT_H_
#define INIT_H_


/**
 * \fn void initEditor()
 * \brief Job's function is to initialize all the fields of editorConfig.
 * */

void initBuiltins();

void initEditor();

void deInitEditor();

/**
 * Reloads the config without restarting: everything init.lisp and the
 * packages set up is forgotten first (Lisp definitions, key bindings,
 * prefixes, hooks, language servers, syntax rules, the theme, the status
 * bar layout, the loaded packages), so a line taken out of init.lisp
 * takes its effect away too, then init.lisp is loaded as at startup and
 * the settings are read again. Open buffers, panes, running language
 * servers and terminals stay. Errors are reported as at startup
 * (*config-errors*), with the emergency key bindings if needed. Fires the
 * "config-reloaded" hook. Must not run inside a Lisp call: use
 * editorRequestConfigReload() there. Returns the number of errors, -1 when
 * the config never loaded.
 */
int editorReloadConfig(void);

// (config-reload): asks for editorReloadConfig() at the run loop's next
// tick, where editorRunPendingConfigReload() does it.
void editorRequestConfigReload(void);
void editorRunPendingConfigReload(void);

// Called after each reload, once the settings are read: narval-gui
// applies GUI-FONT and GUI-FONT-SIZE again (main.c).
void editorSetConfigReloadListener(void (*listener)(void));


#endif // INIT_H_
