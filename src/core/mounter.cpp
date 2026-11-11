#include "core/mounter.h"
#include "core/fsutil.h"
#include "core/gmain_thread.h"
#include "core/xdg.h"

#include <atomic>
#include <mutex>

#include <gio/gio.h>

namespace flo {

struct Mounter::Impl {
    EventQueue::Poster poster;
    std::weak_ptr<Impl> self;
    std::atomic<bool> running{false};
    // GLib thread only:
    GMountOperation *op = nullptr;
    bool trusted = false;
    uint64_t request_id = 0;
    uint64_t next_request = 1;
    MountCallbacks cb;

    void post_request(const MountRequest &r) {
        auto f = cb.on_request;
        auto weak = self;
        if (f)
            poster.post([weak, f, r] {
                if (!weak.expired())
                    f(r);
            });
    }

    // GMountOperation's default handlers would answer "unhandled" right
    // away, which GVfs takes as a cancelled login: stop the emission and
    // keep the operation waiting for the reply.
    static void question(GMountOperation *op, const char *message, const char **choices, gpointer data) {
        auto *self = static_cast<Impl *>(data);
        g_signal_stop_emission_by_name(op, "ask-question");
        if (self->trusted) {
            // GVfs's SFTP questions (new host, key differs from the key for
            // its IP) list "Log In Anyway" first. A changed key for a known
            // machine never gets here: ssh refuses it outright.
            g_mount_operation_set_choice(op, 0);
            g_mount_operation_reply(op, G_MOUNT_OPERATION_HANDLED);
            return;
        }
        MountRequest r;
        r.id = self->request_id = self->next_request++;
        r.kind = MountRequest::Question;
        r.message = message ? message : "";
        for (const char **c = choices; c && *c; ++c)
            r.choices.emplace_back(*c);
        self->post_request(r);
    }

    static void password(GMountOperation *op, const char *message, const char *user, const char *domain,
                         GAskPasswordFlags flags, gpointer data) {
        auto *self = static_cast<Impl *>(data);
        g_signal_stop_emission_by_name(op, "ask-password");
        MountRequest r;
        r.id = self->request_id = self->next_request++;
        r.kind = MountRequest::Password;
        r.message = message ? message : "";
        r.user = user ? user : "";
        r.domain = domain ? domain : "";
        r.needs_user = flags & G_ASK_PASSWORD_NEED_USERNAME;
        r.needs_domain = flags & G_ASK_PASSWORD_NEED_DOMAIN;
        self->post_request(r);
    }

    static void aborted(GMountOperation *, gpointer data) { static_cast<Impl *>(data)->request_id = 0; }

    void finish() {
        if (op) {
            g_signal_handlers_disconnect_by_data(op, this);
            g_object_unref(op);
            op = nullptr;
        }
        request_id = 0;
        running = false;
    }
};

Mounter::Mounter(EventQueue &queue) : impl_(std::make_shared<Impl>()) {
    impl_->poster = queue.poster();
    impl_->self = impl_;
}

Mounter::~Mounter() {
    auto impl = impl_;
    // A prompt nobody will answer would keep GVfs waiting.
    GMainThread::instance().invoke_sync([impl] {
        if (impl->op && impl->request_id)
            g_mount_operation_reply(impl->op, G_MOUNT_OPERATION_ABORTED);
        impl->cb = MountCallbacks{};
    });
}

bool Mounter::busy() const { return impl_->running.load(); }

namespace {
struct MountData {
    std::shared_ptr<Mounter::Impl> impl;
};
} // namespace

bool Mounter::mount(const std::string &uri, bool trusted_host, MountCallbacks callbacks) {
    bool expected = false;
    if (!impl_->running.compare_exchange_strong(expected, true))
        return false;
    auto impl = impl_;
    GMainThread::instance().invoke([impl, uri, trusted_host, callbacks] {
        impl->cb = callbacks;
        impl->trusted = trusted_host;
        impl->op = g_mount_operation_new();
        g_signal_connect(impl->op, "ask-question", G_CALLBACK(Impl::question), impl.get());
        g_signal_connect(impl->op, "ask-password", G_CALLBACK(Impl::password), impl.get());
        g_signal_connect(impl->op, "aborted", G_CALLBACK(Impl::aborted), impl.get());
        GFile *file = g_file_new_for_uri(uri.c_str());
        g_file_mount_enclosing_volume(
            file, G_MOUNT_MOUNT_NONE, impl->op, nullptr,
            [](GObject *src, GAsyncResult *res, gpointer data) {
                auto *md = static_cast<MountData *>(data);
                auto impl = md->impl;
                delete md;
                GError *error = nullptr;
                const bool ok = g_file_mount_enclosing_volume_finish(G_FILE(src), res, &error);
                // Mounted earlier (by Flo or another app) is as good as now.
                const bool already = error && g_error_matches(error, G_IO_ERROR, G_IO_ERROR_ALREADY_MOUNTED);
                std::string local, landing, message;
                if (ok || already) {
                    if (char *p = g_file_get_path(G_FILE(src))) {
                        local = p;
                        g_free(p);
                    }
                    if (local.empty()) {
                        message = "GVfs mounted it but gave no local folder for it";
                    } else {
                        // Land in the matching home folder when the other
                        // machine has one, as it usually does on a tailnet of
                        // one person's machines.
                        const std::string remote_home = local + xdg::home();
                        landing = is_dir(remote_home) ? remote_home : local;
                    }
                } else {
                    const bool cancelled = error && g_error_matches(error, G_IO_ERROR, G_IO_ERROR_FAILED_HANDLED);
                    message = cancelled ? std::string() : std::string(error ? error->message : "mount failed");
                }
                if (error)
                    g_error_free(error);
                MountCallbacks cb = impl->cb;
                impl->finish();
                auto weak = impl->self;
                const bool success = !local.empty();
                impl->poster.post([weak, cb, success, local, landing, message] {
                    if (weak.expired())
                        return;
                    if (success && cb.on_mounted)
                        cb.on_mounted(local, landing);
                    else if (!success && cb.on_failed)
                        cb.on_failed(message);
                });
            },
            new MountData{impl});
        g_object_unref(file);
    });
    return true;
}

void Mounter::reply_password(uint64_t request, const std::string &user, const std::string &password,
                             const std::string &domain) {
    auto impl = impl_;
    GMainThread::instance().invoke([impl, request, user, password, domain] {
        if (!impl->op || impl->request_id != request)
            return;
        impl->request_id = 0;
        if (!user.empty())
            g_mount_operation_set_username(impl->op, user.c_str());
        if (!domain.empty())
            g_mount_operation_set_domain(impl->op, domain.c_str());
        g_mount_operation_set_password(impl->op, password.c_str());
        // Kept by GVfs for this login session only, never saved to disk.
        g_mount_operation_set_password_save(impl->op, G_PASSWORD_SAVE_FOR_SESSION);
        g_mount_operation_reply(impl->op, G_MOUNT_OPERATION_HANDLED);
    });
}

void Mounter::reply_choice(uint64_t request, int choice) {
    auto impl = impl_;
    GMainThread::instance().invoke([impl, request, choice] {
        if (!impl->op || impl->request_id != request)
            return;
        impl->request_id = 0;
        g_mount_operation_set_choice(impl->op, choice);
        g_mount_operation_reply(impl->op, G_MOUNT_OPERATION_HANDLED);
    });
}

void Mounter::abort(uint64_t request) {
    auto impl = impl_;
    GMainThread::instance().invoke([impl, request] {
        if (!impl->op || impl->request_id != request)
            return;
        impl->request_id = 0;
        g_mount_operation_reply(impl->op, G_MOUNT_OPERATION_ABORTED);
    });
}

} // namespace flo
