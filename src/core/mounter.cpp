#include "core/mounter.h"
#include "core/fsutil.h"
#include "core/gmain_thread.h"
#include "core/xdg.h"

#include <atomic>
#include <sys/stat.h>
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

    // Reports the end of a mount on the UI thread (real or fake).
    void complete(const std::string &local, const std::string &landing, const std::string &message) {
        MountCallbacks c = cb;
        finish();
        auto weak = self;
        const bool success = !local.empty();
        poster.post([weak, c, success, local, landing, message] {
            if (weak.expired())
                return;
            if (success && c.on_mounted)
                c.on_mounted(local, landing);
            else if (!success && c.on_failed)
                c.on_failed(message);
        });
    }

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

std::string landing_for(const std::string &local) {
    // Land in the matching home folder when the other machine has one, as
    // it usually does on a tailnet of one person's machines.
    const std::string remote_home = local + xdg::home();
    return is_dir(remote_home) ? remote_home : local;
}

// Test hook (FLO_TEST_FAKE_GVFS=FOLDER, inside a private test run only):
// stands in for GVfs's SFTP backend on the real GMountOperation. It asks
// GVfs's unknown-host question (the trusted-host handler answers it for
// tailnet machines), then the password, re-asking on a wrong one up to
// three times as ssh does; the right password ($FLO_TEST_FAKE_GVFS_PASSWORD,
// default "flo-test") "mounts" FOLDER/sftp:host=HOST. Every answer goes
// through g_mount_operation_reply(), exactly as with GVfs.
struct FakeMount {
    std::shared_ptr<Mounter::Impl> impl;
    std::string host, folder, password;
    int step = 0, tries = 0;
    gulong handler = 0;

    static std::string host_of(const std::string &uri) {
        auto start = uri.find("://");
        std::string rest = start == std::string::npos ? uri : uri.substr(start + 3);
        rest = rest.substr(0, rest.find('/'));
        if (auto at = rest.rfind('@'); at != std::string::npos)
            rest = rest.substr(at + 1);
        return rest.substr(0, rest.find(':'));
    }

    void ask_question() {
        const std::string msg = "The identity of the remote computer (" + host +
                                ") is unknown.\nThis happens when you log in to a computer the first time.\n\n"
                                "The identity sent by the remote computer is ???. If you want to be absolutely "
                                "sure it is safe to continue, contact the system administrator.";
        const char *choices[] = {"Log In Anyway", "Cancel Login", nullptr};
        g_signal_emit_by_name(impl->op, "ask-question", msg.c_str(), choices);
    }

    void ask_password() {
        const std::string msg = "Enter password for ssh as " + std::string(g_get_user_name());
        g_signal_emit_by_name(impl->op, "ask-password", msg.c_str(), g_get_user_name(), "",
                              GAskPasswordFlags(G_ASK_PASSWORD_NEED_PASSWORD | G_ASK_PASSWORD_SAVING_SUPPORTED));
    }

    void end(const std::string &local, const std::string &message) {
        auto keep = impl;
        if (keep->op && handler)
            g_signal_handler_disconnect(keep->op, handler);
        delete this;
        keep->complete(local, local.empty() ? std::string() : landing_for(local), message);
    }

    static void replied(GMountOperation *op, GMountOperationResult result, gpointer data) {
        auto *f = static_cast<FakeMount *>(data);
        // Answer outside the emission that asked, as GVfs does over D-Bus.
        int choice = g_mount_operation_get_choice(op);
        const char *pw = g_mount_operation_get_password(op);
        std::string given = pw ? pw : "";
        GMainThread::instance().invoke([f, result, choice, given] {
            if (result != G_MOUNT_OPERATION_HANDLED) {
                f->end({}, {}); // cancelled
                return;
            }
            if (f->step == 0) {
                if (choice != 0) {
                    f->end({}, {});
                    return;
                }
                f->step = 1;
                f->ask_password();
                return;
            }
            if (given != f->password) {
                if (++f->tries >= 3) {
                    f->end({}, "Permission denied");
                    return;
                }
                f->ask_password();
                return;
            }
            std::string local = f->folder + "/sftp:host=" + f->host;
            ::mkdir(local.c_str(), 0700);
            f->end(is_dir(local) ? local : std::string(), is_dir(local) ? std::string() : "No such fake mount");
        });
    }
};

void fake_mount(const std::shared_ptr<Mounter::Impl> &impl, const std::string &uri, const std::string &folder) {
    auto *f = new FakeMount;
    f->impl = impl;
    f->host = FakeMount::host_of(uri);
    f->folder = folder;
    f->password = xdg::test_hook("FLO_TEST_FAKE_GVFS_PASSWORD");
    if (f->password.empty())
        f->password = "flo-test";
    f->handler = g_signal_connect(impl->op, "reply", G_CALLBACK(FakeMount::replied), f);
    f->ask_question();
}
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
        const std::string fake = xdg::test_hook("FLO_TEST_FAKE_GVFS");
        if (!fake.empty()) {
            fake_mount(impl, uri, fake);
            return;
        }
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
                    if (local.empty())
                        message = "GVfs mounted it but gave no local folder for it";
                    else
                        landing = landing_for(local);
                } else {
                    const bool cancelled = error && g_error_matches(error, G_IO_ERROR, G_IO_ERROR_FAILED_HANDLED);
                    message = cancelled ? std::string() : std::string(error ? error->message : "mount failed");
                }
                if (error)
                    g_error_free(error);
                impl->complete(local, landing, message);
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
