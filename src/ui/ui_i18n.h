/* SPDX-License-Identifier: GPL-3.0-or-later */
/* ui_i18n.h — the on-screen UI's words, by id, in the language chosen
 * (ui.language).
 *
 * Every string is a row of UI_STRINGS below: an id and its English text.
 * A translation is a file ui_i18n_<code>.c with a table of the same ids
 * (a missing one falls back to English) and a line in ui_i18n.c's list of
 * languages; the Language selector offers exactly the languages listed.
 * Code says TR(S_NETWORK), never the English text, for every string that
 * is in the table. The pages written before the table still have some
 * English in place; moving one is: add its row, use TR().
 */
#ifndef UI_I18N_H
#define UI_I18N_H

#define UI_STRINGS(X) \
    X(S_CAMERA, "Camera") \
    X(S_NETWORK, "Network") \
    X(S_STORAGE, "Storage") \
    X(S_SYSTEM, "System") \
    X(S_ACCOUNT, "Account") \
    X(S_ETHERNET, "Ethernet") \
    X(S_WIFI, "Wi-Fi") \
    X(S_LTE, "LTE") \
    X(S_DDNS, "DDNS") \
    X(S_UPNP, "UPnP") \
    X(S_SMTP, "SMTP") \
    X(S_ALARM_CENTER, "Alarm center") \
    X(S_P2P, "P2P") \
    X(S_QOS, "QoS") \
    X(S_BASIC, "Basic") \
    X(S_SCHEDULE, "Schedule") \
    X(S_DISK_MANAGER, "Disk Manager") \
    X(S_DISK_MAINT, "Disk Maintenance") \
    X(S_DISK_QUOTA, "Disk Quota") \
    X(S_NET_STORAGE, "Network Storage") \
    X(S_GENERAL, "General") \
    X(S_USERS, "Users") \
    X(S_GROUPS, "Groups") \
    X(S_SAVE, "Save") \
    X(S_CANCEL, "Cancel") \
    X(S_DISCARD_Q, "Discard the changes on this page?") \
    X(S_DISCARD, "Discard") \
    X(S_SAVED, "Saved") \
    X(S_NOT_MANAGED, "vigild does not manage this system (it is not OpenIPC firmware): shown, not changed.") \
    X(S_KEEP_Q, "Keep these network settings?") \
    X(S_KEEP, "Keep") \
    X(S_UNDO, "Undo") \
    X(S_DEVICE_NAME, "Device name") \
    X(S_LANGUAGE, "Language") \
    X(S_DATE_TIME, "Date and time") \
    X(S_TIME_ZONE, "Time zone") \
    X(S_DST, "Daylight saving time") \
    X(S_NTP, "NTP") \
    X(S_SYNC_NOW, "Sync now") \
    X(S_FORMAT, "Format") \
    X(S_MOUNT, "Mount") \
    X(S_UNMOUNT, "Unmount") \
    X(S_TEST, "Test") \
    X(S_ADD, "Add") \
    X(S_EDIT, "Edit") \
    X(S_REMOVE, "Remove") \
    X(S_ADD_CAMERA, "Add camera") \
    X(S_ST_DISABLED, "Disabled") \
    X(S_ST_CONNECTING, "Connecting...") \
    X(S_ST_NO_DECODER, "No decoder") \
    X(S_ST_NO_VIDEO, "No video") \
    X(S_ST_OFFLINE, "Offline") \
    X(S_ST_AUTH, "Authentication error") \
    X(S_ST_CODEC, "Unsupported codec") \
    X(S_EXIT, "Exit") \
    X(S_LOGOUT, "Logout") \
    X(S_REBOOT, "Reboot") \
    X(S_SHUTDOWN, "Shutdown") \
    X(S_REBOOT_Q, "Reboot the recorder? Recording stops until it is back up.") \
    X(S_SHUTDOWN_Q, "Shut the recorder down? Recording stops until it is switched on again.") \
    X(S_REBOOTING, "Rebooting: recording stops cleanly first") \
    X(S_SHUTTING_DOWN, "Shutting down: recording stops cleanly first") \
    X(S_POWER_UNMANAGED, "Not here: vigild does not manage this system") \
    X(S_LOGIN_OFF, "Logout: the on-screen login is off (System > General).") \
    X(S_LOG_IN, "Log in") \
    X(S_USER, "User") \
    X(S_PASSWORD, "Password") \
    X(S_WRONG_LOGIN, "Wrong user or password") \
    X(S_NO_PERMISSION, "no permission for this") \
    X(S_LOGGED_OUT, "Logged out")

typedef enum {
#define UI_STR_ID(id, en) id,
    UI_STRINGS(UI_STR_ID)
#undef UI_STR_ID
    S_COUNT
} ui_str_t;

/* The text of `id` in the language in force. */
const char *ui_tr(ui_str_t id);
#define TR(id) ui_tr(id)

/* The languages there are: codes ("en") and names in their own language
 * ("English"); NULL past the last. */
const char *ui_i18n_code(int i);
const char *ui_i18n_name(int i);

/* Use language `code` (an unknown one: English). */
void ui_i18n_set(const char *code);

#endif /* UI_I18N_H */
