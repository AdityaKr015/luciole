#include "platform/file_payload.h"

#include <print>
#include <stdexcept>

namespace {
  void require(bool value, const char* message) {
    if (!value)
      throw std::runtime_error(message);
  }
  void testRoundTrip() {
    using namespace files;
    const std::string raw = std::string("/tmp/clipboard/space #?%\n") + static_cast<char>(0xff) + ".txt";
    const FilePayload input{{Location::fromPath(raw), Location::fromPath("/tmp/clipboard/日本語.txt")}, true};
    for (const auto mime : {kUriListMime, kGnomeFilesMime}) {
      const auto encoded = encodeFilePayload(input, mime);
      require(encoded.has_value(), "raw source URI encoding failed");
      const auto parsed = parseFilePayload(mime, *encoded);
      require(parsed && parsed->locations == input.locations, "URI payload changed operation names");
      require(parsed->cut == (mime == kGnomeFilesMime), "clipboard action changed");
      require(!encoded->contains(static_cast<char>(0xff)), "raw filename bytes escaped the URI transport");
    }
    require(*encodeFilePayload(input, kKdeCutMime) == "1", "KDE cut flag missing");
    auto copy = input;
    copy.cut = false;
    require(*encodeFilePayload(copy, kKdeCutMime) == "0", "KDE copy flag missing");
    const auto commented = parseFilePayload(kUriListMime, "# selection\r\n\r\nfile:///tmp/a%20b\r\n");
    require(commented && commented->locations.front().nativePath() == "/tmp/a b", "URI-list comments or CRLF failed");
  }
  void testRejection() {
    using namespace files;
    for (const auto* bytes :
         {"", "# no files\n", "/tmp/plain-path", "file:///tmp/a b", "file:///tmp/a\rgarbage", "file:///tmp/a%00b",
          "file:///tmp/a#fragment", "file:///tmp/a?query", "file:///tmp/a\nfile:///tmp/a",
          "file:///tmp/good\nnot-a-uri"})
      require(!parseFilePayload(kUriListMime, bytes), "malformed file payload was partially accepted");
    require(!parseFilePayload(kGnomeFilesMime, "move\nfile:///tmp/a"), "unknown clipboard action accepted");
    require(!parseFilePayload(kGnomeFilesMime, "cut\n"), "empty cut selection accepted");
    require(!parseFilePayload(kGnomeFilesMime, "copy\nfile:///tmp/a\n\nfile:///tmp/b"), "empty GNOME URI accepted");
    require(!parseFilePayload(kUriListMime, std::string("file:///tmp/a\0b", 15)), "embedded NUL accepted");
    require(!parseFilePayload(kUriListMime, std::string(kFilePayloadLimit + 1, '#')), "oversized offer accepted");
    require(!encodeFilePayload({}, kUriListMime), "empty outgoing selection accepted");
    std::string many;
    for (std::size_t i = 0; i <= kFilePayloadCount; ++i)
      many += "file:///tmp/item" + std::to_string(i) + "\n";
    require(!parseFilePayload(kUriListMime, many), "unbounded URI count accepted");
  }
  void testPartialMoves() {
    using namespace files;
    FilePayload selection{{}, true};
    for (int i = 0; i < 7; ++i)
      selection.locations.push_back(Location::fromPath("/tmp/source/" + std::to_string(i)));
    const std::vector<TransferResult> results{
        {selection.locations[0], TransferOutcome::MOVED, {}},
        {selection.locations[1], TransferOutcome::SKIPPED, {}},
        {selection.locations[2], TransferOutcome::FAILED, "fixture failure"},
        {selection.locations[3], TransferOutcome::PARTIAL, "source removal incomplete"},
        {selection.locations[4], TransferOutcome::CANCELLED, {}},
        {selection.locations[5], TransferOutcome::COPIED, {}},
    };
    const auto remaining = remainingCutFiles(selection, results);
    require(remaining.cut && remaining.locations.size() == 6, "cut completion discarded incomplete sources");
    require(
        remaining.locations.front() == selection.locations[1] && remaining.locations.back() == selection.locations[6],
        "cut completion changed remaining order or unattempted sources"
    );
    selection.cut = false;
    require(remainingCutFiles(selection, results).locations == selection.locations, "copy consumed clipboard sources");
  }
} // namespace
int main() {
  try {
    testRoundTrip();
    testRejection();
    testPartialMoves();
    std::println("file payload tests passed");
    return 0;
  } catch (const std::exception& error) {
    std::println(stderr, "{}", error.what());
    return 1;
  }
}
