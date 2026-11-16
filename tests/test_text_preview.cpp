// Text previews (core/text_preview.h): a capped prefix of regular text
// files, binary refused, cached in the run's private XDG_CACHE_HOME.
#include "harness.h"

#include "core/fsutil.h"
#include "core/text_preview.h"

#include <sys/stat.h>
#include <unistd.h>

using namespace flo;
using namespace flo::text_preview;
using namespace flo_test;

TEST(text_preview_reads_text_and_refuses_binary) {
    std::string d = private_dir("text-preview");
    std::string out;
    std::string code = write_file(d + "/main.c", "int main(void) {\n\treturn 0;\r\n}\n");
    CHECK(read_prefix(code, out));
    CHECK_EQ(out, std::string("int main(void) {\n\treturn 0;\r\n}\n"));
    auto lines = page_lines(out);
    CHECK(lines == (std::vector<std::string>{"int main(void) {", "    return 0;", "}"}));
    CHECK(!read_prefix(write_file(d + "/blob.bin", std::string("ELF\0\1\2", 6)), out));
    CHECK(out.empty());
    CHECK(!read_prefix(write_file(d + "/latin1.txt", "caf\xE9 au lait"), out));
    CHECK(read_prefix(write_file(d + "/utf8.txt", "caf\xC3\xA9"), out));
    // A prefix that cuts a character in two is still text.
    std::string big(kPrefix - 1, 'a');
    big += "\xC3\xA9 and more";
    CHECK(read_prefix(write_file(d + "/big.txt", big), out));
    CHECK_EQ(out.size(), kPrefix);
    // Not regular files: a folder, a FIFO (never waited on).
    CHECK(!read_prefix(d, out));
    CHECK(::mkfifo((d + "/pipe").c_str(), 0600) == 0);
    CHECK(!read_prefix(d + "/pipe", out));
    // Long lines and many lines are capped.
    auto capped = page_lines(std::string(300, 'x') + "\n" + std::string(100, '\n'), 5, 10);
    CHECK_EQ(capped.size(), size_t(5));
    CHECK_EQ(capped[0], std::string(10, 'x'));
}

TEST(text_preview_cache_follows_the_file) {
    std::string d = private_dir("text-preview-cache");
    Cache cache(d + "/cache");
    std::string file = write_file(d + "/notes.txt", "one\ntwo\n");
    std::string text;
    CHECK(fetch(cache, file, 100, 8, text));
    CHECK_EQ(text, std::string("one\ntwo\n"));
    struct stat st;
    CHECK(::stat((d + "/cache").c_str(), &st) == 0 && (st.st_mode & 0777) == 0700);
    CHECK(::stat(cache.path_for(file).c_str(), &st) == 0 && (st.st_mode & 0777) == 0600);
    // Served from the cache while mtime and size match, read again otherwise.
    CHECK(cache.load(file, 100, 8, text));
    CHECK(!cache.load(file, 101, 8, text));
    CHECK(!cache.load(file, 100, 9, text));
    CHECK_EQ(Cache::default_root().find("/flo/text-previews") != std::string::npos, true);
}
