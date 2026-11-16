// The FileChooser portal backend's options and results (core/portal_service.h),
// as xdg-desktop-portal sends and expects them.
#include "harness.h"

#include "core/portal_service.h"

#include <gio/gio.h>

using namespace flo;

TEST(portal_open_options_parse_every_documented_key) {
    GVariantBuilder b;
    g_variant_builder_init(&b, G_VARIANT_TYPE_VARDICT);
    g_variant_builder_add(&b, "{sv}", "accept_label", g_variant_new_string("_Attach"));
    g_variant_builder_add(&b, "{sv}", "multiple", g_variant_new_boolean(TRUE));
    g_variant_builder_add(&b, "{sv}", "directory", g_variant_new_boolean(FALSE));
    g_variant_builder_add(&b, "{sv}", "modal", g_variant_new_boolean(FALSE));
    GVariant *filters = g_variant_new_parsed("[('Images', [(uint32 0, '*.png'), (1, 'image/jpeg')]), ('Text', [(0, '*.txt')])]");
    g_variant_builder_add(&b, "{sv}", "filters", filters);
    g_variant_builder_add(&b, "{sv}", "current_filter", g_variant_new_parsed("('Text', [(uint32 0, '*.txt')])"));
    g_variant_builder_add(&b, "{sv}", "choices",
                          g_variant_new_parsed("[('enc', 'Encoding', [('utf8', 'UTF-8')], 'utf8'), ('ro', 'Read only', @a(ss) [], 'false')]"));
    g_variant_builder_add(&b, "{sv}", "current_folder", g_variant_new_bytestring("/srv/pictures"));
    g_variant_builder_add(&b, "{sv}", "unknown", g_variant_new_int32(5));
    GVariant *opts = g_variant_ref_sink(g_variant_builder_end(&b));
    PickerRequest r;
    parse_picker_options(opts, r);
    g_variant_unref(opts);
    CHECK_EQ(r.accept_label, std::string("_Attach"));
    CHECK(r.multiple && !r.directory && !r.modal);
    CHECK_EQ(r.filters.size(), size_t(2));
    CHECK_EQ(r.filters[0].name, std::string("Images"));
    CHECK_EQ(r.filters[0].rules.size(), size_t(2));
    CHECK_EQ(r.current_filter, 1);
    CHECK_EQ(r.choices.size(), size_t(2));
    CHECK_EQ(r.choices[0].value, std::string("utf8"));
    CHECK_EQ(r.current_folder, std::string("/srv/pictures"));
    CHECK(filter_matches(r.filters[0], "A.PNG", "image/png"));
    CHECK(filter_matches(r.filters[0], "photo", "image/jpeg"));
    CHECK(!filter_matches(r.filters[0], "notes.txt", "text/plain"));
    PickerFilter any{"All images", {{1, "image/*"}}};
    CHECK(filter_matches(any, "x", "image/webp"));
    CHECK(!filter_matches(any, "x", "imagex/webp"));
}

TEST(portal_save_options_and_results) {
    GVariantBuilder b;
    g_variant_builder_init(&b, G_VARIANT_TYPE_VARDICT);
    g_variant_builder_add(&b, "{sv}", "current_file", g_variant_new_bytestring("/home/me/Documents/report.odt"));
    g_variant_builder_add(&b, "{sv}", "current_filter", g_variant_new_parsed("('ODF', [(uint32 0, '*.odt')])"));
    GVariant *opts = g_variant_ref_sink(g_variant_builder_end(&b));
    PickerRequest r;
    r.kind = PickerRequest::Kind::Save;
    parse_picker_options(opts, r);
    g_variant_unref(opts);
    CHECK_EQ(r.current_folder, std::string("/home/me/Documents"));
    CHECK_EQ(r.current_name, std::string("report.odt"));
    // The current filter was not among the filters: it is added.
    CHECK_EQ(r.filters.size(), size_t(1));
    CHECK_EQ(r.current_filter, 0);

    PickerResult res;
    res.response = 0;
    res.uris = {"file:///home/me/Documents/report.odt"};
    res.filter = 0;
    GVariant *v = g_variant_ref_sink(picker_results(r, res));
    GVariant *uris = g_variant_lookup_value(v, "uris", G_VARIANT_TYPE_STRING_ARRAY);
    CHECK(uris != nullptr);
    if (uris) {
        gsize n = 0;
        const gchar **u = g_variant_get_strv(uris, &n);
        CHECK_EQ(n, gsize(1));
        CHECK_EQ(std::string(u[0]), res.uris[0]);
        g_free(u);
        g_variant_unref(uris);
    }
    GVariant *cf = g_variant_lookup_value(v, "current_filter", G_VARIANT_TYPE("(sa(us))"));
    CHECK(cf != nullptr);
    if (cf)
        g_variant_unref(cf);
    CHECK(g_variant_lookup_value(v, "choices", nullptr) == nullptr);
    g_variant_unref(v);
    // A cancel carries no results.
    res.response = 1;
    v = g_variant_ref_sink(picker_results(r, res));
    CHECK_EQ(g_variant_n_children(v), gsize(0));
    g_variant_unref(v);
}

TEST(portal_save_files_names) {
    GVariantBuilder b;
    g_variant_builder_init(&b, G_VARIANT_TYPE_VARDICT);
    const gchar *names[] = {"a.png", "b.png", nullptr};
    g_variant_builder_add(&b, "{sv}", "files", g_variant_new_bytestring_array(names, -1));
    GVariant *opts = g_variant_ref_sink(g_variant_builder_end(&b));
    PickerRequest r;
    r.kind = PickerRequest::Kind::SaveFiles;
    parse_picker_options(opts, r);
    g_variant_unref(opts);
    CHECK(r.files == (std::vector<std::string>{"a.png", "b.png"}));
}

TEST(portal_choice_follows_the_request) {
    PickerRequest r;
    std::vector<std::string> uris;
    std::string nav;
    // Open a file: a marked folder opens; files answer (one unless multiple).
    CHECK_EQ(picker_choice(r, "/d", {"/d/sub"}, {true}, "", uris, nav), std::string());
    CHECK_EQ(nav, std::string("/d/sub"));
    CHECK(uris.empty());
    CHECK_EQ(picker_choice(r, "/d", {}, {}, "", uris, nav), std::string("Select a file"));
    CHECK_EQ(picker_choice(r, "/d", {"/d/a b.txt", "/d/c.txt"}, {false, false}, "", uris, nav), std::string());
    CHECK(uris == (std::vector<std::string>{"file:///d/a%20b.txt"}));
    r.multiple = true;
    picker_choice(r, "/d", {"/d/a.txt", "/d/sub", "/d/c.txt"}, {false, true, false}, "", uris, nav);
    CHECK(uris == (std::vector<std::string>{"file:///d/a.txt", "file:///d/c.txt"}));
    // Folders: the marked ones, or the folder shown.
    r.directory = true;
    picker_choice(r, "/d", {}, {}, "", uris, nav);
    CHECK(uris == (std::vector<std::string>{"file:///d"}));
    CHECK_EQ(picker_choice(r, "/d", {"/d/a.txt"}, {false}, "", uris, nav), std::string("Select a folder"));
    // Save: the name in the folder shown.
    PickerRequest s;
    s.kind = PickerRequest::Kind::Save;
    CHECK_EQ(picker_choice(s, "/d", {}, {}, "", uris, nav), std::string("Type a name"));
    CHECK(!picker_choice(s, "/d", {}, {}, "../x", uris, nav).empty());
    CHECK_EQ(picker_choice(s, "/", {}, {}, "out.txt", uris, nav), std::string());
    CHECK(uris == (std::vector<std::string>{"file:///out.txt"}));
    // Save several: each name in the marked folder, or the folder shown.
    PickerRequest m;
    m.kind = PickerRequest::Kind::SaveFiles;
    m.files = {"a.png", "b.png"};
    picker_choice(m, "/d", {"/d/sub"}, {true}, "", uris, nav);
    CHECK(uris == (std::vector<std::string>{"file:///d/sub/a.png", "file:///d/sub/b.png"}));
}
