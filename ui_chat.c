#include <string.h>
#include <gtk/gtk.h>
#include "ui_chat.h"
#include "theme.h"

// ─────────────────────────────────────────────────────────────────────────────
// ui_chat.c
// Responsible for one thing: displaying messages as bubbles.
// It knows nothing about sockets, files, or the sidebar.
// ─────────────────────────────────────────────────────────────────────────────

// Module-private globals — only accessible inside this file
static GtkWidget *chat_list;   // GtkListBox — one row per message
static GtkWidget *chat_scroll; // GtkScrolledWindow wrapping chat_list

// ── ui_chat_create ────────────────────────────────────────────────────────────
// Called once from main() to build the chat panel.
// Returns a GtkBox containing the scrolled list.
// Initialises chat_list and chat_scroll so append_bubble() works.
GtkWidget *ui_chat_create(void)
{
    // GtkListBox: vertical list of rows, no built-in selection color
    chat_list = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(chat_list), GTK_SELECTION_NONE);
    // GTK_SELECTION_NONE: clicking a row does nothing visual
    gtk_style_context_add_class(
        gtk_widget_get_style_context(chat_list), "chat-list");
    // "chat-list" matches .chat-list in APP_CSS → sets background color

    // GtkScrolledWindow: lets chat_list scroll vertically
    chat_scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_style_context_add_class(
        gtk_widget_get_style_context(chat_scroll), "chat-scroll");
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(chat_scroll),
                                   GTK_POLICY_NEVER,     // no horizontal scroll
                                   GTK_POLICY_AUTOMATIC);// vertical scroll as needed
    gtk_container_add(GTK_CONTAINER(chat_scroll), chat_list);

    // Wrap in a vertical box so the caller gets one widget to pack
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_pack_start(GTK_BOX(box), chat_scroll, TRUE, TRUE, 0);
    // TRUE, TRUE → chat_scroll expands to fill all available vertical space

    return box;
}

// ── scroll_to_bottom ─────────────────────────────────────────────────────────
// Moves the scroll position to the very bottom of chat_scroll.
// Called after every new bubble is inserted.
static void scroll_to_bottom(void)
{
    GtkAdjustment *adj = gtk_scrolled_window_get_vadjustment(
        GTK_SCROLLED_WINDOW(chat_scroll));
    // gtk_adjustment_get_upper() returns the maximum scrollable position
    gtk_adjustment_set_value(adj, gtk_adjustment_get_upper(adj));
}

// ── make_avatar ───────────────────────────────────────────────────────────────
// Builds a 34×34 colored circle with 2-character initials inside.
// Returns a GtkEventBox (needed because plain GtkBox can't have a bg color).
static GtkWidget *make_avatar(const char *sender, int is_me,
                               const BubbleTheme *theme)
{
    // Extract first 2 characters and uppercase them
    char initials[3] = { sender ? (char)sender[0] : '?', '\0', '\0' };
    if (sender && sender[1]) initials[1] = (char)sender[1];
    if (initials[0] >= 'a' && initials[0] <= 'z') initials[0] -= 32;
    if (initials[1] >= 'a' && initials[1] <= 'z') initials[1] -= 32;

    // Label showing the initials with Pango markup
    GtkWidget *av_label = gtk_label_new(NULL);
    char av_markup[64];
    snprintf(av_markup, sizeof(av_markup),
             "<span foreground='%s' weight='bold' size='small'>%s</span>",
             is_me ? "#C8F0E0" : theme->name_color,
             is_me ? "ME" : initials);
    gtk_label_set_markup(GTK_LABEL(av_label), av_markup);
    gtk_widget_set_valign(av_label, GTK_ALIGN_CENTER);
    gtk_widget_set_halign(av_label, GTK_ALIGN_CENTER);

    // EventBox: invisible container that supports background color via CSS
    GtkWidget *av_box = gtk_event_box_new();
    gtk_container_add(GTK_CONTAINER(av_box), av_label);
    gtk_widget_set_size_request(av_box, 34, 34);
    // 34×34 + border-radius:17px in CSS = perfect circle
    gtk_widget_set_valign(av_box, GTK_ALIGN_END);
    // GTK_ALIGN_END → avatar sits at the bottom of the message row

    // Per-widget CSS to set the circle's background color
    // We can't use the global APP_CSS for this because each user has a
    // different color — we generate the CSS string dynamically
    char av_css[256];
    snprintf(av_css, sizeof(av_css),
             "* { background-color: %s; border-radius: 17px; }",
             is_me ? "#0F6E56" : theme->bg);
    GtkCssProvider *av_prov = gtk_css_provider_new();
    gtk_css_provider_load_from_data(av_prov, av_css, -1, NULL);
    gtk_style_context_add_provider(
        gtk_widget_get_style_context(av_box),
        GTK_STYLE_PROVIDER(av_prov),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(av_prov); // GTK keeps a reference; we release ours

    return av_box;
}

// ── make_bubble_label ─────────────────────────────────────────────────────────
// Builds the colored rounded rectangle containing the message text.
static GtkWidget *make_bubble_label(const char *text, int is_me,
                                    int is_private, const BubbleTheme *theme)
{
    GtkWidget *label = gtk_label_new(text);
    gtk_label_set_line_wrap(GTK_LABEL(label), TRUE);
    gtk_label_set_line_wrap_mode(GTK_LABEL(label), PANGO_WRAP_WORD_CHAR);
    // PANGO_WRAP_WORD_CHAR: wrap at words, fall back to character if needed
    gtk_label_set_max_width_chars(GTK_LABEL(label), 45);
    // max 45 chars wide — bubbles never stretch across the whole window
    gtk_label_set_selectable(GTK_LABEL(label), TRUE);
    // TRUE → user can highlight and copy message text
    gtk_widget_set_halign(label, GTK_ALIGN_START);

    // Dynamic CSS: background + text color vary per sender
    GtkStyleContext *ctx = gtk_widget_get_style_context(label);
    char bub_css[512];
    snprintf(bub_css, sizeof(bub_css),
             "label {"
             "  background-color: %s; color: %s;"
             "  border-radius: 16px; padding: 8px 14px; font-size: 14px;"
             "}",
             theme->bg, theme->fg);
    GtkCssProvider *prov = gtk_css_provider_new();
    gtk_css_provider_load_from_data(prov, bub_css, -1, NULL);
    gtk_style_context_add_provider(ctx, GTK_STYLE_PROVIDER(prov),
                                   GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(prov);

    // CSS class sets which corner to flatten to make the "tail"
    if (is_me)
        gtk_style_context_add_class(ctx, "bubble-me");
    else if (is_private)
        gtk_style_context_add_class(ctx, "bubble-private");
    else
        gtk_style_context_add_class(ctx, "bubble-other");

    return label;
}

// ── make_meta_row ─────────────────────────────────────────────────────────────
// Builds the small row above the bubble: "SenderName  08:57 PM"
static GtkWidget *make_meta_row(const char *sender, int is_me,
                                int is_private, const BubbleTheme *theme,
                                const char *timestamp)
{
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);

    // Sender name label
    GtkWidget *name_lbl = gtk_label_new(NULL);
    char name_markup[128];
    if (is_private) {
        snprintf(name_markup, sizeof(name_markup),
                 "<span foreground='%s' weight='bold' size='small'>🔒 %s</span>",
                 theme->name_color, is_me ? "Me (private)" : sender);
    } else {
        snprintf(name_markup, sizeof(name_markup),
                 "<span foreground='%s' weight='bold' size='small'>%s</span>",
                 theme->name_color, is_me ? "Me" : sender);
    }
    gtk_label_set_markup(GTK_LABEL(name_lbl), name_markup);
    gtk_widget_set_halign(name_lbl, GTK_ALIGN_START);

    // Timestamp label — gray, smaller
    GtkWidget *time_lbl = gtk_label_new(NULL);
    char time_markup[64];
    snprintf(time_markup, sizeof(time_markup),
             "<span foreground='#AAAAAA' size='x-small'>%s</span>", timestamp);
    gtk_label_set_markup(GTK_LABEL(time_lbl), time_markup);
    gtk_widget_set_halign(time_lbl, GTK_ALIGN_START);
    gtk_widget_set_valign(time_lbl, GTK_ALIGN_END);

    gtk_box_pack_start(GTK_BOX(row), name_lbl, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(row), time_lbl, FALSE, FALSE, 0);

    return row;
}

// ── append_bubble ─────────────────────────────────────────────────────────────
// The main public function of this file.
// Builds a complete message row and appends it to chat_list.
void append_bubble(const char *sender, const char *text,
                   int is_me, int is_private, int is_system)
{
    char timestamp[20];
    get_timestamp(timestamp, sizeof(timestamp));

    // ── SYSTEM MESSAGE: centered gray italic line ──────────────────────────
    if (is_system) {
        GtkWidget *row   = gtk_list_box_row_new();
        GtkWidget *label = gtk_label_new(NULL);

        char markup[512];
        snprintf(markup, sizeof(markup),
                 "<span foreground='#999994' style='italic' size='small'>%s</span>",
                 text);
        gtk_label_set_markup(GTK_LABEL(label), markup);
        gtk_label_set_xalign(GTK_LABEL(label), 0.5); // center horizontally
        gtk_widget_set_margin_top(label, 4);
        gtk_widget_set_margin_bottom(label, 4);
        gtk_style_context_add_class(
            gtk_widget_get_style_context(label), "bubble-system");

        gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), FALSE);
        gtk_list_box_row_set_selectable(GTK_LIST_BOX_ROW(row), FALSE);
        gtk_container_add(GTK_CONTAINER(row), label);
        gtk_widget_show_all(row);
        gtk_list_box_insert(GTK_LIST_BOX(chat_list), row, -1);
        scroll_to_bottom();
        return;
    }

    // ── CHAT / PRIVATE MESSAGE ────────────────────────────────────────────
    const BubbleTheme *theme = is_me      ? &ME_THEME  :
                               is_private ? &PVT_THEME :
                               get_theme(sender);

    // Outer list row — not clickable, not selectable
    GtkWidget *row = gtk_list_box_row_new();
    gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), FALSE);
    gtk_list_box_row_set_selectable(GTK_LIST_BOX_ROW(row), FALSE);
    gtk_widget_set_margin_top(row, 3);
    gtk_widget_set_margin_bottom(row, 3);

    // Horizontal box: holds avatar + content (order depends on is_me)
    GtkWidget *msg_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_margin_start(msg_box, 12);
    gtk_widget_set_margin_end(msg_box,  12);

    // Build the three sub-widgets using helper functions
    GtkWidget *avatar    = make_avatar(sender, is_me, theme);
    GtkWidget *meta_row  = make_meta_row(sender, is_me, is_private, theme, timestamp);
    GtkWidget *bubble    = make_bubble_label(text, is_me, is_private, theme);

    // Vertical column: meta_row on top, bubble below
    GtkWidget *content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 3);
    gtk_widget_set_valign(content, GTK_ALIGN_CENTER);
    gtk_box_pack_start(GTK_BOX(content), meta_row, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(content), bubble,   FALSE, FALSE, 0);

    // Invisible spacer that takes all remaining horizontal space,
    // pushing the real content to the correct side
    GtkWidget *spacer = gtk_label_new("");
    gtk_widget_set_hexpand(spacer, TRUE);

    if (is_me) {
        // MY messages go to the RIGHT:  [spacer][content][avatar]
        gtk_box_pack_start(GTK_BOX(msg_box), spacer,  TRUE,  TRUE,  0);
        gtk_box_pack_start(GTK_BOX(msg_box), content, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(msg_box), avatar,  FALSE, FALSE, 0);
    } else {
        // OTHERS go to the LEFT:  [avatar][content][spacer]
        gtk_box_pack_start(GTK_BOX(msg_box), avatar,  FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(msg_box), content, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(msg_box), spacer,  TRUE,  TRUE,  0);
    }

    gtk_container_add(GTK_CONTAINER(row), msg_box);
    gtk_widget_show_all(row);
    gtk_list_box_insert(GTK_LIST_BOX(chat_list), row, -1);
    // -1 = insert at the end of the list

    scroll_to_bottom();
}

// ── compatibility wrappers ────────────────────────────────────────────────────
// Lets old call sites like append_chat("msg") keep working unchanged.
void append_chat(const char *msg)       { append_bubble(NULL, msg, 0, 0, 1); }
void append_system_msg(const char *msg) { append_bubble(NULL, msg, 0, 0, 1); }
