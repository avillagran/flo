#include "core/portal_service.h"

#include "core/gmain_thread.h"
#include "core/name_filter.h"
#include "core/uri.h"

#include <gio/gio.h>

#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>

namespace flo {

// --- Filters and options ----------------------------------------------------------

bool filter_matches(const PickerFilter &f, const std::string &name, const std::string &mime) {
    for (const auto &r : f.rules) {
        if (r.first == 0 && glob_match(r.second, name))
            return true;
        if (r.first == 1 && !mime.empty()) {
            const std::string &m = r.second;
            if (m == mime || m == "*/*" || m == "*")
                return true;
            if (m.size() > 2 && m.compare(m.size() - 2, 2, "/*") == 0 &&
                mime.compare(0, m.size() - 1, m.substr(0, m.size() - 1)) == 0)
                return true;
        }
    }
    return false;
}

namespace {

bool read_filter(GVariant *v, PickerFilter &out) {
    if (!v || !g_variant_is_of_type(v, G_VARIANT_TYPE("(sa(us))")))
        return false;
    const char *name = nullptr;
    GVariantIter *it = nullptr;
    g_variant_get(v, "(&sa(us))", &name, &it);
    out.name = name ? name : "";
    out.rules.clear();
    guint32 kind = 0;
    const char *pattern = nullptr;
    while (g_variant_iter_next(it, "(u&s)", &kind, &pattern))
        if (pattern && (kind == 0 || kind == 1))
            out.rules.emplace_back(kind, pattern);
    g_variant_iter_free(it);
    return true;
}

// A byte string as the portal sends paths: NUL-terminated "ay".
std::string read_bytes(GVariant *v) {
    if (!v || !g_variant_is_of_type(v, G_VARIANT_TYPE_BYTESTRING))
        return {};
    const char *s = g_variant_get_bytestring(v);
    return s ? std::string(s) : std::string();
}

GVariant *filter_variant(const PickerFilter &f) {
    GVariantBuilder rules;
    g_variant_builder_init(&rules, G_VARIANT_TYPE("a(us)"));
    for (const auto &r : f.rules)
        g_variant_builder_add(&rules, "(us)", r.first, r.second.c_str());
    return g_variant_new("(sa(us))", f.name.c_str(), &rules);
}

} // namespace

void parse_picker_options(GVariant *options, PickerRequest &out) {
    if (!options || !g_variant_is_of_type(options, G_VARIANT_TYPE_VARDICT))
        return;
    GVariantIter it;
    g_variant_iter_init(&it, options);
    const char *key = nullptr;
    GVariant *value = nullptr;
    PickerFilter current;
    bool has_current = false;
    while (g_variant_iter_next(&it, "{&sv}", &key, &value)) {
        const std::string k = key;
        if (k == "accept_label" && g_variant_is_of_type(value, G_VARIANT_TYPE_STRING)) {
            out.accept_label = g_variant_get_string(value, nullptr);
        } else if (k == "multiple" && g_variant_is_of_type(value, G_VARIANT_TYPE_BOOLEAN)) {
            out.multiple = g_variant_get_boolean(value);
        } else if (k == "directory" && g_variant_is_of_type(value, G_VARIANT_TYPE_BOOLEAN)) {
            out.directory = g_variant_get_boolean(value);
        } else if (k == "modal" && g_variant_is_of_type(value, G_VARIANT_TYPE_BOOLEAN)) {
            out.modal = g_variant_get_boolean(value);
        } else if (k == "filters" && g_variant_is_of_type(value, G_VARIANT_TYPE("a(sa(us))"))) {
            GVariantIter fi;
            g_variant_iter_init(&fi, value);
            GVariant *f = nullptr;
            while ((f = g_variant_iter_next_value(&fi))) {
                PickerFilter pf;
                if (read_filter(f, pf))
                    out.filters.push_back(pf);
                g_variant_unref(f);
            }
        } else if (k == "current_filter") {
            has_current = read_filter(value, current);
        } else if (k == "choices" && g_variant_is_of_type(value, G_VARIANT_TYPE("a(ssa(ss)s)"))) {
            GVariantIter ci;
            g_variant_iter_init(&ci, value);
            const char *id = nullptr, *label = nullptr, *def = nullptr;
            GVariantIter *opts = nullptr;
            while (g_variant_iter_next(&ci, "(&s&sa(ss)&s)", &id, &label, &opts, &def)) {
                out.choices.push_back({id ? id : "", label ? label : "", def ? def : ""});
                g_variant_iter_free(opts);
            }
        } else if (k == "current_folder") {
            out.current_folder = read_bytes(value);
        } else if (k == "current_name" && g_variant_is_of_type(value, G_VARIANT_TYPE_STRING)) {
            out.current_name = g_variant_get_string(value, nullptr);
        } else if (k == "current_file") {
            out.current_file = read_bytes(value);
        } else if (k == "files" && g_variant_is_of_type(value, G_VARIANT_TYPE_BYTESTRING_ARRAY)) {
            gsize n = 0;
            const gchar **names = g_variant_get_bytestring_array(value, &n);
            for (gsize i = 0; i < n; ++i)
                if (names[i] && *names[i])
                    out.files.emplace_back(names[i]);
            g_free(names);
        }
        g_variant_unref(value);
    }
    if (has_current) {
        for (size_t i = 0; i < out.filters.size(); ++i)
            if (out.filters[i] == current)
                out.current_filter = int(i);
        if (out.current_filter < 0) {
            out.filters.push_back(current);
            out.current_filter = int(out.filters.size()) - 1;
        }
    }
    // A file being saved again names its folder and name.
    if (out.kind == PickerRequest::Kind::Save && !out.current_file.empty() && out.current_file[0] == '/') {
        size_t slash = out.current_file.rfind('/');
        if (out.current_folder.empty())
            out.current_folder = slash == 0 ? "/" : out.current_file.substr(0, slash);
        if (out.current_name.empty())
            out.current_name = out.current_file.substr(slash + 1);
    }
}

GVariant *picker_results(const PickerRequest &req, const PickerResult &res) {
    GVariantBuilder b;
    g_variant_builder_init(&b, G_VARIANT_TYPE_VARDICT);
    if (res.response == 0) {
        GVariantBuilder uris;
        g_variant_builder_init(&uris, G_VARIANT_TYPE_STRING_ARRAY);
        for (const auto &u : res.uris)
            g_variant_builder_add(&uris, "s", u.c_str());
        g_variant_builder_add(&b, "{sv}", "uris", g_variant_builder_end(&uris));
        if (res.filter >= 0 && res.filter < int(req.filters.size()))
            g_variant_builder_add(&b, "{sv}", "current_filter", filter_variant(req.filters[size_t(res.filter)]));
        if (!req.choices.empty()) {
            GVariantBuilder ch;
            g_variant_builder_init(&ch, G_VARIANT_TYPE("a(ss)"));
            for (const auto &c : req.choices)
                g_variant_builder_add(&ch, "(ss)", c.id.c_str(), c.value.c_str());
            g_variant_builder_add(&b, "{sv}", "choices", g_variant_builder_end(&ch));
        }
    }
    return g_variant_builder_end(&b);
}

// --- Choosing ---------------------------------------------------------------------

std::string picker_choice(const PickerRequest &req, const std::string &folder, const std::vector<std::string> &marked,
                          const std::vector<bool> &marked_dirs, const std::string &name, std::vector<std::string> &uris,
                          std::string &navigate) {
    uris.clear();
    navigate.clear();
    auto join = [](const std::string &dir, const std::string &leaf) {
        return dir.size() > 1 && dir.back() != '/' ? dir + "/" + leaf : dir == "/" ? "/" + leaf : dir + leaf;
    };
    std::vector<std::string> dirs, files;
    for (size_t i = 0; i < marked.size(); ++i)
        (i < marked_dirs.size() && marked_dirs[i] ? dirs : files).push_back(marked[i]);
    switch (req.kind) {
    case PickerRequest::Kind::Open:
        if (req.directory) {
            if (dirs.empty() && !files.empty())
                return "Select a folder";
            if (dirs.empty())
                dirs.push_back(folder);
            for (const auto &d : dirs) {
                uris.push_back(path_to_uri(d));
                if (!req.multiple)
                    break;
            }
            return {};
        }
        if (files.empty()) {
            if (dirs.size() == 1) {
                navigate = dirs[0];
                return {};
            }
            return "Select a file";
        }
        for (const auto &f : files) {
            uris.push_back(path_to_uri(f));
            if (!req.multiple)
                break;
        }
        return {};
    case PickerRequest::Kind::Save: {
        if (name.empty())
            return "Type a name";
        if (name.find('/') != std::string::npos || name == "." || name == "..")
            return "A name can't contain \"/\"";
        uris.push_back(path_to_uri(join(folder, name)));
        return {};
    }
    case PickerRequest::Kind::SaveFiles: {
        std::string into = dirs.size() == 1 && files.empty() ? dirs[0] : folder;
        for (const auto &f : req.files) {
            if (f.find('/') != std::string::npos)
                continue;
            uris.push_back(path_to_uri(join(into, f)));
        }
        return uris.empty() ? std::string("Nothing to save") : std::string();
    }
    }
    return {};
}

// --- The service ---------------------------------------------------------------------

static const char *kPortalXml =
    "<node>"
    "<interface name='org.freedesktop.impl.portal.FileChooser'>"
    "<method name='OpenFile'><arg type='o' name='handle' direction='in'/>"
    "<arg type='s' name='app_id' direction='in'/><arg type='s' name='parent_window' direction='in'/>"
    "<arg type='s' name='title' direction='in'/><arg type='a{sv}' name='options' direction='in'/>"
    "<arg type='u' name='response' direction='out'/><arg type='a{sv}' name='results' direction='out'/></method>"
    "<method name='SaveFile'><arg type='o' name='handle' direction='in'/>"
    "<arg type='s' name='app_id' direction='in'/><arg type='s' name='parent_window' direction='in'/>"
    "<arg type='s' name='title' direction='in'/><arg type='a{sv}' name='options' direction='in'/>"
    "<arg type='u' name='response' direction='out'/><arg type='a{sv}' name='results' direction='out'/></method>"
    "<method name='SaveFiles'><arg type='o' name='handle' direction='in'/>"
    "<arg type='s' name='app_id' direction='in'/><arg type='s' name='parent_window' direction='in'/>"
    "<arg type='s' name='title' direction='in'/><arg type='a{sv}' name='options' direction='in'/>"
    "<arg type='u' name='response' direction='out'/><arg type='a{sv}' name='results' direction='out'/></method>"
    "<property name='version' type='u' access='read'/>"
    "</interface>"
    "<interface name='org.freedesktop.impl.portal.Request'><method name='Close'/></interface>"
    "</node>";

struct PortalService::Impl {
    EventQueue::Poster poster;
    std::function<void(PickerRequest, Reply)> on_request;
    std::function<void(const std::string &)> on_close;
    std::function<void(bool)> on_ready;
    std::weak_ptr<Impl> self;
    GDBusNodeInfo *info = nullptr;
    guint owner = 0;
    GDBusConnection *conn = nullptr; // GLib thread only
    // Calls waiting for an answer, by handle, with their Request object.
    struct Pending {
        GDBusMethodInvocation *inv = nullptr;
        guint request_id = 0;
        PickerRequest req;
    };
    std::map<std::string, Pending> pending; // GLib thread only
    mutable std::mutex count_mu;
    int count = 0;

    template <typename F> void post(F f) {
        auto weak = self;
        poster.post([weak, f] {
            if (!weak.expired())
                f();
        });
    }

    void set_count() {
        std::lock_guard<std::mutex> l(count_mu);
        count = int(pending.size());
    }

    // GLib thread: sends the answer for `handle` (once).
    void finish(const std::string &handle, const PickerResult &res) {
        auto it = pending.find(handle);
        if (it == pending.end())
            return;
        Pending p = it->second;
        pending.erase(it);
        set_count();
        if (p.request_id && conn)
            g_dbus_connection_unregister_object(conn, p.request_id);
        g_dbus_method_invocation_return_value(p.inv, g_variant_new("(u@a{sv})", res.response,
                                                                   picker_results(p.req, res)));
    }

    static void request_call(GDBusConnection *, const char *, const char *path, const char *, const char *method,
                             GVariant *, GDBusMethodInvocation *inv, gpointer data) {
        auto *self = static_cast<Impl *>(data);
        const std::string handle = path;
        if (g_strcmp0(method, "Close") == 0) {
            g_dbus_method_invocation_return_value(inv, nullptr);
            PickerResult res;
            res.response = 2;
            self->finish(handle, res);
            auto cb = self->on_close;
            if (cb)
                self->post([cb, handle] { cb(handle); });
            return;
        }
        g_dbus_method_invocation_return_dbus_error(inv, "org.freedesktop.DBus.Error.UnknownMethod", "No such method");
    }

    static void chooser_call(GDBusConnection *conn, const char *, const char *, const char *, const char *method,
                             GVariant *params, GDBusMethodInvocation *inv, gpointer data) {
        auto *self = static_cast<Impl *>(data);
        PickerRequest req;
        req.kind = g_strcmp0(method, "SaveFile") == 0    ? PickerRequest::Kind::Save
                   : g_strcmp0(method, "SaveFiles") == 0 ? PickerRequest::Kind::SaveFiles
                                                         : PickerRequest::Kind::Open;
        const char *handle = nullptr, *app = nullptr, *parent = nullptr, *title = nullptr;
        GVariant *options = nullptr;
        g_variant_get(params, "(&o&s&s&s@a{sv})", &handle, &app, &parent, &title, &options);
        req.handle = handle ? handle : "";
        req.app_id = app ? app : "";
        req.parent_window = parent ? parent : "";
        req.title = title ? title : "";
        parse_picker_options(options, req);
        g_variant_unref(options);
        static const bool trace = std::getenv("FLO_TRACE_PORTAL") != nullptr;
        if (trace)
            std::fprintf(stderr, "flo portal: %s %s from '%s'\n", method, req.handle.c_str(), req.app_id.c_str());
        if (self->pending.count(req.handle)) {
            g_dbus_method_invocation_return_dbus_error(inv, "org.freedesktop.DBus.Error.InvalidArgs",
                                                       "This request is already open");
            return;
        }
        Pending p;
        p.inv = inv;
        p.req = req;
        static const GDBusInterfaceVTable rv = {request_call, nullptr, nullptr, {nullptr}};
        p.request_id = g_dbus_connection_register_object(conn, req.handle.c_str(), self->info->interfaces[1], &rv,
                                                         self, nullptr, nullptr);
        self->pending[req.handle] = p;
        self->set_count();
        auto cb = self->on_request;
        std::weak_ptr<Impl> weak = self->self;
        const std::string h = req.handle;
        Reply reply = [weak, h](const PickerResult &res) {
            GMainThread::instance().invoke([weak, h, res] {
                if (auto s = weak.lock())
                    s->finish(h, res);
            });
        };
        if (cb)
            self->post([cb, req, reply] { cb(req, reply); });
        else
            reply(PickerResult{});
    }

    static GVariant *get_property(GDBusConnection *, const char *, const char *, const char *, const char *name,
                                  GError **, gpointer) {
        if (g_strcmp0(name, "version") == 0)
            return g_variant_new_uint32(4);
        return nullptr;
    }
};

PortalService::PortalService(EventQueue &queue, std::function<void(PickerRequest, Reply)> on_request,
                             std::function<void(const std::string &)> on_close)
    : impl_(std::make_shared<Impl>()) {
    impl_->poster = queue.poster();
    impl_->on_request = std::move(on_request);
    impl_->on_close = std::move(on_close);
    impl_->self = impl_;
}

PortalService::~PortalService() {
    auto impl = impl_;
    GMainThread::instance().invoke_sync([impl] {
        // Whatever is still open ends as "other" rather than hanging.
        std::vector<std::string> handles;
        for (const auto &kv : impl->pending)
            handles.push_back(kv.first);
        for (const auto &h : handles)
            impl->finish(h, PickerResult{});
        if (impl->owner)
            g_bus_unown_name(impl->owner);
        impl->owner = 0;
        if (impl->info)
            g_dbus_node_info_unref(impl->info);
        impl->info = nullptr;
        if (impl->conn)
            g_object_unref(impl->conn);
        impl->conn = nullptr;
    });
}

int PortalService::pending() const {
    std::lock_guard<std::mutex> l(impl_->count_mu);
    return impl_->count;
}

void PortalService::start(std::function<void(bool)> on_ready) {
    auto impl = impl_;
    GMainThread::instance().invoke([impl, on_ready] {
        impl->on_ready = on_ready;
        impl->info = g_dbus_node_info_new_for_xml(kPortalXml, nullptr);
        impl->owner = g_bus_own_name(
            G_BUS_TYPE_SESSION, kPortalImplBusName, G_BUS_NAME_OWNER_FLAGS_NONE,
            [](GDBusConnection *conn, const char *, gpointer data) {
                auto *self = static_cast<Impl *>(data);
                self->conn = G_DBUS_CONNECTION(g_object_ref(conn));
                static const GDBusInterfaceVTable v = {Impl::chooser_call, Impl::get_property, nullptr, {nullptr}};
                g_dbus_connection_register_object(conn, kPortalObjectPath, self->info->interfaces[0], &v, self,
                                                  nullptr, nullptr);
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

} // namespace flo
