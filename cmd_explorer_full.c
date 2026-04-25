/**
 * Linux Command Explorer - Full Edition
 * 
 * Features:
 *   - Scans all commands in $PATH (supports up to 65535 commands)
 *   - Shows brief description using 'whatis' (manual page database)
 *   - Provides full manual page on demand via 'man' command
 *   - GUI (GTK3) with search, filter, double-click to see details
 *   - CLI mode available with --cli argument
 *   - Persistent cache (~/.cmd_explorer_cache)
 * 
 * Compilation (on Debian/Ubuntu):
 *   sudo apt install libgtk-3-dev man-db build-essential
 *   gcc -o cmd_explorer cmd_explorer_full.c `pkg-config --cflags --libs gtk+-3.0` -lpthread -lm
 * 
 * Usage:
 *   ./cmd_explorer          # GUI mode (double-click in file manager)
 *   ./cmd_explorer --cli    # CLI interactive mode
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <pthread.h>
#include <gtk/gtk.h>
#include <ctype.h>
#include <errno.h>

/* ------------------------- Constants ------------------------- */
#define MAX_COMMANDS        65535
#define MAX_DESC_LEN        1024
#define CACHE_FILE          ".cmd_explorer_cache"
#define BRIEF_DELAY_USEC    1000    /* microseconds between brief fetches to avoid overload */

/* ------------------------- Command Structure ------------------------- */
typedef struct {
    int     id;
    char   *name;
    char   *brief;          /* short description (whatis) */
} Command;

/* Global data */
static Command   *g_commands = NULL;
static int        g_num_commands = 0;
static GtkWidget *g_main_window;
static GtkWidget *g_tree_view;
static GtkListStore *g_list_store;
static GtkTreeModelFilter *g_filter_model;
static GtkWidget *g_search_entry;
static GtkWidget *g_status_bar;
static pthread_t  g_load_thread;
static volatile int g_loading_done = 0;
static GMutex     g_cache_mutex;

/* ------------------------- Helper Functions ------------------------- */

/* Trim whitespace */
char *trim(char *str) {
    char *end;
    while (isspace((unsigned char)*str)) str++;
    if (*str == 0) return str;
    end = str + strlen(str) - 1;
    while (end > str && isspace((unsigned char)*end)) end--;
    end[1] = '\0';
    return str;
}

/* Get cache file path in home directory */
void get_cache_path(char *out, size_t size) {
    const char *home = getenv("HOME");
    if (!home) home = ".";
    snprintf(out, size, "%s/%s", home, CACHE_FILE);
}

/* Load cached command list (name + brief) */
int load_cache(Command **cmds, int *count) {
    char path[512];
    get_cache_path(path, sizeof(path));
    FILE *fp = fopen(path, "r");
    if (!fp) return 0;
    
    int n = 0;
    char line[MAX_DESC_LEN + 256];
    while (fgets(line, sizeof(line), fp) && n < MAX_COMMANDS) {
        line[strcspn(line, "\n")] = 0;
        char *saveptr;
        char *id_str = strtok_r(line, "|", &saveptr);
        char *name   = strtok_r(NULL, "|", &saveptr);
        char *brief  = strtok_r(NULL, "|", &saveptr);
        if (!name || !brief) continue;
        Command *new_cmds = realloc(*cmds, (n+1) * sizeof(Command));
        if (!new_cmds) { fclose(fp); return 0; }
        *cmds = new_cmds;
        (*cmds)[n].id = n+1;
        (*cmds)[n].name = strdup(name);
        (*cmds)[n].brief = strdup(brief);
        n++;
    }
    fclose(fp);
    *count = n;
    return 1;
}

/* Save command list to cache */
void save_cache(Command *cmds, int count) {
    char path[512];
    get_cache_path(path, sizeof(path));
    FILE *fp = fopen(path, "w");
    if (!fp) return;
    for (int i = 0; i < count; i++) {
        fprintf(fp, "%d|%s|%s\n", cmds[i].id, cmds[i].name, cmds[i].brief);
    }
    fclose(fp);
}

/* Get brief description using 'whatis' */
char *get_brief_description(const char *cmd) {
    char buffer[MAX_DESC_LEN];
    char command[512];
    snprintf(command, sizeof(command), "whatis '%s' 2>/dev/null | head -1", cmd);
    FILE *fp = popen(command, "r");
    if (!fp) return strdup("No description available.");
    
    if (fgets(buffer, sizeof(buffer), fp) != NULL) {
        buffer[strcspn(buffer, "\n")] = 0;
        char *dash = strstr(buffer, " - ");
        if (dash && dash[3]) {
            char *desc = dash + 3;
            while (isspace(*desc)) desc++;
            char *result = strdup(desc);
            pclose(fp);
            return result;
        } else {
            pclose(fp);
            return strdup(buffer);
        }
    } else {
        pclose(fp);
        return strdup("No manual entry - type 'man' to learn more.");
    }
}

/* Get full manual page content for a command (first section) */
char *get_full_manual(const char *cmd) {
    char buffer[65536];
    char command[1024];
    snprintf(command, sizeof(command), "man '%s' 2>/dev/null | col -b", cmd);
    FILE *fp = popen(command, "r");
    if (!fp) return strdup("Unable to retrieve manual page.");
    
    size_t total = 0;
    buffer[0] = '\0';
    while (fgets(buffer + total, sizeof(buffer) - total - 1, fp)) {
        total = strlen(buffer);
        if (total >= sizeof(buffer) - 512) break;
    }
    pclose(fp);
    if (total == 0) {
        snprintf(buffer, sizeof(buffer), "No manual page found for '%s'.", cmd);
    }
    return strdup(buffer);
}

/* Scan all directories in PATH and collect unique executable names */
char **get_all_command_names(int *total) {
    char *path_env = getenv("PATH");
    if (!path_env) return NULL;
    
    char *path_copy = strdup(path_env);
    if (!path_copy) return NULL;
    
    /* Split PATH by ':' */
    char **dirs = NULL;
    int dir_count = 0;
    char *token = strtok(path_copy, ":");
    while (token) {
        dirs = realloc(dirs, (dir_count+1) * sizeof(char*));
        dirs[dir_count++] = strdup(token);
        token = strtok(NULL, ":");
    }
    free(path_copy);
    
    /* Collect commands (use a simple hash set to deduplicate) */
    char **cmd_names = malloc(MAX_COMMANDS * sizeof(char*));
    int cmd_count = 0;
    
    for (int i = 0; i < dir_count; i++) {
        DIR *d = opendir(dirs[i]);
        if (!d) continue;
        struct dirent *entry;
        while ((entry = readdir(d)) != NULL) {
            if (entry->d_name[0] == '.') continue;
            char fullpath[1024];
            snprintf(fullpath, sizeof(fullpath), "%s/%s", dirs[i], entry->d_name);
            struct stat st;
            if (stat(fullpath, &st) == 0 && (st.st_mode & S_IXUSR)) {
                /* Check for duplicates */
                int found = 0;
                for (int j = 0; j < cmd_count; j++) {
                    if (strcmp(cmd_names[j], entry->d_name) == 0) {
                        found = 1;
                        break;
                    }
                }
                if (!found && cmd_count < MAX_COMMANDS) {
                    cmd_names[cmd_count++] = strdup(entry->d_name);
                }
            }
        }
        closedir(d);
        free(dirs[i]);
    }
    free(dirs);
    *total = cmd_count;
    return cmd_names;
}

/* Load all commands (names + brief descriptions) */
Command *load_all_commands(int *out_count, void (*progress_cb)(int, int)) {
    int total_names;
    char **names = get_all_command_names(&total_names);
    if (!names) return NULL;
    
    Command *cmds = malloc(total_names * sizeof(Command));
    if (!cmds) {
        for (int i = 0; i < total_names; i++) free(names[i]);
        free(names);
        return NULL;
    }
    
    for (int i = 0; i < total_names; i++) {
        cmds[i].id = i+1;
        cmds[i].name = names[i];
        if (progress_cb) progress_cb(i+1, total_names);
        /* Brief description via whatis (may be slow) */
        cmds[i].brief = get_brief_description(names[i]);
        usleep(BRIEF_DELAY_USEC);  /* be kind to system */
    }
    free(names);
    *out_count = total_names;
    return cmds;
}

/* ------------------------- CLI Mode ------------------------- */
void cli_mode(void) {
    printf("\033[1;36m=== Linux Command Explorer (CLI) ===\033[0m\n");
    printf("Scanning PATH and loading command list...\n");
    
    int total;
    Command *cmds = load_all_commands(&total, NULL);
    if (!cmds || total == 0) {
        printf("Error: No commands found or failed to load.\n");
        return;
    }
    save_cache(cmds, total);
    
    int choice;
    char input[16];
    while (1) {
        printf("\n\033[1;33mTotal commands available: %d\033[0m\n", total);
        printf("Enter command number (1-%d) to view full manual, or 0 to exit: ", total);
        fgets(input, sizeof(input), stdin);
        choice = atoi(input);
        if (choice == 0) {
            printf("Goodbye!\n");
            break;
        }
        if (choice < 1 || choice > total) {
            printf("Invalid number.\n");
            continue;
        }
        Command *cmd = &cmds[choice-1];
        printf("\n\033[1;32mCommand: %s\033[0m\n", cmd->name);
        printf("\033[1;32mBrief:   %s\033[0m\n", cmd->brief);
        printf("\nFetching full manual page...\n");
        char *full = get_full_manual(cmd->name);
        printf("\n\033[1;34m--- Full Manual (first part) ---\033[0m\n%s\n", full);
        free(full);
        printf("\nPress Enter to continue...");
        getchar();
    }
    
    for (int i = 0; i < total; i++) {
        free(cmds[i].name);
        free(cmds[i].brief);
    }
    free(cmds);
}

/* ------------------------- GUI Mode ------------------------- */

/* Dialog to show full manual */
void show_full_manual_dialog(const char *cmd_name) {
    char *full_text = get_full_manual(cmd_name);
    GtkWidget *dialog = gtk_dialog_new_with_buttons(
        "Command Manual",
        GTK_WINDOW(g_main_window),
        GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
        "Close", GTK_RESPONSE_CLOSE,
        NULL
    );
    gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_CLOSE);
    
    GtkWidget *scrolled = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    GtkWidget *text_view = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(text_view), FALSE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(text_view), GTK_WRAP_WORD);
    GtkTextBuffer *buffer = gtk_text_buffer_new(NULL);
    gtk_text_buffer_set_text(buffer, full_text, -1);
    gtk_text_view_set_buffer(GTK_TEXT_VIEW(text_view), buffer);
    gtk_container_add(GTK_CONTAINER(scrolled), text_view);
    
    GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
    gtk_box_pack_start(GTK_BOX(content), scrolled, TRUE, TRUE, 0);
    gtk_widget_set_size_request(dialog, 800, 600);
    
    gtk_widget_show_all(dialog);
    gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);
    free(full_text);
}

/* Tree view row activation (double-click) */
void on_row_activated(GtkTreeView *tree_view, GtkTreePath *path, GtkTreeViewColumn *col, gpointer user_data) {
    GtkTreeIter iter;
    GtkTreeModel *model = gtk_tree_view_get_model(tree_view);
    if (gtk_tree_model_get_iter(model, &iter, path)) {
        gchar *cmd_name;
        gtk_tree_model_get(model, &iter, 1, &cmd_name, -1);
        show_full_manual_dialog(cmd_name);
        g_free(cmd_name);
    }
}

/* Filter function for search */
gboolean filter_func(GtkTreeModel *model, GtkTreeIter *iter, gpointer user_data) {
    const char *search_text = (const char*)user_data;
    if (!search_text || strlen(search_text) == 0) return TRUE;
    gchar *cmd_name;
    gtk_tree_model_get(model, iter, 1, &cmd_name, -1);
    gboolean match = (strcasestr(cmd_name, search_text) != NULL);
    g_free(cmd_name);
    return match;
}

void on_search_changed(GtkEditable *editable, gpointer user_data) {
    const char *text = gtk_entry_get_text(GTK_ENTRY(g_search_entry));
    static gchar *current_search = NULL;
    if (current_search) g_free(current_search);
    current_search = g_strdup(text);
    gtk_tree_model_filter_set_visible_func(g_filter_model, filter_func, current_search, NULL);
}

/* Populate tree model from global commands */
void populate_tree_model(void) {
    gtk_list_store_clear(g_list_store);
    for (int i = 0; i < g_num_commands; i++) {
        GtkTreeIter iter;
        gtk_list_store_append(g_list_store, &iter);
        gtk_list_store_set(g_list_store, &iter,
                           0, g_commands[i].id,
                           1, g_commands[i].name,
                           2, g_commands[i].brief,
                           -1);
    }
}

/* Background thread: load commands from cache or fresh scan */
void *background_load_thread(void *arg) {
    g_mutex_lock(&g_cache_mutex);
    int loaded = load_cache(&g_commands, &g_num_commands);
    g_mutex_unlock(&g_cache_mutex);
    
    if (!loaded) {
        /* Fresh scan */
        Command *cmds = load_all_commands(&g_num_commands, NULL);
        if (cmds) {
            g_mutex_lock(&g_cache_mutex);
            g_commands = cmds;
            g_mutex_unlock(&g_cache_mutex);
            save_cache(g_commands, g_num_commands);
        } else {
            g_num_commands = 0;
        }
    }
    
    /* Update UI in main thread */
    gdk_threads_enter();
    if (g_num_commands > 0) {
        populate_tree_model();
        gtk_widget_set_sensitive(g_tree_view, TRUE);
        gtk_widget_set_sensitive(g_search_entry, TRUE);
        char status[256];
        snprintf(status, sizeof(status), "Ready. %d commands loaded. Double-click any command to see full manual.",
                 g_num_commands);
        gtk_statusbar_push(GTK_STATUSBAR(g_status_bar),
                           gtk_statusbar_get_context_id(GTK_STATUSBAR(g_status_bar), "ready"),
                           status);
    } else {
        gtk_statusbar_push(GTK_STATUSBAR(g_status_bar),
                           gtk_statusbar_get_context_id(GTK_STATUSBAR(g_status_bar), "error"),
                           "Failed to load commands. Check PATH and whatis.");
    }
    g_loading_done = 1;
    gdk_threads_leave();
    return NULL;
}

void on_quit(GtkWidget *widget, gpointer data) {
    gtk_main_quit();
}

void gui_mode(int argc, char *argv[]) {
    gtk_init(&argc, &argv);
    
    /* Main window */
    g_main_window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(g_main_window), "Linux Command Explorer - Full Edition");
    gtk_window_set_default_size(GTK_WINDOW(g_main_window), 1000, 700);
    g_signal_connect(g_main_window, "destroy", G_CALLBACK(on_quit), NULL);
    
    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
    gtk_container_add(GTK_CONTAINER(g_main_window), vbox);
    
    /* Search bar */
    GtkWidget *search_hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
    GtkWidget *search_label = gtk_label_new("Search command:");
    g_search_entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(g_search_entry), "Type to filter commands...");
    g_signal_connect(g_search_entry, "changed", G_CALLBACK(on_search_changed), NULL);
    gtk_box_pack_start(GTK_BOX(search_hbox), search_label, FALSE, FALSE, 5);
    gtk_box_pack_start(GTK_BOX(search_hbox), g_search_entry, TRUE, TRUE, 5);
    gtk_box_pack_start(GTK_BOX(vbox), search_hbox, FALSE, FALSE, 5);
    
    /* Scrolled window for tree */
    GtkWidget *scrolled = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    
    /* List store: ID (int), Name (string), Brief (string) */
    g_list_store = gtk_list_store_new(3, G_TYPE_INT, G_TYPE_STRING, G_TYPE_STRING);
    g_filter_model = GTK_TREE_MODEL_FILTER(gtk_tree_model_filter_new(GTK_TREE_MODEL(g_list_store), NULL));
    gtk_tree_model_filter_set_visible_func(g_filter_model, filter_func, NULL, NULL);
    
    /* Tree view */
    g_tree_view = gtk_tree_view_new_with_model(GTK_TREE_MODEL(g_filter_model));
    g_object_unref(g_filter_model);
    gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(g_tree_view), TRUE);
    
    /* Columns */
    GtkCellRenderer *renderer = gtk_cell_renderer_text_new();
    GtkTreeViewColumn *col_id = gtk_tree_view_column_new_with_attributes("ID", renderer, "text", 0, NULL);
    gtk_tree_view_append_column(GTK_TREE_VIEW(g_tree_view), col_id);
    gtk_tree_view_column_set_fixed_width(col_id, 60);
    
    GtkTreeViewColumn *col_name = gtk_tree_view_column_new_with_attributes("Command", renderer, "text", 1, NULL);
    gtk_tree_view_append_column(GTK_TREE_VIEW(g_tree_view), col_name);
    gtk_tree_view_column_set_fixed_width(col_name, 200);
    
    GtkTreeViewColumn *col_brief = gtk_tree_view_column_new_with_attributes("Brief Description", renderer, "text", 2, NULL);
    gtk_tree_view_append_column(GTK_TREE_VIEW(g_tree_view), col_brief);
    gtk_tree_view_column_set_expand(col_brief, TRUE);
    
    g_signal_connect(g_tree_view, "row-activated", G_CALLBACK(on_row_activated), NULL);
    
    gtk_container_add(GTK_CONTAINER(scrolled), g_tree_view);
    gtk_box_pack_start(GTK_BOX(vbox), scrolled, TRUE, TRUE, 5);
    
    /* Status bar */
    g_status_bar = gtk_statusbar_new();
    gtk_statusbar_push(GTK_STATUSBAR(g_status_bar),
                       gtk_statusbar_get_context_id(GTK_STATUSBAR(g_status_bar), "init"),
                       "Initializing... Loading command list...");
    gtk_box_pack_start(GTK_BOX(vbox), g_status_bar, FALSE, FALSE, 0);
    
    gtk_widget_show_all(g_main_window);
    
    /* Disable tree and search until loaded */
    gtk_widget_set_sensitive(g_tree_view, FALSE);
    gtk_widget_set_sensitive(g_search_entry, FALSE);
    
    /* Start background loading thread */
    pthread_create(&g_load_thread, NULL, background_load_thread, NULL);
    
    gtk_main();
    
    /* Cleanup */
    if (!g_loading_done) pthread_cancel(g_load_thread);
    pthread_join(g_load_thread, NULL);
    if (g_commands) {
        for (int i = 0; i < g_num_commands; i++) {
            free(g_commands[i].name);
            free(g_commands[i].brief);
        }
        free(g_commands);
    }
}

/* ------------------------- Main Entry Point ------------------------- */
int main(int argc, char *argv[]) {
    /* Default to GUI mode (double-click friendly), --cli for text mode */
    if (argc >= 2 && strcmp(argv[1], "--cli") == 0) {
        cli_mode();
    } else {
        gui_mode(argc, argv);
    }
    return 0;
}
