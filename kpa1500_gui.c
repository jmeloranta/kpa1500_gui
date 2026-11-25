/*
 * Simple application to control Elecraft KPA1500.
 *
 */

#include <gtk/gtk.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <errno.h>

// Initial hostname (can be changed in the GUI - no need to edit)
#define DEFAULT_AMP_HOST "192.168.1.100"
#define AMP_PORT "1500"
#define BUF_SIZE 256
#define CONNECT_TIMEOUT_MS 5000

enum {
    STATE_DISCONNECTED = 0,
    STATE_CONNECTING   = 1,
    STATE_CONNECTED    = 2
};

typedef struct {
    int sock;
    struct sockaddr_in amp_addr;
    char amp_host[256];
    int conn_state;
    gint64 connect_start_us;
    int missed_polls;
    int is_operate;
    GtkWidget *win;
    GtkWidget *btn_connect;
    GtkWidget *btn_mode;
    GtkWidget *lbl_band;
    GtkWidget *lbl_fwd;
    GtkWidget *lbl_ref;
    GtkWidget *lbl_swr;
    GtkWidget *lbl_temp;
    GtkWidget *lbl_curr;
    GtkWidget *bar_fwd;
    GtkWidget *lbl_conn;
} AppWidgets;

typedef struct {
    AppWidgets *app;
    GtkWidget *dialog;
    GtkWidget *entry;
} ConnectDialogData;

static void get_config_path(char *path, size_t size) {

  const char *home = getenv("HOME");

  if (home && home[0] != '\0')
    snprintf(path, size, "%s/.config/kpa1500_ctrl.conf", home);
  else
    snprintf(path, size, "kpa1500_ctrl.conf");
}

static void save_last_host(const char *host) {

  const char *home;
  char path[512], dir[512];
  FILE *fp;
  
  home = getenv("HOME");
  if (home && home[0] != '\0') {
    snprintf(dir, sizeof(dir), "%s/.config", home);
    mkdir(dir, 0755);
  }
  
  get_config_path(path, sizeof(path));
  if ((fp = fopen(path, "W"))) {
    fprintf(fp, "%s\n", host);
    fclose(fp);
  }
}

static void load_last_host(char *host, size_t size) {

  char path[512];
  size_t len;
  FILE *fp;
  
  get_config_path(path, sizeof(path));
  if ((fp = fopen(path, "r"))) {
    if (fgets(host, (int)size, fp) != NULL) {
      len = strlen(host);
      while (len > 0 && (host[len - 1] == '\r' || host[len - 1] == '\n' || host[len - 1] == ' '))
	host[--len] = '\0';
      if (len > 0) {
	fclose(fp);
	return;
      }
    }
    fclose(fp);
  }
  snprintf(host, size, "%s", DEFAULT_AMP_HOST);
}

static const char* band_to_str(int band_num) {

  switch (band_num) {
  case 0:  return "160 m";
  case 1:  return "80 m";
  case 2:  return "60 m";
  case 3:  return "40 m";
  case 4:  return "30 m";
  case 5:  return "20 m";
  case 6:  return "17 m";
  case 7:  return "15 m";
  case 8:  return "12 m";
  case 9:  return "10 m";
  case 10: return "6 m";
  default: return "----";
  }
}

static void flush_udp_socket(int sock) {

  char trash[BUF_SIZE];
  struct sockaddr_in src;
  socklen_t slen = sizeof(src);

  while (recvfrom(sock, trash, sizeof(trash), MSG_DONTWAIT, (struct sockaddr *)&src, &slen) > 0);
}

static int start_connect(AppWidgets *app, const char *host) {

  struct addrinfo hints = {0};
  struct addrinfo *res = NULL;
  char msg[512];
  
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_DGRAM;
  
  snprintf(app->amp_host, sizeof(app->amp_host), "%s", host);
  
  if (getaddrinfo(host, AMP_PORT, &hints, &res) != 0 || res == NULL) {
    app->conn_state = STATE_DISCONNECTED;
    if (app->lbl_conn) {
      snprintf(msg, sizeof(msg), "Resolve failed: %s", host);
      gtk_label_set_text(GTK_LABEL(app->lbl_conn), msg);
    }
    return -1;
  }
  
  memcpy(&app->amp_addr, res->ai_addr, sizeof(struct sockaddr_in));
  freeaddrinfo(res);
  
  app->conn_state = STATE_CONNECTING;
  app->connect_start_us = g_get_monotonic_time();
  app->missed_polls = 0;
  
  if (app->lbl_conn) {
    snprintf(msg, sizeof(msg), "Connecting to %s...", app->amp_host);
    gtk_label_set_text(GTK_LABEL(app->lbl_conn), msg);
  }
  return 0;
}

static int kpa_query(AppWidgets *app, const char *cmd, char *resp) {

  socklen_t addr_len;
  ssize_t len;
  
  if (app->conn_state == STATE_DISCONNECTED) return -1;

  flush_udp_socket(app->sock);
  
  if (sendto(app->sock, cmd, strlen(cmd), 0, (struct sockaddr *)&app->amp_addr, sizeof(app->amp_addr)) < 0) return -1;

  addr_len = sizeof(app->amp_addr);
  len = recvfrom(app->sock, resp, BUF_SIZE - 1, 0, (struct sockaddr *)&app->amp_addr, &addr_len);
  if (len <= 0) {
    resp[0] = '\0';
    return -1;
  }
  resp[len] = '\0';
  
  while (len > 0 && (resp[len - 1] == '\r' || resp[len - 1] == '\n')) resp[--len] = '\0';

  return (int)len;
}

static void kpa_send_set(AppWidgets *app, const char *cmd) {

  if (app->conn_state != STATE_CONNECTED) return;
  
  //  printf("TX -> %s\n", cmd);
  fflush(stdout);
  if (sendto(app->sock, cmd, strlen(cmd), 0, (struct sockaddr *)&app->amp_addr, sizeof(app->amp_addr)) < 0)
    perror("sendto SET");

  usleep(25000);
}

static void set_button_state(AppWidgets *app, int operate) {

  if (operate) {
    app->is_operate = 1;
    gtk_button_set_label(GTK_BUTTON(app->btn_mode), "OPERATE");
    gtk_widget_remove_css_class(app->btn_mode, "btn-standby");
    gtk_widget_add_css_class(app->btn_mode, "btn-operate");
  } else {
    app->is_operate = 0;
    gtk_button_set_label(GTK_BUTTON(app->btn_mode), "STANDBY");
    gtk_widget_remove_css_class(app->btn_mode, "btn-operate");
    gtk_widget_add_css_class(app->btn_mode, "btn-standby");
  }
}

static void on_mode_clicked(GtkWidget *widget, gpointer data) {

  (void) widget;
  AppWidgets *app = (AppWidgets *) data;
  
  if (app->conn_state != STATE_CONNECTED) {
    gtk_label_set_text(GTK_LABEL(app->lbl_conn), "Not connected — click Connect");
    return;
  }
  
  if (app->is_operate) {
    kpa_send_set(app, "^OS0;");
    set_button_state(app, 0);
  } else {
    kpa_send_set(app, "^OS1;");
    set_button_state(app, 1);
  }
}

static void on_dialog_connect_clicked(GtkWidget *widget, gpointer user_data) {

  (void) widget;
  ConnectDialogData *dlg = (ConnectDialogData *) user_data;
  const char *host = gtk_editable_get_text(GTK_EDITABLE(dlg->entry));

  if (host && strlen(host) > 0)
    start_connect(dlg->app, host);

  gtk_window_destroy(GTK_WINDOW(dlg->dialog));
  g_free(dlg);
}

static void on_dialog_cancel_clicked(GtkWidget *widget, gpointer user_data) {

  (void) widget;
  ConnectDialogData *dlg = (ConnectDialogData *)user_data;
  
  gtk_window_destroy(GTK_WINDOW(dlg->dialog));
  g_free(dlg);
}

static void on_connect_clicked(GtkWidget *widget, gpointer data) {

  (void) widget;
  AppWidgets *app = (AppWidgets *)data;
  GtkWidget *dialog, *vbox, *lbl, *entry, *hbox, *btn_cancel, *btn_ok;
  ConnectDialogData *dlg;  

  dialog = gtk_window_new();
  gtk_window_set_title(GTK_WINDOW(dialog), "Connect KPA1500");
  gtk_window_set_transient_for(GTK_WINDOW(dialog), GTK_WINDOW(app->win));
  gtk_window_set_modal(GTK_WINDOW(dialog), TRUE);
  gtk_window_set_default_size(GTK_WINDOW(dialog), 260, 110);
  
  vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
  gtk_widget_set_margin_top(vbox, 10);
  gtk_widget_set_margin_bottom(vbox, 10);
  gtk_widget_set_margin_start(vbox, 10);
  gtk_widget_set_margin_end(vbox, 10);
  gtk_window_set_child(GTK_WINDOW(dialog), vbox);
  
  lbl = gtk_label_new("Hostname or IP:");
  gtk_widget_set_halign(lbl, GTK_ALIGN_START);
  gtk_box_append(GTK_BOX(vbox), lbl);
  
  entry = gtk_entry_new();
  gtk_editable_set_text(GTK_EDITABLE(entry), app->amp_host);
  gtk_box_append(GTK_BOX(vbox), entry);
  
  hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_widget_set_halign(hbox, GTK_ALIGN_END);
  gtk_box_append(GTK_BOX(vbox), hbox);
  
  btn_cancel = gtk_button_new_with_label("Cancel");
  btn_ok = gtk_button_new_with_label("Connect");
  gtk_box_append(GTK_BOX(hbox), btn_cancel);
  gtk_box_append(GTK_BOX(hbox), btn_ok);
  
  dlg = g_new(ConnectDialogData, 1);
  dlg->app = app;
  dlg->dialog = dialog;
  dlg->entry = entry;
  
  g_signal_connect(entry, "activate", G_CALLBACK(on_dialog_connect_clicked), dlg);
  g_signal_connect(btn_ok, "clicked", G_CALLBACK(on_dialog_connect_clicked), dlg);
  g_signal_connect(btn_cancel, "clicked", G_CALLBACK(on_dialog_cancel_clicked), dlg);
  
  gtk_window_present(GTK_WINDOW(dialog));
}

static gboolean poll_amplifier(gpointer data) {

  AppWidgets *app = (AppWidgets *) data;
  char resp[BUF_SIZE];
  char text[512];
  int volts_x10 = 0, curr_x10 = 0;

  if (app->conn_state == STATE_DISCONNECTED) return G_SOURCE_CONTINUE;

  if (app->conn_state == STATE_CONNECTING) {
    gint64 elapsed_ms = (g_get_monotonic_time() - app->connect_start_us) / 1000;
    if (elapsed_ms >= CONNECT_TIMEOUT_MS) {
      app->conn_state = STATE_DISCONNECTED;
      snprintf(text, sizeof(text), "Aborted: no reply (5s)");
      gtk_label_set_text(GTK_LABEL(app->lbl_conn), text);
      return G_SOURCE_CONTINUE;
    }
    
    if (kpa_query(app, "^OS;", resp) > 0 && strncmp(resp, "^OS", 3) == 0) {
      app->conn_state = STATE_CONNECTED;
      app->missed_polls = 0;
      save_last_host(app->amp_host);
      snprintf(text, sizeof(text), "Connected: %s", app->amp_host);
      gtk_label_set_text(GTK_LABEL(app->lbl_conn), text);
      set_button_state(app, strstr(resp, "^OS1;") != NULL);
    } else {
      int rem_sec = (int)((CONNECT_TIMEOUT_MS - elapsed_ms + 999) / 1000);
      snprintf(text, sizeof(text), "Connecting %s (%ds)...", app->amp_host, rem_sec);
      gtk_label_set_text(GTK_LABEL(app->lbl_conn), text);
      return G_SOURCE_CONTINUE;
    }
  }
  
  // 1. Poll Operate/Standby Mode
  if (kpa_query(app, "^OS;", resp) > 0 && strncmp(resp, "^OS", 3) == 0) {
    app->missed_polls = 0;
    snprintf(text, sizeof(text), "Connected: %s", app->amp_host);
    gtk_label_set_text(GTK_LABEL(app->lbl_conn), text);
    set_button_state(app, strstr(resp, "^OS1;") != NULL);
  } else {
    app->missed_polls++;
    if (app->missed_polls >= 4) {
      snprintf(text, sizeof(text), "No response: %s", app->amp_host);
      gtk_label_set_text(GTK_LABEL(app->lbl_conn), text);
    }
    return G_SOURCE_CONTINUE;
  }
  
  // 2. Poll Current Band
  int band = -1;
  if (kpa_query(app, "^BN;", resp) > 0 && sscanf(resp, "^BN%d;", &band) == 1) {
    snprintf(text, sizeof(text), "%s", band_to_str(band));
    gtk_label_set_text(GTK_LABEL(app->lbl_band), text);
  }
  
  // 3. Poll Forward Power and SWR (^WS;)
  int fwd = 0, swr_x10 = 10;
  if (kpa_query(app, "^WS;", resp) > 0 && sscanf(resp, "^WS%4d%3d;", &fwd, &swr_x10) == 2) {
    snprintf(text, sizeof(text), "%d W", fwd);
    gtk_label_set_text(GTK_LABEL(app->lbl_fwd), text);
    
    double frac = (double) fwd / 1500.0;
    if (frac > 1.0) frac = 1.0;
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(app->bar_fwd), frac);
    
    if (swr_x10 < 10) swr_x10 = 10;
    snprintf(text, sizeof(text), "%.1f", swr_x10 / 10.0);
    gtk_label_set_text(GTK_LABEL(app->lbl_swr), text);
  }
  
  // 4. Poll Reflected Power (^PWR;)
  int ref = 0;
  if (kpa_query(app, "^PWR;", resp) > 0 && sscanf(resp, "^PWR%d;", &ref) == 1) {
    snprintf(text, sizeof(text), "%d W", ref);
    gtk_label_set_text(GTK_LABEL(app->lbl_ref), text);
  }
  
  // 5. Poll Amplifier Temperature (^TM;)
  int temp_c = 0;
  if (kpa_query(app, "^TM;", resp) > 0 && sscanf(resp, "^TM%d;", &temp_c) == 1) {
    snprintf(text, sizeof(text), "%d °C", temp_c);
    gtk_label_set_text(GTK_LABEL(app->lbl_temp), text);
  }
  
  // 6. Poll Amplifier PA Current (^VI;) -> ^VIvvv iii; (iii = tenths of Amps)
  if (kpa_query(app, "^VI;", resp) > 0 && sscanf(resp, "^VI%3d %3d;", &volts_x10, &curr_x10) == 2) {
    snprintf(text, sizeof(text), "%.1f A", curr_x10);
    gtk_label_set_text(GTK_LABEL(app->lbl_curr), text);
  }
  
  return G_SOURCE_CONTINUE;
}

static void load_css(void) {

  GtkCssProvider *provider = gtk_css_provider_new();
  const char *css =
    "button.btn-standby {"
    "  background: #f1c40f;"
    "  color: #111111;"
    "  font-weight: bold;"
    "  border-radius: 4px;"
    "  padding: 4px 8px;"
    "  min-height: 24px;"
    "}"
    "button.btn-standby:hover {"
    "  background: #f39c12;"
    "}"
    "button.btn-operate {"
    "  background: #e74c3c;"
    "  color: #ffffff;"
    "  font-weight: bold;"
    "  border-radius: 4px;"
    "  padding: 4px 8px;"
    "  min-height: 24px;"
    "}"
    "button.btn-operate:hover {"
    "  background: #c0392b;"
    "}"
    "button.btn-compact {"
    "  padding: 4px 8px;"
    "  min-height: 24px;"
    "}"
    ".metric-title {"
    "  font-size: 0.82em;"
    "  opacity: 0.75;"
    "}"
    ".metric-val {"
    "  font-weight: bold;"
    "}"
    ".status-line {"
    "  font-size: 0.82em;"
    "  opacity: 0.8;"
    "}";
  
  gtk_css_provider_load_from_string(provider, css);
  gtk_style_context_add_provider_for_display(gdk_display_get_default(),
					     GTK_STYLE_PROVIDER(provider),
					     GTK_STYLE_PROVIDER_PRIORITY_USER
					    );
  g_object_unref(provider);
}

static GtkWidget* make_metric_cell(const char *title, GtkWidget **val_label) {

  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 1);
  GtkWidget *lbl_t = gtk_label_new(title);

  gtk_widget_add_css_class(lbl_t, "metric-title");
  gtk_widget_set_halign(lbl_t, GTK_ALIGN_START);
  
  *val_label = gtk_label_new("---");
  gtk_widget_add_css_class(*val_label, "metric-val");
  gtk_widget_set_halign(*val_label, GTK_ALIGN_START);
  
  gtk_box_append(GTK_BOX(box), lbl_t);
  gtk_box_append(GTK_BOX(box), *val_label);

  return box;
}

static void on_activate(GtkApplication *gtk_app, gpointer user_data) {

  AppWidgets *app = (AppWidgets *) user_data;
  GtkWidget *vbox, *top_row, *grid;
  char status[512];
  
  load_css();

  app->win = gtk_application_window_new(gtk_app);
  gtk_window_set_title(GTK_WINDOW(app->win), "KPA1500");
  gtk_window_set_default_size(GTK_WINDOW(app->win), 250, 165);
  gtk_window_set_resizable(GTK_WINDOW(app->win), FALSE);
  
  vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
  gtk_widget_set_margin_top(vbox, 8);
  gtk_widget_set_margin_bottom(vbox, 8);
  gtk_widget_set_margin_start(vbox, 8);
  gtk_widget_set_margin_end(vbox, 8);
  gtk_window_set_child(GTK_WINDOW(app->win), vbox);
  
  // Top row: Connect button + Operate/Standby button side-by-side
  top_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_box_append(GTK_BOX(vbox), top_row);
  
  app->btn_connect = gtk_button_new_with_label("Connect");
  gtk_widget_add_css_class(app->btn_connect, "btn-compact");
  g_signal_connect(app->btn_connect, "clicked", G_CALLBACK(on_connect_clicked), app);
  gtk_box_append(GTK_BOX(top_row), app->btn_connect);
  
  app->btn_mode = gtk_button_new_with_label("STANDBY");
  gtk_widget_set_hexpand(app->btn_mode, TRUE);
  gtk_widget_add_css_class(app->btn_mode, "btn-standby");
  g_signal_connect(app->btn_mode, "clicked", G_CALLBACK(on_mode_clicked), app);
  gtk_box_append(GTK_BOX(top_row), app->btn_mode);
  
  // Compact 3x2 grid for telemetry readouts
  grid = gtk_grid_new();
  gtk_grid_set_row_spacing(GTK_GRID(grid), 4);
  gtk_grid_set_column_spacing(GTK_GRID(grid), 12);
  gtk_grid_set_column_homogeneous(GTK_GRID(grid), TRUE);
  gtk_box_append(GTK_BOX(vbox), grid);
  
  gtk_grid_attach(GTK_GRID(grid), make_metric_cell("BAND", &app->lbl_band), 0, 0, 1, 1);
  gtk_grid_attach(GTK_GRID(grid), make_metric_cell("FWD",  &app->lbl_fwd),  1, 0, 1, 1);
  gtk_grid_attach(GTK_GRID(grid), make_metric_cell("REF",  &app->lbl_ref),  2, 0, 1, 1);
  
  gtk_grid_attach(GTK_GRID(grid), make_metric_cell("SWR",  &app->lbl_swr),  0, 1, 1, 1);
  gtk_grid_attach(GTK_GRID(grid), make_metric_cell("TEMP", &app->lbl_temp), 1, 1, 1, 1);
  gtk_grid_attach(GTK_GRID(grid), make_metric_cell("CURR", &app->lbl_curr), 2, 1, 1, 1);
  
  gtk_label_set_text(GTK_LABEL(app->lbl_fwd),  "0 W");
  gtk_label_set_text(GTK_LABEL(app->lbl_ref),  "0 W");
  gtk_label_set_text(GTK_LABEL(app->lbl_swr),  "1.0:1");
  gtk_label_set_text(GTK_LABEL(app->lbl_temp), "-- °C");
  gtk_label_set_text(GTK_LABEL(app->lbl_curr), "0.0 A");
  
  // Thin forward power bar
  app->bar_fwd = gtk_progress_bar_new();
  gtk_box_append(GTK_BOX(vbox), app->bar_fwd);
  
  // Compact bottom status line
  app->lbl_conn = gtk_label_new("Not Connected");
  gtk_widget_add_css_class(app->lbl_conn, "status-line");
  gtk_widget_set_halign(app->lbl_conn, GTK_ALIGN_START);
  gtk_label_set_ellipsize(GTK_LABEL(app->lbl_conn), PANGO_ELLIPSIZE_END);
  gtk_box_append(GTK_BOX(vbox), app->lbl_conn);
  
  if (app->conn_state == STATE_CONNECTING) {
    snprintf(status, sizeof(status), "Connecting %s (5s)...", app->amp_host);
    gtk_label_set_text(GTK_LABEL(app->lbl_conn), status);
  }
  
  g_timeout_add(250, poll_amplifier, app);
  gtk_window_present(GTK_WINDOW(app->win));
}

int main(int argc, char *argv[]) {

  GtkApplication *gtk_app;
  AppWidgets app = {0};
  char initial_host[256];
  struct timeval tv = { .tv_sec = 0, .tv_usec = 200000 };
  int status;
  
  app.sock = socket(AF_INET, SOCK_DGRAM, 0);
  if (app.sock < 0) {
    perror("socket");
    return 1;
  }
  
  setsockopt(app.sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  
  if (argc > 1 && argv[1][0] != '\0')
    snprintf(initial_host, sizeof(initial_host), "%s", argv[1]);
  else
    load_last_host(initial_host, sizeof(initial_host));

  start_connect(&app, initial_host);
  
  gtk_app = gtk_application_new("org.hamradio.kpa1500", G_APPLICATION_DEFAULT_FLAGS);
  g_signal_connect(gtk_app, "activate", G_CALLBACK(on_activate), &app);
  
  status = g_application_run(G_APPLICATION(gtk_app), 1, argv);
  
  g_object_unref(gtk_app);
  close(app.sock);

  return status;
}
