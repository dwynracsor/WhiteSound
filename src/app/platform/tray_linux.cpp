// Linux system tray backend.
//
// Implements the StatusNotifierItem protocol (org.kde.StatusNotifierItem) plus
// the com.canonical.dbusmenu interface used by the desktop's tray host to draw
// the context menu, directly over the session D-Bus with libdbus (no GTK, no
// GTK main loop). The shell drives it from its own loop: pump() reads and
// dispatches pending D-Bus traffic once per iteration.
//
// The item lives on this process's unique bus name at the conventional paths
// /StatusNotifierItem and /MenuBar, and registers itself with the
// StatusNotifierWatcher. If the watcher is not running yet, the match on
// NameOwnerChanged makes us register as soon as it appears.

#include "tray.h"

#include <dbus/dbus.h>

#include <cstdio>
#include <cstring>
#include <string>

#include "app/platform/tray_icons.h"

namespace ws {
namespace platform {
namespace {

const char *kSniPath = "/StatusNotifierItem";
const char *kMenuPath = "/MenuBar";
const char *kSniIface = "org.kde.StatusNotifierItem";
const char *kMenuIface = "com.canonical.dbusmenu";
const char *kPropsIface = "org.freedesktop.DBus.Properties";
const char *kIntrospectIface = "org.freedesktop.DBus.Introspectable";
const char *kWatcherName = "org.kde.StatusNotifierWatcher";
const char *kWatcherPath = "/StatusNotifierWatcher";
const char *kWatcherIface = "org.kde.StatusNotifierWatcher";

// Stable menu item ids (the root is 0 by convention).
enum {
    kIdRoot = 0,
    kIdOpen = 1,
    kIdSep1 = 2,
    kIdEnable = 3,
    kIdSep2 = 4,
    kIdConfig = 5,
    kIdAbout = 6,
    kIdSep3 = 7,
    kIdQuit = 8,
};

const char *kIntrospectionXml =
    "<!DOCTYPE node PUBLIC \"-//freedesktop//DTD D-BUS Object Introspection 1.0//EN\"\n"
    " \"http://www.freedesktop.org/standards/dbus/1.0/introspect.dtd\">\n"
    "<node>\n"
    "  <interface name=\"org.kde.StatusNotifierItem\">\n"
    "    <property name=\"Category\" type=\"s\" access=\"read\"/>\n"
    "    <property name=\"Id\" type=\"s\" access=\"read\"/>\n"
    "    <property name=\"Title\" type=\"s\" access=\"read\"/>\n"
    "    <property name=\"Status\" type=\"s\" access=\"read\"/>\n"
    "    <property name=\"IconName\" type=\"s\" access=\"read\"/>\n"
    "    <property name=\"IconPixmap\" type=\"a(iiay)\" access=\"read\"/>\n"
    "    <property name=\"Menu\" type=\"o\" access=\"read\"/>\n"
    "    <property name=\"ItemIsMenu\" type=\"b\" access=\"read\"/>\n"
    "    <method name=\"Activate\"><arg name=\"x\" type=\"i\" direction=\"in\"/>"
    "<arg name=\"y\" type=\"i\" direction=\"in\"/></method>\n"
    "    <method name=\"SecondaryActivate\"><arg name=\"x\" type=\"i\" direction=\"in\"/>"
    "<arg name=\"y\" type=\"i\" direction=\"in\"/></method>\n"
    "    <method name=\"ContextMenu\"><arg name=\"x\" type=\"i\" direction=\"in\"/>"
    "<arg name=\"y\" type=\"i\" direction=\"in\"/></method>\n"
    "    <method name=\"Scroll\"><arg name=\"delta\" type=\"i\" direction=\"in\"/>"
    "<arg name=\"orientation\" type=\"s\" direction=\"in\"/></method>\n"
    "    <signal name=\"NewIcon\"/>\n"
    "  </interface>\n"
    "  <interface name=\"com.canonical.dbusmenu\">\n"
    "    <property name=\"Version\" type=\"u\" access=\"read\"/>\n"
    "    <property name=\"TextDirection\" type=\"s\" access=\"read\"/>\n"
    "    <property name=\"Status\" type=\"s\" access=\"read\"/>\n"
    "    <method name=\"GetLayout\"><arg name=\"parentId\" type=\"i\" direction=\"in\"/>"
    "<arg name=\"recursionDepth\" type=\"i\" direction=\"in\"/>"
    "<arg name=\"propertyNames\" type=\"as\" direction=\"in\"/>"
    "<arg name=\"revision\" type=\"u\" direction=\"out\"/>"
    "<arg name=\"layout\" type=\"(ia{sv}av)\" direction=\"out\"/></method>\n"
    "    <method name=\"GetGroupProperties\"><arg name=\"ids\" type=\"ai\" direction=\"in\"/>"
    "<arg name=\"propertyNames\" type=\"as\" direction=\"in\"/>"
    "<arg name=\"properties\" type=\"a(ia{sv})\" direction=\"out\"/></method>\n"
    "    <method name=\"GetProperty\"><arg name=\"id\" type=\"i\" direction=\"in\"/>"
    "<arg name=\"name\" type=\"s\" direction=\"in\"/>"
    "<arg name=\"value\" type=\"v\" direction=\"out\"/></method>\n"
    "    <method name=\"Event\"><arg name=\"id\" type=\"i\" direction=\"in\"/>"
    "<arg name=\"eventId\" type=\"s\" direction=\"in\"/>"
    "<arg name=\"data\" type=\"v\" direction=\"in\"/>"
    "<arg name=\"timestamp\" type=\"u\" direction=\"in\"/></method>\n"
    "    <method name=\"EventGroup\"><arg name=\"events\" type=\"a(isvu)\" direction=\"in\"/>"
    "<arg name=\"idErrors\" type=\"ai\" direction=\"out\"/></method>\n"
    "    <method name=\"AboutToShow\"><arg name=\"id\" type=\"i\" direction=\"in\"/>"
    "<arg name=\"needUpdate\" type=\"b\" direction=\"out\"/></method>\n"
    "    <method name=\"AboutToShowGroup\"><arg name=\"ids\" type=\"ai\" direction=\"in\"/>"
    "<arg name=\"updatesNeeded\" type=\"ai\" direction=\"out\"/>"
    "<arg name=\"idErrors\" type=\"ai\" direction=\"out\"/></method>\n"
    "    <signal name=\"LayoutUpdated\"><arg name=\"revision\" type=\"u\"/>"
    "<arg name=\"parent\" type=\"i\"/></signal>\n"
    "  </interface>\n"
    "  <interface name=\"org.freedesktop.DBus.Properties\">\n"
    "    <method name=\"Get\"><arg name=\"interface\" type=\"s\" direction=\"in\"/>"
    "<arg name=\"property\" type=\"s\" direction=\"in\"/>"
    "<arg name=\"value\" type=\"v\" direction=\"out\"/></method>\n"
    "    <method name=\"GetAll\"><arg name=\"interface\" type=\"s\" direction=\"in\"/>"
    "<arg name=\"properties\" type=\"a{sv}\" direction=\"out\"/></method>\n"
    "  </interface>\n"
    "</node>\n";

// ---- value helpers ---------------------------------------------------------

void appendString(DBusMessageIter *it, const char *s) {
    dbus_message_iter_append_basic(it, DBUS_TYPE_STRING, &s);
}

void appendBool(DBusMessageIter *it, dbus_bool_t b) {
    dbus_message_iter_append_basic(it, DBUS_TYPE_BOOLEAN, &b);
}

bool variantString(DBusMessageIter *it, const char *s) {
    DBusMessageIter v;
    dbus_message_iter_open_container(it, DBUS_TYPE_VARIANT, "s", &v);
    appendString(&v, s);
    dbus_message_iter_close_container(it, &v);
    return true;
}

bool variantBool(DBusMessageIter *it, dbus_bool_t b) {
    DBusMessageIter v;
    dbus_message_iter_open_container(it, DBUS_TYPE_VARIANT, "b", &v);
    appendBool(&v, b);
    dbus_message_iter_close_container(it, &v);
    return true;
}

bool variantInt32(DBusMessageIter *it, dbus_int32_t value) {
    DBusMessageIter v;
    dbus_message_iter_open_container(it, DBUS_TYPE_VARIANT, "i", &v);
    dbus_message_iter_append_basic(&v, DBUS_TYPE_INT32, &value);
    dbus_message_iter_close_container(it, &v);
    return true;
}

bool variantUint32(DBusMessageIter *it, dbus_uint32_t value) {
    DBusMessageIter v;
    dbus_message_iter_open_container(it, DBUS_TYPE_VARIANT, "u", &v);
    dbus_message_iter_append_basic(&v, DBUS_TYPE_UINT32, &value);
    dbus_message_iter_close_container(it, &v);
    return true;
}

bool variantObjectPath(DBusMessageIter *it, const char *path) {
    DBusMessageIter v;
    dbus_message_iter_open_container(it, DBUS_TYPE_VARIANT, "o", &v);
    dbus_message_iter_append_basic(&v, DBUS_TYPE_OBJECT_PATH, &path);
    dbus_message_iter_close_container(it, &v);
    return true;
}

bool variantPixmap(DBusMessageIter *it, const unsigned char *argb, int width,
                   int height) {
    DBusMessageIter v, array, element, bytes;
    dbus_message_iter_open_container(it, DBUS_TYPE_VARIANT, "a(iiay)", &v);
    dbus_message_iter_open_container(&v, DBUS_TYPE_ARRAY, "(iiay)", &array);
    dbus_message_iter_open_container(&array, DBUS_TYPE_STRUCT, nullptr, &element);
    dbus_int32_t w = width;
    dbus_int32_t h = height;
    dbus_message_iter_append_basic(&element, DBUS_TYPE_INT32, &w);
    dbus_message_iter_append_basic(&element, DBUS_TYPE_INT32, &h);
    dbus_message_iter_open_container(&element, DBUS_TYPE_ARRAY, "y", &bytes);
    dbus_message_iter_append_fixed_array(&bytes, DBUS_TYPE_BYTE, argb,
                                         width * height * 4);
    dbus_message_iter_close_container(&element, &bytes);
    dbus_message_iter_close_container(&array, &element);
    dbus_message_iter_close_container(&v, &array);
    dbus_message_iter_close_container(it, &v);
    return true;
}

void addPropString(DBusMessageIter *array, const char *key, const char *value) {
    DBusMessageIter entry;
    dbus_message_iter_open_container(array, DBUS_TYPE_DICT_ENTRY, nullptr, &entry);
    appendString(&entry, key);
    variantString(&entry, value);
    dbus_message_iter_close_container(array, &entry);
}

void addPropBool(DBusMessageIter *array, const char *key, dbus_bool_t value) {
    DBusMessageIter entry;
    dbus_message_iter_open_container(array, DBUS_TYPE_DICT_ENTRY, nullptr, &entry);
    appendString(&entry, key);
    variantBool(&entry, value);
    dbus_message_iter_close_container(array, &entry);
}

void addPropInt32(DBusMessageIter *array, const char *key, dbus_int32_t value) {
    DBusMessageIter entry;
    dbus_message_iter_open_container(array, DBUS_TYPE_DICT_ENTRY, nullptr, &entry);
    appendString(&entry, key);
    variantInt32(&entry, value);
    dbus_message_iter_close_container(array, &entry);
}

bool readInt32(DBusMessageIter *it, dbus_int32_t &out) {
    if (dbus_message_iter_get_arg_type(it) != DBUS_TYPE_INT32) return false;
    dbus_message_iter_get_basic(it, &out);
    return true;
}

} // namespace

class LinuxTray : public Tray {
public:
    ~LinuxTray() override { destroy(); }

    bool create(const TrayCallbacks &callbacks) override;
    void destroy() override;
    void setEnabled(bool enabled) override;
    void showMenu() override;   // the desktop draws the dbusmenu itself
    void notify(const std::string &title, const std::string &body) override;
    void pump() override;

    // Called by the D-Bus vtable/filter thunks.
    DBusHandlerResult onSniMessage(DBusConnection *conn, DBusMessage *msg);
    DBusHandlerResult onMenuMessage(DBusConnection *conn, DBusMessage *msg);
    void registerWithWatcher();

private:
    // Property emitters.
    bool appendSniProperty(DBusMessageIter *it, const char *name);
    bool appendMenuProperty(DBusMessageIter *it, const char *name);
    void appendItemProps(DBusMessageIter *array, int id);
    void appendLayout(DBusMessageIter *it, int id);
    void appendGroupEntry(DBusMessageIter *array, int id);

    // Signals.
    void emitPropertiesChanged();
    void emitLayoutUpdated();

    void dispatchCommand(int id);
    void replyEmpty(DBusConnection *conn, DBusMessage *msg);
    void replyIntrospect(DBusConnection *conn, DBusMessage *msg);

    DBusConnection *m_conn = nullptr;
    TrayCallbacks m_cb;
    bool m_enabled = true;
    dbus_uint32_t m_revision = 1;
    bool m_registered = false;
    int m_retryTicks = 0;
};

namespace {

DBusHandlerResult sniThunk(DBusConnection *conn, DBusMessage *msg, void *user) {
    return static_cast<LinuxTray *>(user)->onSniMessage(conn, msg);
}

DBusHandlerResult menuThunk(DBusConnection *conn, DBusMessage *msg, void *user) {
    return static_cast<LinuxTray *>(user)->onMenuMessage(conn, msg);
}

DBusHandlerResult filterThunk(DBusConnection *, DBusMessage *msg, void *user) {
    auto *tray = static_cast<LinuxTray *>(user);
    if (dbus_message_is_signal(msg, DBUS_INTERFACE_DBUS, "NameOwnerChanged")) {
        const char *name = nullptr;
        const char *oldOwner = nullptr;
        const char *newOwner = nullptr;
        if (dbus_message_get_args(msg, nullptr, DBUS_TYPE_STRING, &name,
                                  DBUS_TYPE_STRING, &oldOwner, DBUS_TYPE_STRING,
                                  &newOwner, DBUS_TYPE_INVALID)) {
            if (name && std::strcmp(name, kWatcherName) == 0 && newOwner &&
                *newOwner) {
                tray->registerWithWatcher();
            }
        }
    }
    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

} // namespace

bool LinuxTray::create(const TrayCallbacks &callbacks) {
    m_cb = callbacks;

    DBusError error;
    dbus_error_init(&error);
    m_conn = dbus_bus_get(DBUS_BUS_SESSION, &error);
    if (!m_conn) {
        std::fprintf(stderr, "[tray] no D-Bus session bus: %s\n",
                     dbus_error_is_set(&error) ? error.message : "unknown");
        dbus_error_free(&error);
        return false;
    }
    dbus_error_free(&error);
    dbus_connection_set_exit_on_disconnect(m_conn, FALSE);

    static const DBusObjectPathVTable sniVtable = {nullptr, &sniThunk};
    static const DBusObjectPathVTable menuVtable = {nullptr, &menuThunk};
    if (!dbus_connection_register_object_path(m_conn, kSniPath, &sniVtable,
                                              this) ||
        !dbus_connection_register_object_path(m_conn, kMenuPath, &menuVtable,
                                              this)) {
        std::fprintf(stderr, "[tray] could not register D-Bus object paths\n");
        destroy();
        return false;
    }

    dbus_bus_add_match(m_conn,
                       "type='signal',interface='org.freedesktop.DBus',"
                       "member='NameOwnerChanged',arg0='org.kde.StatusNotifierWatcher'",
                       &error);
    if (dbus_error_is_set(&error)) {
        dbus_error_free(&error);
    } else {
        dbus_connection_flush(m_conn);
    }

    dbus_connection_add_filter(m_conn, &filterThunk, this, nullptr);

    registerWithWatcher();
    return true;
}

void LinuxTray::destroy() {
    if (!m_conn) return;

    dbus_connection_remove_filter(m_conn, &filterThunk, this);
    dbus_connection_unregister_object_path(m_conn, kSniPath);
    dbus_connection_unregister_object_path(m_conn, kMenuPath);
    dbus_connection_flush(m_conn);

    // Do not close a shared bus connection; just drop our reference.
    dbus_connection_unref(m_conn);
    m_conn = nullptr;
}

void LinuxTray::registerWithWatcher() {
    if (!m_conn) return;

    DBusMessage *call =
        dbus_message_new_method_call(kWatcherName, kWatcherPath, kWatcherIface,
                                     "RegisterStatusNotifierItem");
    if (!call) return;

    const char *service = dbus_bus_get_unique_name(m_conn);
    if (!service) service = "";
    dbus_message_append_args(call, DBUS_TYPE_STRING, &service, DBUS_TYPE_INVALID);

    DBusError error;
    dbus_error_init(&error);
    DBusMessage *reply =
        dbus_connection_send_with_reply_and_block(m_conn, call, 1000, &error);
    if (reply) {
        m_registered = true;
        dbus_message_unref(reply);
    } else {
        m_registered = false;
        dbus_error_free(&error);
    }
    dbus_message_unref(call);
}

void LinuxTray::setEnabled(bool enabled) {
    if (m_enabled == enabled) return;
    m_enabled = enabled;
    emitPropertiesChanged();
    m_revision++;
    emitLayoutUpdated();
}

void LinuxTray::showMenu() {
    // The desktop shell owns the menu (it renders the dbusmenu we expose), so
    // there is nothing to pop up here.
}

void LinuxTray::notify(const std::string &title, const std::string &body) {
    if (!m_conn) return;

    DBusMessage *call = dbus_message_new_method_call(
        "org.freedesktop.Notifications", "/org/freedesktop/Notifications",
        "org.freedesktop.Notifications", "Notify");
    if (!call) return;

    DBusMessageIter it;
    dbus_message_iter_init_append(call, &it);
    const char *app = "White Sound";
    dbus_uint32_t replaces = 0;
    const char *icon = m_enabled ? "white-sound" : "white-sound-off";
    const char *summary = title.c_str();
    const char *text = body.c_str();
    dbus_int32_t expire = -1;

    appendString(&it, app);
    dbus_message_iter_append_basic(&it, DBUS_TYPE_UINT32, &replaces);
    appendString(&it, icon);
    appendString(&it, summary);
    appendString(&it, text);

    DBusMessageIter array, dict;
    dbus_message_iter_open_container(&it, DBUS_TYPE_ARRAY, "s", &array);
    dbus_message_iter_close_container(&it, &array);
    dbus_message_iter_open_container(&it, DBUS_TYPE_ARRAY, "{sv}", &dict);
    dbus_message_iter_close_container(&it, &dict);
    dbus_message_iter_append_basic(&it, DBUS_TYPE_INT32, &expire);

    dbus_connection_send(m_conn, call, nullptr);
    dbus_message_unref(call);
}

void LinuxTray::pump() {
    if (!m_conn) return;

    dbus_connection_read_write(m_conn, 0);
    while (dbus_connection_get_dispatch_status(m_conn) ==
           DBUS_DISPATCH_DATA_REMAINS) {
        dbus_connection_dispatch(m_conn);
    }
    dbus_connection_flush(m_conn);

    // Backstop for a watcher that started before us but was not yet owning the
    // name, in case the NameOwnerChanged signal was missed.
    if (!m_registered && ++m_retryTicks >= 80) {
        m_retryTicks = 0;
        registerWithWatcher();
    }
}

// ---- properties ------------------------------------------------------------

bool LinuxTray::appendSniProperty(DBusMessageIter *it, const char *name) {
    if (std::strcmp(name, "Category") == 0) {
        return variantString(it, "ApplicationStatus");
    }
    if (std::strcmp(name, "Id") == 0) return variantString(it, "white-sound");
    if (std::strcmp(name, "Title") == 0) return variantString(it, "White Sound");
    if (std::strcmp(name, "Status") == 0) return variantString(it, "Active");
    if (std::strcmp(name, "IconName") == 0) {
        return variantString(it, m_enabled ? "white-sound" : "white-sound-off");
    }
    if (std::strcmp(name, "Menu") == 0) return variantObjectPath(it, kMenuPath);
    if (std::strcmp(name, "ItemIsMenu") == 0) return variantBool(it, FALSE);
    if (std::strcmp(name, "IconPixmap") == 0) {
        return variantPixmap(it,
                             m_enabled ? kTrayIconOnArgb : kTrayIconOffArgb,
                             kTrayIconWidth, kTrayIconHeight);
    }
    return false;
}

bool LinuxTray::appendMenuProperty(DBusMessageIter *it, const char *name) {
    if (std::strcmp(name, "Version") == 0) return variantUint32(it, 3);
    if (std::strcmp(name, "TextDirection") == 0) return variantString(it, "ltr");
    if (std::strcmp(name, "Status") == 0) return variantString(it, "normal");
    return false;
}

void LinuxTray::appendItemProps(DBusMessageIter *array, int id) {
    switch (id) {
        case kIdRoot:
            addPropString(array, "children-display", "submenu");
            break;
        case kIdOpen:
            addPropString(array, "label", "Open panel");
            addPropBool(array, "enabled", TRUE);
            addPropBool(array, "visible", TRUE);
            break;
        case kIdSep1:
        case kIdSep2:
        case kIdSep3:
            addPropString(array, "type", "separator");
            break;
        case kIdEnable:
            addPropString(array, "label", "Enabled");
            addPropString(array, "toggle-type", "checkmark");
            addPropInt32(array, "toggle-state", m_enabled ? 1 : 0);
            addPropBool(array, "enabled", TRUE);
            addPropBool(array, "visible", TRUE);
            break;
        case kIdConfig:
            addPropString(array, "label", "Configuration...");
            addPropBool(array, "enabled", TRUE);
            break;
        case kIdAbout:
            addPropString(array, "label", "About...");
            addPropBool(array, "enabled", TRUE);
            break;
        case kIdQuit:
            addPropString(array, "label", "Quit");
            addPropBool(array, "enabled", TRUE);
            break;
        default:
            break;
    }
}

void LinuxTray::appendLayout(DBusMessageIter *it, int id) {
    DBusMessageIter item, props, children;
    dbus_message_iter_open_container(it, DBUS_TYPE_STRUCT, nullptr, &item);
    dbus_int32_t itemId = id;
    dbus_message_iter_append_basic(&item, DBUS_TYPE_INT32, &itemId);

    dbus_message_iter_open_container(&item, DBUS_TYPE_ARRAY, "{sv}", &props);
    appendItemProps(&props, id);
    dbus_message_iter_close_container(&item, &props);

    dbus_message_iter_open_container(&item, DBUS_TYPE_ARRAY, "v", &children);
    if (id == kIdRoot) {
        static const int order[] = {kIdOpen,  kIdSep1,  kIdEnable, kIdSep2,
                                    kIdConfig, kIdAbout, kIdSep3,  kIdQuit};
        for (int child : order) {
            DBusMessageIter childVariant;
            dbus_message_iter_open_container(&children, DBUS_TYPE_VARIANT,
                                             "(ia{sv}av)", &childVariant);
            appendLayout(&childVariant, child);
            dbus_message_iter_close_container(&children, &childVariant);
        }
    }
    dbus_message_iter_close_container(&item, &children);

    dbus_message_iter_close_container(it, &item);
}

void LinuxTray::appendGroupEntry(DBusMessageIter *array, int id) {
    DBusMessageIter entry, props;
    dbus_message_iter_open_container(array, DBUS_TYPE_STRUCT, nullptr, &entry);
    dbus_int32_t itemId = id;
    dbus_message_iter_append_basic(&entry, DBUS_TYPE_INT32, &itemId);
    dbus_message_iter_open_container(&entry, DBUS_TYPE_ARRAY, "{sv}", &props);
    appendItemProps(&props, id);
    dbus_message_iter_close_container(&entry, &props);
    dbus_message_iter_close_container(array, &entry);
}

// ---- signals ---------------------------------------------------------------

void LinuxTray::emitPropertiesChanged() {
    if (!m_conn) return;
    DBusMessage *signal = dbus_message_new_signal(kSniPath, kPropsIface,
                                                  "PropertiesChanged");
    if (!signal) return;

    DBusMessageIter it, dict, invalidated;
    dbus_message_iter_init_append(signal, &it);
    appendString(&it, kSniIface);

    dbus_message_iter_open_container(&it, DBUS_TYPE_ARRAY, "{sv}", &dict);
    {
        DBusMessageIter entry;
        dbus_message_iter_open_container(&dict, DBUS_TYPE_DICT_ENTRY, nullptr,
                                         &entry);
        appendString(&entry, "IconName");
        variantString(&entry, m_enabled ? "white-sound" : "white-sound-off");
        dbus_message_iter_close_container(&dict, &entry);
    }
    {
        DBusMessageIter entry;
        dbus_message_iter_open_container(&dict, DBUS_TYPE_DICT_ENTRY, nullptr,
                                         &entry);
        appendString(&entry, "IconPixmap");
        variantPixmap(&entry, m_enabled ? kTrayIconOnArgb : kTrayIconOffArgb,
                      kTrayIconWidth, kTrayIconHeight);
        dbus_message_iter_close_container(&dict, &entry);
    }
    dbus_message_iter_close_container(&it, &dict);

    dbus_message_iter_open_container(&it, DBUS_TYPE_ARRAY, "s", &invalidated);
    dbus_message_iter_close_container(&it, &invalidated);

    dbus_connection_send(m_conn, signal, nullptr);
    dbus_message_unref(signal);
}

void LinuxTray::emitLayoutUpdated() {
    if (!m_conn) return;
    DBusMessage *signal =
        dbus_message_new_signal(kMenuPath, kMenuIface, "LayoutUpdated");
    if (!signal) return;

    DBusMessageIter it;
    dbus_message_iter_init_append(signal, &it);
    dbus_uint32_t revision = m_revision;
    dbus_int32_t parent = kIdRoot;
    dbus_message_iter_append_basic(&it, DBUS_TYPE_UINT32, &revision);
    dbus_message_iter_append_basic(&it, DBUS_TYPE_INT32, &parent);
    dbus_connection_send(m_conn, signal, nullptr);
    dbus_message_unref(signal);
}

// ---- replies ---------------------------------------------------------------

void LinuxTray::replyEmpty(DBusConnection *conn, DBusMessage *msg) {
    DBusMessage *reply = dbus_message_new_method_return(msg);
    if (!reply) return;
    dbus_connection_send(conn, reply, nullptr);
    dbus_message_unref(reply);
}

void LinuxTray::replyIntrospect(DBusConnection *conn, DBusMessage *msg) {
    DBusMessage *reply = dbus_message_new_method_return(msg);
    if (!reply) return;
    DBusMessageIter it;
    dbus_message_iter_init_append(reply, &it);
    appendString(&it, kIntrospectionXml);
    dbus_connection_send(conn, reply, nullptr);
    dbus_message_unref(reply);
}

void LinuxTray::dispatchCommand(int id) {
    if (!m_cb.onCommand) return;
    switch (id) {
        case kIdOpen:   m_cb.onCommand(TrayCommand::OpenPanel); break;
        case kIdEnable: m_cb.onCommand(TrayCommand::ToggleEnabled); break;
        case kIdConfig: m_cb.onCommand(TrayCommand::ShowConfig); break;
        case kIdAbout:  m_cb.onCommand(TrayCommand::ShowAbout); break;
        case kIdQuit:   m_cb.onCommand(TrayCommand::Quit); break;
        default: return;
    }
    // No local state change here. ToggleEnabled has the shell flip the
    // controller, which calls back into setEnabled() (exactly like the Windows
    // and macOS backends); toggling m_enabled again would invert the checkmark
    // and the icon.
}

// ---- StatusNotifierItem messages ------------------------------------------

DBusHandlerResult LinuxTray::onSniMessage(DBusConnection *conn,
                                          DBusMessage *msg) {
    if (dbus_message_is_method_call(msg, kIntrospectIface, "Introspect")) {
        replyIntrospect(conn, msg);
        return DBUS_HANDLER_RESULT_HANDLED;
    }

    if (dbus_message_is_method_call(msg, kPropsIface, "Get")) {
        const char *iface = nullptr;
        const char *prop = nullptr;
        if (!dbus_message_get_args(msg, nullptr, DBUS_TYPE_STRING, &iface,
                                   DBUS_TYPE_STRING, &prop, DBUS_TYPE_INVALID)) {
            return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
        }
        DBusMessage *reply = dbus_message_new_method_return(msg);
        if (!reply) return DBUS_HANDLER_RESULT_NEED_MEMORY;
        DBusMessageIter it;
        dbus_message_iter_init_append(reply, &it);
        if (!appendSniProperty(&it, prop)) {
            dbus_message_unref(reply);
            reply = dbus_message_new_error(msg,
                                           "org.freedesktop.DBus.Error.InvalidArgs",
                                           "Unknown property");
        }
        if (reply) {
            dbus_connection_send(conn, reply, nullptr);
            dbus_message_unref(reply);
        }
        return DBUS_HANDLER_RESULT_HANDLED;
    }

    if (dbus_message_is_method_call(msg, kPropsIface, "GetAll")) {
        DBusMessage *reply = dbus_message_new_method_return(msg);
        if (!reply) return DBUS_HANDLER_RESULT_NEED_MEMORY;
        DBusMessageIter it, array;
        dbus_message_iter_init_append(reply, &it);
        dbus_message_iter_open_container(&it, DBUS_TYPE_ARRAY, "{sv}", &array);
        static const char *names[] = {"Category", "Id",          "Title",
                                      "Status",   "IconName",    "IconPixmap",
                                      "Menu",     "ItemIsMenu"};
        for (const char *name : names) {
            DBusMessageIter entry;
            dbus_message_iter_open_container(&array, DBUS_TYPE_DICT_ENTRY,
                                             nullptr, &entry);
            appendString(&entry, name);
            appendSniProperty(&entry, name);
            dbus_message_iter_close_container(&array, &entry);
        }
        dbus_message_iter_close_container(&it, &array);
        dbus_connection_send(conn, reply, nullptr);
        dbus_message_unref(reply);
        return DBUS_HANDLER_RESULT_HANDLED;
    }

    if (dbus_message_is_method_call(msg, kSniIface, "Activate")) {
        if (m_cb.onActivate) m_cb.onActivate();
        replyEmpty(conn, msg);
        return DBUS_HANDLER_RESULT_HANDLED;
    }
    if (dbus_message_is_method_call(msg, kSniIface, "SecondaryActivate") ||
        dbus_message_is_method_call(msg, kSniIface, "ContextMenu")) {
        showMenu();
        replyEmpty(conn, msg);
        return DBUS_HANDLER_RESULT_HANDLED;
    }
    if (dbus_message_is_method_call(msg, kSniIface, "Scroll")) {
        replyEmpty(conn, msg);
        return DBUS_HANDLER_RESULT_HANDLED;
    }

    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

// ---- dbusmenu messages -----------------------------------------------------

DBusHandlerResult LinuxTray::onMenuMessage(DBusConnection *conn,
                                           DBusMessage *msg) {
    if (dbus_message_is_method_call(msg, kIntrospectIface, "Introspect")) {
        replyIntrospect(conn, msg);
        return DBUS_HANDLER_RESULT_HANDLED;
    }

    if (dbus_message_is_method_call(msg, kPropsIface, "Get")) {
        const char *iface = nullptr;
        const char *prop = nullptr;
        if (!dbus_message_get_args(msg, nullptr, DBUS_TYPE_STRING, &iface,
                                   DBUS_TYPE_STRING, &prop, DBUS_TYPE_INVALID)) {
            return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
        }
        DBusMessage *reply = dbus_message_new_method_return(msg);
        if (!reply) return DBUS_HANDLER_RESULT_NEED_MEMORY;
        DBusMessageIter it;
        dbus_message_iter_init_append(reply, &it);
        if (!appendMenuProperty(&it, prop)) {
            dbus_message_unref(reply);
            reply = dbus_message_new_error(msg,
                                           "org.freedesktop.DBus.Error.InvalidArgs",
                                           "Unknown property");
        }
        if (reply) {
            dbus_connection_send(conn, reply, nullptr);
            dbus_message_unref(reply);
        }
        return DBUS_HANDLER_RESULT_HANDLED;
    }

    if (dbus_message_is_method_call(msg, kPropsIface, "GetAll")) {
        DBusMessage *reply = dbus_message_new_method_return(msg);
        if (!reply) return DBUS_HANDLER_RESULT_NEED_MEMORY;
        DBusMessageIter it, array;
        dbus_message_iter_init_append(reply, &it);
        dbus_message_iter_open_container(&it, DBUS_TYPE_ARRAY, "{sv}", &array);
        static const char *names[] = {"Version", "TextDirection", "Status"};
        for (const char *name : names) {
            DBusMessageIter entry;
            dbus_message_iter_open_container(&array, DBUS_TYPE_DICT_ENTRY,
                                             nullptr, &entry);
            appendString(&entry, name);
            appendMenuProperty(&entry, name);
            dbus_message_iter_close_container(&array, &entry);
        }
        dbus_message_iter_close_container(&it, &array);
        dbus_connection_send(conn, reply, nullptr);
        dbus_message_unref(reply);
        return DBUS_HANDLER_RESULT_HANDLED;
    }

    if (dbus_message_is_method_call(msg, kMenuIface, "GetLayout")) {
        DBusMessageIter it;
        dbus_int32_t parentId = kIdRoot;
        if (!dbus_message_iter_init(msg, &it) || !readInt32(&it, parentId)) {
            return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
        }
        DBusMessage *reply = dbus_message_new_method_return(msg);
        if (!reply) return DBUS_HANDLER_RESULT_NEED_MEMORY;
        DBusMessageIter out;
        dbus_message_iter_init_append(reply, &out);
        dbus_uint32_t revision = m_revision;
        dbus_message_iter_append_basic(&out, DBUS_TYPE_UINT32, &revision);
        appendLayout(&out, parentId == kIdRoot ? kIdRoot : parentId);
        dbus_connection_send(conn, reply, nullptr);
        dbus_message_unref(reply);
        return DBUS_HANDLER_RESULT_HANDLED;
    }

    if (dbus_message_is_method_call(msg, kMenuIface, "GetGroupProperties")) {
        DBusMessageIter it, ids;
        if (!dbus_message_iter_init(msg, &it)) {
            return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
        }
        DBusMessage *reply = dbus_message_new_method_return(msg);
        if (!reply) return DBUS_HANDLER_RESULT_NEED_MEMORY;
        DBusMessageIter out, array;
        dbus_message_iter_init_append(reply, &out);
        dbus_message_iter_open_container(&out, DBUS_TYPE_ARRAY, "(ia{sv})",
                                         &array);
        if (dbus_message_iter_get_arg_type(&it) == DBUS_TYPE_ARRAY) {
            dbus_message_iter_recurse(&it, &ids);
            while (dbus_message_iter_get_arg_type(&ids) == DBUS_TYPE_INT32) {
                dbus_int32_t id = 0;
                dbus_message_iter_get_basic(&ids, &id);
                appendGroupEntry(&array, id);
                dbus_message_iter_next(&ids);
            }
        }
        dbus_message_iter_close_container(&out, &array);
        dbus_connection_send(conn, reply, nullptr);
        dbus_message_unref(reply);
        return DBUS_HANDLER_RESULT_HANDLED;
    }

    if (dbus_message_is_method_call(msg, kMenuIface, "GetProperty")) {
        DBusMessageIter it;
        dbus_int32_t id = 0;
        const char *name = nullptr;
        if (!dbus_message_iter_init(msg, &it) || !readInt32(&it, id) ||
            !dbus_message_iter_next(&it) ||
            dbus_message_iter_get_arg_type(&it) != DBUS_TYPE_STRING) {
            return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
        }
        dbus_message_iter_get_basic(&it, &name);

        DBusMessage *reply = dbus_message_new_method_return(msg);
        if (!reply) return DBUS_HANDLER_RESULT_NEED_MEMORY;
        DBusMessageIter out;
        dbus_message_iter_init_append(reply, &out);
        bool ok = true;
        if (std::strcmp(name, "label") == 0) {
            const char *label = "";
            switch (id) {
                case kIdOpen: label = "Open panel"; break;
                case kIdEnable: label = "Enabled"; break;
                case kIdConfig: label = "Configuration..."; break;
                case kIdAbout: label = "About..."; break;
                case kIdQuit: label = "Quit"; break;
                default: break;
            }
            variantString(&out, label);
        } else if (std::strcmp(name, "type") == 0) {
            const bool separator = (id == kIdSep1 || id == kIdSep2 || id == kIdSep3);
            variantString(&out, separator ? "separator" : "standard");
        } else if (std::strcmp(name, "enabled") == 0 ||
                   std::strcmp(name, "visible") == 0) {
            variantBool(&out, TRUE);
        } else if (std::strcmp(name, "toggle-type") == 0) {
            variantString(&out, "checkmark");
        } else if (std::strcmp(name, "toggle-state") == 0) {
            variantInt32(&out, m_enabled ? 1 : 0);
        } else if (std::strcmp(name, "children-display") == 0) {
            variantString(&out, "submenu");
        } else {
            ok = false;
        }
        if (!ok) {
            dbus_message_unref(reply);
            reply = dbus_message_new_error(msg,
                                           "org.freedesktop.DBus.Error.InvalidArgs",
                                           "Unknown property");
        }
        if (reply) {
            dbus_connection_send(conn, reply, nullptr);
            dbus_message_unref(reply);
        }
        return DBUS_HANDLER_RESULT_HANDLED;
    }

    if (dbus_message_is_method_call(msg, kMenuIface, "Event")) {
        DBusMessageIter it;
        dbus_int32_t id = 0;
        const char *eventId = nullptr;
        if (!dbus_message_iter_init(msg, &it) || !readInt32(&it, id) ||
            !dbus_message_iter_next(&it) ||
            dbus_message_iter_get_arg_type(&it) != DBUS_TYPE_STRING) {
            return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
        }
        dbus_message_iter_get_basic(&it, &eventId);
        if (eventId && (std::strcmp(eventId, "clicked") == 0 ||
                        std::strcmp(eventId, "activated") == 0)) {
            dispatchCommand(id);
        }
        replyEmpty(conn, msg);
        return DBUS_HANDLER_RESULT_HANDLED;
    }

    if (dbus_message_is_method_call(msg, kMenuIface, "EventGroup")) {
        DBusMessageIter it, events;
        if (dbus_message_iter_init(msg, &it) &&
            dbus_message_iter_get_arg_type(&it) == DBUS_TYPE_ARRAY) {
            dbus_message_iter_recurse(&it, &events);
            while (dbus_message_iter_get_arg_type(&events) == DBUS_TYPE_STRUCT) {
                DBusMessageIter entry;
                dbus_message_iter_recurse(&events, &entry);
                dbus_int32_t id = 0;
                if (dbus_message_iter_get_arg_type(&entry) == DBUS_TYPE_INT32) {
                    dbus_message_iter_get_basic(&entry, &id);
                    if (dbus_message_iter_next(&entry) &&
                        dbus_message_iter_get_arg_type(&entry) ==
                            DBUS_TYPE_STRING) {
                        const char *eventId = nullptr;
                        dbus_message_iter_get_basic(&entry, &eventId);
                        if (eventId &&
                            (std::strcmp(eventId, "clicked") == 0 ||
                             std::strcmp(eventId, "activated") == 0)) {
                            dispatchCommand(id);
                        }
                    }
                }
                dbus_message_iter_next(&events);
            }
        }
        DBusMessage *reply = dbus_message_new_method_return(msg);
        if (!reply) return DBUS_HANDLER_RESULT_NEED_MEMORY;
        DBusMessageIter out, array;
        dbus_message_iter_init_append(reply, &out);
        dbus_message_iter_open_container(&out, DBUS_TYPE_ARRAY, "i", &array);
        dbus_message_iter_close_container(&out, &array);
        dbus_connection_send(conn, reply, nullptr);
        dbus_message_unref(reply);
        return DBUS_HANDLER_RESULT_HANDLED;
    }

    if (dbus_message_is_method_call(msg, kMenuIface, "AboutToShow")) {
        DBusMessage *reply = dbus_message_new_method_return(msg);
        if (!reply) return DBUS_HANDLER_RESULT_NEED_MEMORY;
        DBusMessageIter out;
        dbus_message_iter_init_append(reply, &out);
        appendBool(&out, FALSE);
        dbus_connection_send(conn, reply, nullptr);
        dbus_message_unref(reply);
        return DBUS_HANDLER_RESULT_HANDLED;
    }

    if (dbus_message_is_method_call(msg, kMenuIface, "AboutToShowGroup")) {
        DBusMessage *reply = dbus_message_new_method_return(msg);
        if (!reply) return DBUS_HANDLER_RESULT_NEED_MEMORY;
        DBusMessageIter out, a1, a2;
        dbus_message_iter_init_append(reply, &out);
        dbus_message_iter_open_container(&out, DBUS_TYPE_ARRAY, "i", &a1);
        dbus_message_iter_close_container(&out, &a1);
        dbus_message_iter_open_container(&out, DBUS_TYPE_ARRAY, "i", &a2);
        dbus_message_iter_close_container(&out, &a2);
        dbus_connection_send(conn, reply, nullptr);
        dbus_message_unref(reply);
        return DBUS_HANDLER_RESULT_HANDLED;
    }

    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

std::unique_ptr<Tray> createTray() { return std::make_unique<LinuxTray>(); }

} // namespace platform
} // namespace ws
