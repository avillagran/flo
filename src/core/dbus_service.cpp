#include "core/dbus_service.h"
#include "core/gmain_thread.h"
#include "core/uri.h"

#include <gio/gio.h>

namespace flo {

static const char *kFm1Xml =
    "<node><interface name='org.freedesktop.FileManager1'>"
    "<method name='ShowFolders'><arg type='as' name='URIs' direction='in'/>"
    "<arg type='s' name='StartupId' direction='in'/></method>"
    "<method name='ShowItems'><arg type='as' name='URIs' direction='in'/>"
    "<arg type='s' name='StartupId' direction='in'/></method>"
    "<method name='ShowItemProperties'><arg type='as' name='URIs' direction='in'/>"
    "<arg type='s' name='StartupId' direction='in'/></method>"
    "</interface></node>";

static const char *kFloXml =
    "<node><interface name='io.github.avillagran.Flo'>"
    "<method name='Open'><arg type='s' name='Folder' direction='in'/>"
    "<arg type='s' name='Select' direction='in'/></method>"
    "</interface></node>";

std::vector<std::string> local_paths_from_uris(const std::vector<std::string> &uris) {
    std::vector<std::string> out;
    for (const auto &u : uris) {
        std::string p = uri_to_path(u);
        if (p.empty() && !u.empty() && u[0] == '/')
            p = u;
        if (!p.empty())
            out.push_back(p);
    }
    return out;
}

struct DBusService::Impl {
    EventQueue::Poster poster;
    DBusHandlers handlers;
    std::weak_ptr<Impl> self;
    guint flo_owner = 0, fm1_owner = 0;
    GDBusNodeInfo *fm1_info = nullptr, *flo_info = nullptr;
    std::function<void(bool)> on_ready;
    bool own_fm1 = true;

    template <typename F> void post(F f) {
        auto weak = self;
        poster.post([weak, f] {
            if (!weak.expired())
                f();
        });
    }

    static void method_call(GDBusConnection *, const char *, const char *, const char *interface, const char *method,
                            GVariant *params, GDBusMethodInvocation *inv, gpointer data) {
        auto *self = static_cast<Impl *>(data);
        if (g_strcmp0(interface, "io.github.avillagran.Flo") == 0) {
            const char *folder = nullptr, *select = nullptr;
            g_variant_get(params, "(&s&s)", &folder, &select);
            const std::string f = folder ? folder : "", s = select ? select : "";
            auto h = self->handlers.open_window;
            if (h)
                self->post([h, f, s] { h(f, s); });
            g_dbus_method_invocation_return_value(inv, nullptr);
            return;
        }
        GVariantIter *iter = nullptr;
        const char *startup = nullptr;
        g_variant_get(params, "(as&s)", &iter, &startup);
        std::vector<std::string> uris;
        const char *u;
        while (g_variant_iter_next(iter, "&s", &u))
            uris.emplace_back(u);
        g_variant_iter_free(iter);
        const auto paths = local_paths_from_uris(uris);
        std::function<void(const std::vector<std::string> &)> h;
        if (g_strcmp0(method, "ShowFolders") == 0)
            h = self->handlers.show_folders;
        else if (g_strcmp0(method, "ShowItems") == 0)
            h = self->handlers.show_items;
        else
            h = self->handlers.show_item_properties ? self->handlers.show_item_properties : self->handlers.show_items;
        if (h && !paths.empty())
            self->post([h, paths] { h(paths); });
        g_dbus_method_invocation_return_value(inv, nullptr);
    }

    static const GDBusInterfaceVTable *vtable() {
        static const GDBusInterfaceVTable v = {method_call, nullptr, nullptr, {nullptr}};
        return &v;
    }
};

DBusService::DBusService(EventQueue &queue, DBusHandlers handlers) : impl_(std::make_shared<Impl>()) {
    impl_->poster = queue.poster();
    impl_->handlers = std::move(handlers);
    impl_->self = impl_;
}

DBusService::~DBusService() {
    auto impl = impl_;
    GMainThread::instance().invoke_sync([impl] {
        if (impl->flo_owner)
            g_bus_unown_name(impl->flo_owner);
        if (impl->fm1_owner)
            g_bus_unown_name(impl->fm1_owner);
        impl->flo_owner = impl->fm1_owner = 0;
        if (impl->fm1_info)
            g_dbus_node_info_unref(impl->fm1_info);
        if (impl->flo_info)
            g_dbus_node_info_unref(impl->flo_info);
        impl->fm1_info = impl->flo_info = nullptr;
    });
}

void DBusService::start(bool own_filemanager1, std::function<void(bool)> on_ready) {
    auto impl = impl_;
    GMainThread::instance().invoke([impl, own_filemanager1, on_ready] {
        impl->on_ready = on_ready;
        impl->own_fm1 = own_filemanager1;
        impl->fm1_info = g_dbus_node_info_new_for_xml(kFm1Xml, nullptr);
        impl->flo_info = g_dbus_node_info_new_for_xml(kFloXml, nullptr);
        impl->flo_owner = g_bus_own_name(
            G_BUS_TYPE_SESSION, kFloBusName, G_BUS_NAME_OWNER_FLAGS_NONE,
            [](GDBusConnection *conn, const char *, gpointer data) {
                auto *self = static_cast<Impl *>(data);
                g_dbus_connection_register_object(conn, kFloObjectPath, self->flo_info->interfaces[0], Impl::vtable(),
                                                  self, nullptr, nullptr);
                // FileManager1 lives on the same connection; another file
                // manager may already own the name, which is fine.
                if (self->own_fm1) {
                    g_dbus_connection_register_object(conn, "/org/freedesktop/FileManager1",
                                                      self->fm1_info->interfaces[0], Impl::vtable(), self, nullptr,
                                                      nullptr);
                    self->fm1_owner = g_bus_own_name_on_connection(conn, kFileManager1Name,
                                                                   G_BUS_NAME_OWNER_FLAGS_NONE, nullptr, nullptr,
                                                                   nullptr, nullptr);
                }
            },
            [](GDBusConnection *, const char *, gpointer data) {
                auto *self = static_cast<Impl *>(data);
                auto cb = self->on_ready;
                if (cb)
                    self->post([cb] { cb(true); });
            },
            [](GDBusConnection *, const char *, gpointer data) {
                auto *self = static_cast<Impl *>(data);
                auto cb = self->on_ready;
                self->on_ready = nullptr;
                if (cb)
                    self->post([cb] { cb(false); });
            },
            impl.get(), nullptr);
    });
}

bool forward_open_to_running_flo(const std::string &folder, const std::string &select, int timeout_ms) {
    GError *error = nullptr;
    GDBusConnection *conn = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &error);
    if (!conn) {
        if (error)
            g_error_free(error);
        return false;
    }
    GVariant *r = g_dbus_connection_call_sync(conn, kFloBusName, kFloObjectPath, kFloBusName, "Open",
                                              g_variant_new("(ss)", folder.c_str(), select.c_str()), nullptr,
                                              G_DBUS_CALL_FLAGS_NO_AUTO_START, timeout_ms, nullptr, &error);
    const bool ok = r != nullptr;
    if (r)
        g_variant_unref(r);
    if (error)
        g_error_free(error);
    g_object_unref(conn);
    return ok;
}

} // namespace flo
