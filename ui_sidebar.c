#include <string.h>
#include <gtk/gtk.h>
#include "ui_sidebar.h"
#include "theme.h"
#include "protocol.h"

// ─────────────────────────────────────────────────────────────────────────────
// ui_sidebar.c
// Builds and manages the right-side user list panel.
// Knows nothing about sockets — it only displays names and handles
// the double-click → private message shortcut.
// ─────────────────────────────────────────────────────────────────────────────

// Module-private globals
static GtkListStore *user_store; // data model: one row = one username string
static GtkWidget *entry_ref;     // pointer to the input field (set in _create)

// ── render_user_cell ──────────────────────────────────────────────────────────
// GTK calls this for every row in the tree view before drawing it.
// We use it to set a colored dot (⬤) before each username,
// using the same color the user gets in the chat bubbles.
static void render_user_cell(GtkTreeViewColumn *col, GtkCellRenderer *renderer,
                             GtkTreeModel *model, GtkTreeIter *iter,
                             gpointer data)
{
    gchar *name;
    gtk_tree_model_get(model, iter, 0, &name, -1); // read username from row

    const BubbleTheme *t = get_theme(name); // get matching color theme

    char markup[160];
    snprintf(markup, sizeof(markup),
             "<span foreground='%s' size='large'>⬤</span>  %s",
             t->name_color, name);
    // ⬤ = filled circle unicode, colored with the user's theme color
    // Two spaces between dot and name for visual breathing room

    g_object_set(renderer, "markup", markup, NULL);
    // "markup" property tells the cell renderer to interpret HTML-like tags
    g_free(name); // GTK allocated this string; we must free it
}

// ── on_user_activated ─────────────────────────────────────────────────────────
// Called when the user double-clicks a name in the user list.
// Writes "@username: " into the message input box and focuses it,
// so the user can immediately type the private message.
static void on_user_activated(GtkTreeView *tv, GtkTreePath *path,
                              GtkTreeViewColumn *col, gpointer data)
{
    GtkTreeModel *model = gtk_tree_view_get_model(tv);
    GtkTreeIter iter;
    if (gtk_tree_model_get_iter(model, &iter, path))
    {
        gchar *username;
        gtk_tree_model_get(model, &iter, 0, &username, -1);

        char cmd[64];
        snprintf(cmd, sizeof(cmd), "@%s: ", username);
        gtk_entry_set_text(GTK_ENTRY(entry_ref), cmd);
        // entry_ref was saved in ui_sidebar_create() — no global needed

        gtk_widget_grab_focus(entry_ref);
        // moves keyboard focus to the input box so user can type immediately

        g_free(username);
    }
}

// ── ui_sidebar_create ─────────────────────────────────────────────────────────
// Builds the full sidebar widget tree.
// entry_msg is stored in entry_ref so the click handler can access it.
GtkWidget *ui_sidebar_create(GtkWidget *entry_msg)
{
    entry_ref = entry_msg; // save for use in on_user_activated

    // Outer scrolled window — this is what main.c packs into the layout
    GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_style_context_add_class(gtk_widget_get_style_context(scroll), "sidebar");
    // "sidebar" class → .sidebar in APP_CSS → darker background color
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_size_request(scroll, 200, -1);
    // 200px wide, height = whatever the window gives it

    // Vertical box inside the scroll: header label + tree view
    GtkWidget *col_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

    // "ACTIVE USERS" header label
    GtkWidget *hdr = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(hdr),
                         "<span size='small' weight='bold' foreground='#777770'>ACTIVE USERS</span>");
    gtk_widget_set_margin_top(hdr, 14);
    gtk_widget_set_margin_bottom(hdr, 8);
    gtk_widget_set_margin_start(hdr, 14);
    gtk_label_set_xalign(GTK_LABEL(hdr), 0.0); // left-align the text

    // Data model: one column of strings (usernames)
    user_store = gtk_list_store_new(1, G_TYPE_STRING);

    // Tree view: displays the model
    GtkWidget *tree = gtk_tree_view_new_with_model(GTK_TREE_MODEL(user_store));
    gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(tree), FALSE);
    // FALSE → hide the "Users" column header, not needed visually
    gtk_tree_view_set_activate_on_single_click(GTK_TREE_VIEW(tree), FALSE);
    // FALSE → only double-click triggers row-activated signal
    gtk_style_context_add_class(gtk_widget_get_style_context(tree), "user-list");

    // Cell renderer: draws one row's text
    GtkCellRenderer *cell = gtk_cell_renderer_text_new();
    g_object_set(cell, "xpad", 12, "ypad", 6, NULL);
    // xpad/ypad = horizontal/vertical padding inside each row

    // Column: connects cell renderer to the model, uses custom draw function
    GtkTreeViewColumn *tvc = gtk_tree_view_column_new_with_attributes(
        "", cell, NULL); // "" = no header title
    gtk_tree_view_column_set_cell_data_func(tvc, cell, render_user_cell, NULL, NULL);
    // render_user_cell is called instead of the default renderer
    // this is how we inject the colored ⬤ dot before each name
    gtk_tree_view_append_column(GTK_TREE_VIEW(tree), tvc);

    // Connect double-click signal
    g_signal_connect(tree, "row-activated", G_CALLBACK(on_user_activated), NULL);

    gtk_box_pack_start(GTK_BOX(col_box), hdr, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(col_box), tree, TRUE, TRUE, 0);

    // add_with_viewport: needed because GtkScrolledWindow requires a
    // scrollable child; GtkBox is not scrollable, viewport wraps it
    gtk_scrolled_window_add_with_viewport(GTK_SCROLLED_WINDOW(scroll), col_box);

    return scroll;
}

// ── ui_sidebar_update_users ───────────────────────────────────────────────────
// Replaces the entire user list with new data.
// comma_list example: "mark,mina,fo2sh"
// Called from network.c when a TYPE_LIST packet arrives.
void ui_sidebar_update_users(const char *comma_list)
{
    gtk_list_store_clear(user_store); // remove all existing rows

    // strtok splits the string at each comma
    char buf[MAX_NAME * 10];
    strncpy(buf, comma_list, sizeof(buf) - 1);
    char *name = strtok(buf, ",");
    while (name)
    {
        if (strlen(name) > 0)
        {
            GtkTreeIter iter;
            gtk_list_store_append(user_store, &iter); // add empty row
            gtk_list_store_set(user_store, &iter, 0, name, -1);
            // set column 0 of that row to the username string
        }
        name = strtok(NULL, ","); // next token
    }
}
