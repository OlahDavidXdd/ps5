#pragma once

// Synthetic ISO 9660 fixture: no game data or external image tools required.
void TestIsoArchive(const std::filesystem::path &directory) {
  std::vector<unsigned char> image(24 * 2048);
  const auto number = [&](size_t at, uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) {
      image[at + i] = (value >> (8 * i)) & 255;
      image[at + 7 - i] = (value >> (8 * i)) & 255;
    }
  };
  const auto record = [&](size_t at, uint32_t sector, uint32_t size, bool dir,
                          std::string_view name) {
    const auto length = 33 + name.size() + (name.size() % 2 == 0);
    image[at] = static_cast<unsigned char>(length);
    number(at + 2, sector);
    number(at + 10, size);
    image[at + 25] = dir ? 2 : 0;
    image[at + 28] = image[at + 31] = 1;
    image[at + 32] = static_cast<unsigned char>(name.size());
    std::memcpy(image.data() + at + 33, name.data(), name.size());
    return length;
  };
  const size_t pvd = 16 * 2048;
  image[pvd] = 1;
  std::memcpy(image.data() + pvd + 1, "CD001", 5);
  image[pvd + 6] = 1;
  image[pvd + 129] = image[pvd + 130] = 8;
  record(pvd + 156, 20, 2048, true, std::string_view("\0", 1));
  size_t at = 20 * 2048;
  at += record(at, 20, 2048, true, std::string_view("\0", 1));
  at += record(at, 20, 2048, true, std::string_view("\1", 1));
  const auto file_record = at;
  at += record(at, 22, 7, false, "EBOOT.BIN;1");
  record(at, 21, 2048, true, "SCE_SYS");
  record(21 * 2048, 23, 2, false, "PARAM.JSON;1");
  std::memcpy(image.data() + 22 * 2048, "fixture", 7);
  std::memcpy(image.data() + 23 * 2048, "{}", 2);
  const auto write = [&](const char *name) {
    const auto path = directory / name;
    std::ofstream stream(path, std::ios::binary);
    stream.write(reinterpret_cast<const char *>(image.data()), image.size());
    stream.close();
    Check(!stream.fail(), "write ISO fixture");
    return path;
  };
  const auto path = write("game.ISO");
  auto reader = Common::OpenArchive(path);
  Check(reader != nullptr, "open ISO");
  const auto member = Common::MakeArchivePath(path, "eboot.bin");
  Common::File file(member, Common::File::Mode::Read);
  Check(!file.IsInvalid() && file.Size() == 7,
        "ISO version and case normalization");
  char data[16]{};
  uint32_t count = 0;
  file.Read(data, sizeof(data), &count);
  Check(count == 7 && std::string_view(data, count) == "fixture",
        "ISO read and EOF");
  Check(file.Seek(2), "ISO seek");
  file.Read(data, 3, &count);
  Check(count == 3 && std::string_view(data, count) == "xtu",
        "ISO random read");
  Check(Common::GetArchiveInfo(
            Common::MakeArchivePath(path, "sce_sys/param.json"))
                ->size == 2,
        "ISO nested directory");
  Check(Common::GetArchiveDirEntries(Common::MakeArchivePath(path)).size() == 2,
        "ISO directory listing");
  image[file_record + 25] = 0x80;
  Check(!Common::OpenArchive(write("multi.iso")), "reject multi-extent ISO");
  image[file_record + 25] = 0;
  number(file_record + 2, 1000);
  Check(!Common::OpenArchive(write("outside.iso")),
        "reject out-of-image extent");
  image[pvd + 1] = 'X';
  Check(!Common::OpenArchive(write("invalid.iso")),
        "reject invalid descriptor");
}
