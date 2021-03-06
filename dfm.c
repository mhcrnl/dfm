/* dfm - Simple GTK+ file manager
 *
 * Copyright (c) 2011 David Stenberg
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 */

#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <dirent.h>
#include <pthread.h>
#include <sys/stat.h>
#include <gtk/gtk.h>
#include <gdk/gdkkeysyms.h>

#include "version.h"

#define ARRSIZE(x) (sizeof(x) / sizeof(*x))
#define CLEANMASK(mask) (mask & ~(GDK_MOD2_MASK))

enum ListColumns {
	NAME_STR,
	PERMS_STR,
	SIZE_STR,
	MTIME_STR,
	IS_DIR
};

enum Movement {
	UP,
	DOWN,
	HOME,
	END,
	PAGEUP,
	PAGEDOWN
};

enum Preferences {
	DOTFILES
};

typedef struct {
	GtkWidget *win;
	GtkWidget *scroll;
	GtkWidget *tree;
	gchar *path;
	gboolean show_dot;
	time_t mtime;
} FmWindow;

typedef struct {
	gboolean b;
	gint i;
	void *v;
} Arg;

typedef struct {
	guint mod;
	guint key;
	void (*func)(FmWindow *fw, const Arg *arg);
	const Arg arg;
} Key;

/* functions */
static void action(GtkWidget *w, GtkTreePath *p, GtkTreeViewColumn *c, FmWindow *fw);
static void bookmark(FmWindow *fw, const Arg *arg);
static gint compare(GtkTreeModel *m, GtkTreeIter *a, GtkTreeIter *b, gpointer p);
static gchar *create_perm_str(mode_t mode);
static gchar *create_size_str(size_t size);
static gchar *create_time_str(const char *fmt, const struct tm *time);
static FmWindow *createwin();
static void destroywin(GtkWidget *w, FmWindow *fw);
static void dir_exec(FmWindow *fw, const Arg *arg);
static gint get_mtime(const gchar *path, time_t *time);
static GList *get_selected(FmWindow *fw);
static gboolean keypress(GtkWidget *w, GdkEventKey *ev, FmWindow *fw);
static void make_dir(FmWindow *fw, const Arg *arg);
static void move_cursor(FmWindow *fw, const Arg *arg);
static void mv(FmWindow *fw, const Arg *arg);
static void newwin(FmWindow *fw, const Arg *arg);
static void open_directory(FmWindow *fw, const char *str);
static void set_path(FmWindow *fw, const Arg *arg);
static gchar *prev_dir(gchar *path);
static void read_files(FmWindow *fw, DIR *dir);
static void reload(FmWindow *fw);
static void spawn(const gchar * const *argv, const gchar *path);
static gchar *text_dialog(GtkWindow *p, const gchar *title, const gchar *text);
static void text_dialog_enter(GtkWidget *w, GtkDialog *dialog);
static void toggle_pref(FmWindow *fw, const Arg *arg);
static void update(FmWindow *fw);
static void *update_thread(void *v);
static int valid_filename(const char *s, int show_dot);

/* variables */
static gboolean show_dotfiles = FALSE;
static GList *windows = NULL;

#include "config.h"

/* enters the selected item if directory, otherwise 
 * executes program with the file as argument */
void
action(GtkWidget *w, GtkTreePath *p, GtkTreeViewColumn *c, FmWindow *fw)
{
	GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(fw->tree));
	GtkTreeIter iter;
	gchar *name;
	gchar fpath[PATH_MAX];
	gboolean is_dir;

	gtk_tree_model_get_iter(model, &iter, p);
	gtk_tree_model_get(model, &iter, NAME_STR, &name,
			IS_DIR, &is_dir, -1);

	chdir(fw->path);
	realpath(name, fpath);
	g_free(name);

	if (is_dir) { /* open directory */
		open_directory(fw, fpath);
	} else { /* execute program */
		spawn(filecmd, fpath);
	}
}

/* open a bookmark */
void
bookmark(FmWindow *fw, const Arg *arg)
{
	if (arg->i >= 0 && arg->i < ARRSIZE(bookmarks))
		open_directory(fw, (char *)bookmarks[arg->i]);
}

/* compares two rows in the tree model */
gint
compare(GtkTreeModel *m, GtkTreeIter *a, GtkTreeIter *b, gpointer p)
{
	gchar *name[2];
	gint isdir[2];
	gint ret;

	gtk_tree_model_get(m, a, NAME_STR, &name[0], IS_DIR, &isdir[0], -1);
	gtk_tree_model_get(m, b, NAME_STR, &name[1], IS_DIR, &isdir[1], -1);

	if (isdir[0] == isdir[1])
		ret = g_ascii_strcasecmp(name[0], name[1]);
	else 
		ret = isdir[0] ? -1 : 1;

	g_free(name[0]);
	g_free(name[1]);
	return ret;
}

/* creates a formatted permission string */
gchar* 
create_perm_str(mode_t mode)
{
	char *permstr[] = { 
		"---", "--x", "-w-", "-wx", 
		"r--", "r-x", "rw-", "rwx" };

	return g_strdup_printf("%s%s%s", permstr[(mode >> 6) & 7],
			permstr[(mode >> 3) & 7],
			permstr[mode & 7]);
}

/* creates a formatted size string */
gchar* 
create_size_str(size_t size)
{
	if (size < 1024)
		return g_strdup_printf("%i B", (int)size);
	else if (size < 1024*1024)
		return g_strdup_printf("%.1f KB", size/1024.0);
	else if (size < 1024*1024*1024)
		return g_strdup_printf("%.1f MB", size/(1024.0*1024));
	else
		return g_strdup_printf("%.1f GB", size/(1024.0*1024*1024));
}

/* creates a formatted time string */
gchar* 
create_time_str(const char *fmt, const struct tm *time)
{
	gchar buf[64];
	strftime(buf, sizeof(buf), fmt, time);
	return g_strdup(buf);
}

/* creates and initializes a FmWindow */
FmWindow*
createwin()
{
	FmWindow *fw;

	GtkCellRenderer *rend;
	GtkListStore *store;
	GtkTreeSortable *sortable;

	fw = g_malloc(sizeof(FmWindow));
	fw->path = NULL;
	fw->show_dot = show_dotfiles;
	fw->win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
	gtk_window_set_default_size(GTK_WINDOW(fw->win), 640, 480);
	gtk_window_set_icon_name(GTK_WINDOW(fw->win), "folder");

	/* setup scrolled window */
	fw->scroll = gtk_scrolled_window_new(NULL, NULL);
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(fw->scroll),
			GTK_POLICY_AUTOMATIC, GTK_POLICY_ALWAYS);

	/* setup list store */
	store = gtk_list_store_new(5, G_TYPE_STRING, G_TYPE_STRING,
			G_TYPE_STRING, G_TYPE_STRING, G_TYPE_BOOLEAN);
	sortable = GTK_TREE_SORTABLE(store);

	/* setup tree view */
	fw->tree = gtk_tree_view_new_with_model(GTK_TREE_MODEL(store));
	gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(fw->tree), TRUE);
	gtk_tree_view_set_rubber_banding(GTK_TREE_VIEW(fw->tree), TRUE);
	gtk_tree_view_set_rules_hint(GTK_TREE_VIEW(fw->tree), TRUE);
	gtk_tree_selection_set_mode(
			gtk_tree_view_get_selection(GTK_TREE_VIEW(fw->tree)),
			GTK_SELECTION_MULTIPLE);

	rend = gtk_cell_renderer_text_new();
	gtk_tree_view_insert_column_with_attributes(GTK_TREE_VIEW(fw->tree),
			-1, "Name", rend, "text", NAME_STR, NULL);
	rend = gtk_cell_renderer_text_new();
	gtk_tree_view_insert_column_with_attributes(GTK_TREE_VIEW(fw->tree),
			-1, "Permissions", rend, "text", PERMS_STR, NULL);
	rend = gtk_cell_renderer_text_new();
	gtk_tree_view_insert_column_with_attributes(GTK_TREE_VIEW(fw->tree),
			-1, "Size", rend, "text", SIZE_STR, NULL);
	rend = gtk_cell_renderer_text_new();
	gtk_tree_view_insert_column_with_attributes(GTK_TREE_VIEW(fw->tree),
			-1, "Modified", rend, "text", MTIME_STR, NULL);

	/* expand first column */
	gtk_tree_view_column_set_expand(
			gtk_tree_view_get_column(GTK_TREE_VIEW(fw->tree), 0), 
			TRUE); 

	/* setup list sorting */
	gtk_tree_sortable_set_sort_func(sortable, NAME_STR, compare, NULL, NULL);
	gtk_tree_sortable_set_sort_column_id(sortable, NAME_STR, GTK_SORT_ASCENDING);

	/* connect signals */
	g_signal_connect(G_OBJECT(fw->win), "destroy", 
			G_CALLBACK(destroywin), fw);
	g_signal_connect(G_OBJECT(fw->win), "key-press-event",
			G_CALLBACK(keypress), fw);
	g_signal_connect(G_OBJECT(fw->tree), "row-activated", 
			G_CALLBACK(action), fw);

	/* add widgets */
	gtk_container_add(GTK_CONTAINER(fw->scroll), fw->tree);
	gtk_container_add(GTK_CONTAINER(fw->win), fw->scroll);

	gtk_widget_show_all(fw->win);

	return fw;
}

/* removes and deallocates a FmWindow */
void
destroywin(GtkWidget *w, FmWindow *fw)
{
	if ((windows = g_list_remove(windows, fw)) == NULL)
		gtk_main_quit();

	gtk_widget_destroy(fw->tree);
	gtk_widget_destroy(fw->scroll);
	gtk_widget_destroy(fw->win);
	
	if (fw->path)
		g_free(fw->path);

	g_free(fw);
}

/* change directory to current, and spawns program in background */
void
dir_exec(FmWindow *fw, const Arg *arg)
{
	g_return_if_fail(fw->path && arg->v);
	spawn(arg->v, fw->path);
}

/* get mtime for a file. returns 0 if ok */
gint
get_mtime(const gchar *path, time_t *time)
{
	struct stat st;
	gint err;

	g_return_val_if_fail(time, 0);

	if ((err = stat(path, &st)) == 0)
		*time = st.st_mtime;

	return err;
}

/* returns a list of name for selected files (relative file names, not absolute) */
GList *
get_selected(FmWindow *fw)
{
	GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(fw->tree));
	GtkTreeModel *model;
	GtkTreeIter iter;
	GList *lsel = gtk_tree_selection_get_selected_rows(sel, &model);
	GList *node = lsel;
	GList *files = NULL;
	gchar *name;

	while (node) {
		gtk_tree_model_get_iter(model, &iter, node->data);
		gtk_tree_model_get(model, &iter, NAME_STR, &name, -1);
		files = g_list_append(files, name);

		node = g_list_next(node);
	}

	g_list_foreach(lsel, (GFunc) gtk_tree_path_free, NULL);
	g_list_free(lsel);

	return files;
}

/* handles key events on the FmWindow */
gboolean
keypress(GtkWidget *w, GdkEventKey *ev, FmWindow *fw)
{
	gint i;

	for (i = 0; i < ARRSIZE(keys); i++) {
		if (gdk_keyval_to_lower(ev->keyval) == keys[i].key &&
				CLEANMASK(ev->state) == keys[i].mod &&
				keys[i].func) {
			keys[i].func(fw, &keys[i].arg);
		}
	}

	return FALSE;
}

void
make_dir(FmWindow *fw, const Arg *arg)
{
	gchar *path;

	g_return_if_fail(fw->path);

	if ((path = text_dialog(GTK_WINDOW(fw->win), "make directory", NULL))) {
		if (mkdir(path, arg->i) == -1)
			g_warning("mkdir: %s", strerror(errno));
		g_free(path);
	}
}

/* moves cursor in the tree view */
void
move_cursor(FmWindow *fw, const Arg *arg)
{
	/* TODO: fix this */

	GtkMovementStep m;
	gint v;
	gboolean ret;

	switch (arg->i) {
	case UP:
		m = GTK_MOVEMENT_DISPLAY_LINES;
		v = -1;
		break;
	case DOWN:
		m = GTK_MOVEMENT_DISPLAY_LINES;
		v = 1;
		break;
	case HOME:
		m = GTK_MOVEMENT_BUFFER_ENDS;
		v = -1;
		break;
	case END:
		m = GTK_MOVEMENT_BUFFER_ENDS;
		v = 1;
		break;
	case PAGEUP:
		m = GTK_MOVEMENT_PAGES;
		v = -1;
		break;
	case PAGEDOWN:
		m = GTK_MOVEMENT_PAGES;
		v = 1;
		break;
	default:
		return;
	}

	g_signal_emit_by_name(G_OBJECT(fw->tree), "move-cursor", m, v, &ret);
}

void
mv(FmWindow *fw, const Arg *arg)
{
	/* TODO: dummy atm */

	GList *files = get_selected(fw);
	GList *iter = files;

	while (iter) {
		printf("f: '%s'\n", (char *)iter->data);
		iter = g_list_next(iter);
	}

	g_list_foreach(files, (GFunc) g_free, NULL);
	g_list_free(files);
}

/* creates and inserts a new FmWindow to the window list */
void
newwin(FmWindow *fw, const Arg *arg)
{
	FmWindow *new = createwin();
	windows = g_list_append(windows, new);

	open_directory(new, arg->v ? arg->v : (fw ? fw->path : NULL));
}

/* open and reads directory data to FmWindow */
void
open_directory(FmWindow *fw, const char *str)
{
	DIR *dir;
	char rpath[PATH_MAX];

	g_return_if_fail(str);

	/* change to current working directory to get relative paths right */
	if (fw->path)
		chdir(fw->path);

	/* get clean absolute path string */
	realpath(str, rpath);

	if (!(dir = opendir(rpath))) {
		g_warning("%s: %s\n", rpath, g_strerror(errno));

		if (strcmp(rpath, "/") != 0) {
			/* try to go up one level and load directory */
			open_directory(fw, prev_dir(rpath));	
		}
		return;
	}

	if (fw->path)
		g_free(fw->path);

	fw->path = g_strdup(rpath);
	chdir(fw->path);
	get_mtime(fw->path, &fw->mtime);

	gtk_window_set_title(GTK_WINDOW(fw->win), fw->path);

	read_files(fw, dir);

	closedir(dir);
}

gchar*
prev_dir(gchar *path)
{
	gchar *p;
	if ((p = g_strrstr(path, "/"))) {
		if (p == path)
			*(p + 1) = '\0';
		else
			*p = '\0';
	}
	return path;
}

/* reads files in to fw's list store from an opened DIR struct */
void
read_files(FmWindow *fw, DIR *dir)
{
	struct dirent *e;
	struct stat st;
	struct tm *time;
	gchar *name_str;
	gchar *mtime_str;
	gchar *perms_str;
	gchar *size_str;

	GtkListStore *store = GTK_LIST_STORE(
			gtk_tree_view_get_model(GTK_TREE_VIEW(fw->tree)));
	GtkTreeIter iter;
	GtkTreeSortable *sortable = GTK_TREE_SORTABLE(store);

	/* remove previous entries */
	gtk_list_store_clear(store);

	/* disable sort to speed up insertion */
	gtk_tree_sortable_set_sort_column_id(sortable,
			GTK_TREE_SORTABLE_UNSORTED_SORT_COLUMN_ID,
			GTK_SORT_ASCENDING);

	while ((e = readdir(dir))) {

		if (valid_filename(e->d_name, fw->show_dot)
				&& (stat(e->d_name, &st) == 0)) {

			if (S_ISDIR(st.st_mode))
				name_str = g_strdup_printf("%s/", e->d_name);
			else
				name_str = g_strdup(e->d_name);

			time = localtime(&st.st_mtime);
			mtime_str = create_time_str(timefmt, time);
			perms_str = create_perm_str(st.st_mode);
			size_str = create_size_str(st.st_size);

			gtk_list_store_append(store, &iter);
			gtk_list_store_set(store, &iter,
					NAME_STR, name_str,
					PERMS_STR, perms_str,
					SIZE_STR, size_str,
					MTIME_STR, mtime_str,
					IS_DIR, S_ISDIR(st.st_mode),
					-1);

			g_free(name_str);
			g_free(mtime_str);
			g_free(perms_str);
			g_free(size_str);
		}
	}

	/* reenable sort */
	gtk_tree_sortable_set_sort_column_id(sortable, NAME_STR, GTK_SORT_ASCENDING);
}

/* reload a FmWindow */
void
reload(FmWindow *fw)
{
	open_directory(fw, fw->path);
}

void
set_path(FmWindow *fw, const Arg *arg)
{
	char *path;

	if ((path = arg->v)) {
		open_directory(fw, path);
	} else if ((path = text_dialog(GTK_WINDOW(fw->win), "path", fw->path))) {
		open_directory(fw, path);
		g_free(path);
	}
}

/* change working directory and spawns a program to the background */
void
spawn(const gchar * const *argv, const gchar *path)
{
	if (fork() == 0) {
		g_setenv("DFM_PATH", path, TRUE);
		chdir(path);
		execvp(*argv, (gchar **)argv);
		g_warning("spawn: %s", strerror(errno));
	}
}

gchar*
text_dialog(GtkWindow *p, const gchar *title, const gchar *text)
{
	GtkWidget *dialog = gtk_dialog_new_with_buttons(title, p, 
			GTK_DIALOG_MODAL, NULL);
	GtkWidget *entry = gtk_entry_new();
	GtkWidget *area = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
	gchar *str = NULL;

	if (text)
		gtk_entry_set_text(GTK_ENTRY(entry), text);

	g_signal_connect(G_OBJECT(entry), "activate",
			G_CALLBACK(text_dialog_enter), dialog);

	gtk_container_add(GTK_CONTAINER(area), entry);
	gtk_widget_show(entry);

	if (gtk_dialog_run(GTK_DIALOG(dialog)) == 1)
		str = g_strdup(gtk_entry_get_text(GTK_ENTRY(entry)));

	/* clean up */
	gtk_widget_destroy(dialog);

	return str;
}

void
text_dialog_enter(GtkWidget *w, GtkDialog *dialog)
{
	gtk_dialog_response(dialog, 1);
}

/* toggle preferences in a FmWindow, and reload if necessary */
void
toggle_pref(FmWindow *fw, const Arg *arg)
{
	switch (arg->i) {
		case DOTFILES:
			fw->show_dot = !fw->show_dot;
			reload(fw);
			break;
	}
}

void
update(FmWindow *fw)
{
	time_t mtime = 0;

	if (fw->path) {
		if (get_mtime(fw->path, &mtime) != 0 || mtime > fw->mtime) {
			/* directory updated or removed, reload */
			reload(fw);
		}
	}
}

void*
update_thread(void *v)
{
	GList *p;

	for (;;) {
		sleep(polltime);

		gdk_threads_enter();

		for (p = windows; p != NULL; p = g_list_next(p))
			update((FmWindow *)p->data);

		gdk_threads_leave();
	}

	return NULL;
}

/* returns 1 if valid filename, i.e. not '.' or '..' (or .* if show_dot = 0) */
int
valid_filename(const char *s, int show_dot)
{
	return show_dot ? 
		(g_strcmp0(s, ".") != 0 && g_strcmp0(s, "..") != 0) :
		*s != '.';
}

int
main(int argc, char *argv[])
{
	Arg arg;
	pthread_t u_tid;
	pid_t pid;
	gboolean silent = FALSE;
	int i;

	/* read arguments */
	for(i = 1; i < argc && argv[i][0] == '-'; i++) {
		switch(argv[i][1]) {
			case 'v':
				g_print("%s\n", VERSION);
				exit(EXIT_SUCCESS);
			case 'd':
				show_dotfiles = TRUE;
				break;
			case 's':
				silent = TRUE;
				break;
			default:
				g_printerr("Usage: %s [-v] [-d] [-s] PATH\n", argv[0]);
				exit(EXIT_FAILURE);
		}
	}

	arg.v = i < argc ? argv[i] : ".";

	if ((pid = fork()) > 0)
		return EXIT_SUCCESS;
	else if (pid < 0)
		printf("fork: %s\n", g_strerror(errno));

	if (silent) {
		close(STDOUT_FILENO);
		close(STDERR_FILENO);
	}

	/* initialize threads */
	g_thread_init(NULL);
	gdk_threads_init();
	gdk_threads_enter();

	gtk_init(&argc, &argv);

	newwin(NULL, &arg);

	/* create update thread */
	pthread_create(&u_tid, NULL, update_thread, NULL);

	gtk_main();
	gdk_threads_leave();

	return EXIT_SUCCESS;
}
