#include "common/archive.h"

#include "common/stringUtils.h"

#include <algorithm>
#include <condition_variable>
#include <cstring>
#include <array>
#include <fstream>
#include <mutex>
#include <optional>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <zarchive/zarchivereader.h>

// POSIX hosts and MinGW's pthread-backed libstdc++ have cancellable condition waits.
// MSVC/ClangCL and MinGW's native Win32 threading backend do not use pthread waits.
#if !defined(_WIN32) || defined(_GLIBCXX_GCC_GTHR_POSIX_H)
#include <pthread.h>
#define KYTY_ARCHIVE_PTHREAD_CANCELLATION
#endif

namespace Common {

// Backends expose immutable metadata and offset-based reads. Cursor management, path routing,
// reader lifetime and the host-stack worker are shared by every archive format.
class ArchiveReader {
public:
	struct Entry {
		uint64_t id;
		uint64_t size;
		bool     is_file;
	};

	virtual ~ArchiveReader()                                                       = default;
	virtual std::optional<Entry>        Find(std::string_view member)              = 0;
	virtual std::vector<File::DirEntry> List(std::string_view member)              = 0;
	virtual uint64_t Read(uint64_t id, uint64_t offset, uint32_t size, void* data) = 0;
};

namespace {

class ZArchiveReaderBackend final: public ArchiveReader {
public:
	explicit ZArchiveReaderBackend(std::unique_ptr<ZArchiveReader> reader)
	    : m_reader(std::move(reader)) {}

	std::optional<Entry> Find(std::string_view member) override {
		const auto node = m_reader->LookUp(member);
		if (node == ZARCHIVE_INVALID_NODE) {
			return {};
		}
		return Entry {node, m_reader->GetFileSize(node), m_reader->IsFile(node)};
	}

	std::vector<File::DirEntry> List(std::string_view member) override {
		const auto node = m_reader->LookUp(member);
		if (!m_reader->IsDirectory(node)) {
			return {};
		}
		const auto count = m_reader->GetDirEntryCount(node);
		if (count > (1u << 20u)) {
			return {};
		}
		std::vector<File::DirEntry> result;
		result.reserve(count);
		for (uint32_t index = 0; index < count; ++index) {
			ZArchiveReader::DirEntry entry {};
			if (m_reader->GetDirEntry(node, index, entry)) {
				result.push_back({std::string(entry.name), entry.isFile});
			}
		}
		return result;
	}

	uint64_t Read(uint64_t id, uint64_t offset, uint32_t size, void* data) override {
		// ZArchiveReader already serializes access to its stream and decompression cache.
		return m_reader->ReadFromFile(static_cast<ZArchiveNodeHandle>(id), offset, size, data);
	}

private:
	std::unique_ptr<ZArchiveReader> m_reader;
};

std::shared_ptr<ArchiveReader> OpenZArchive(const std::filesystem::path& path) {
	auto reader = std::unique_ptr<ZArchiveReader>(ZArchiveReader::OpenFromFile(path));
	return reader ? std::make_shared<ZArchiveReaderBackend>(std::move(reader)) : nullptr;
}

// ISO 9660 single-extent files are streamed in place. UDF, encrypted disc images,
// multi-extent files and interleaved files are deliberately rejected.
class IsoArchiveReader final: public ArchiveReader {
  public:
	explicit IsoArchiveReader(const std::filesystem::path &path)
		: m_stream(path, std::ios::binary) {}

	bool Initialize() {
		if (!m_stream)
			return false;
		m_stream.seekg(0, std::ios::end);
		const auto end = m_stream.tellg();
		if (end < 0)
			return false;
		m_size = static_cast<uint64_t>(end);
		std::array<unsigned char, 2048> descriptor{};
		for (uint64_t sector = 16; sector < 80; ++sector) {
			if (!ReadAt(sector * 2048, descriptor.data(), descriptor.size()) ||
				std::memcmp(descriptor.data() + 1, "CD001", 5) != 0 ||
				descriptor[6] != 1)
				return false;
			if (descriptor[0] == 255)
				return false;
			if (descriptor[0] != 1)
				continue;
			if (descriptor[128] != 0 || descriptor[129] != 8 ||
				descriptor[130] != 8 || descriptor[131] != 0)
				return false;
			Node root{};
			if (!Decode(descriptor.data() + 156, 2048 - 156, root) ||
				root.is_file)
				return false;
			m_nodes.emplace("", root);
			return Index("", root, 0);
		}
		return false;
	}

	std::optional<Entry> Find(std::string_view member) override {
		const auto it = m_nodes.find(Key(member));
		if (it == m_nodes.end())
			return {};
		return Entry{it->second.offset, it->second.size, it->second.is_file};
	}

	std::vector<File::DirEntry> List(std::string_view member) override {
		const auto it = m_children.find(Key(member));
		return it == m_children.end() ? std::vector<File::DirEntry>{}
									  : it->second;
	}

	uint64_t Read(uint64_t id, uint64_t offset, uint32_t size,
				  void *data) override {
		std::lock_guard lock(m_mutex);
		if (id > m_size || offset > m_size - id)
			return 0;
		return ReadAt(id + offset, data, size) ? size : 0;
	}

  private:
	struct Node {
		uint64_t offset;
		uint64_t size;
		bool is_file;
	};
	static std::string Key(std::string_view name) {
		std::string result(name);
		for (auto &c : result)
			if (c >= 'A' && c <= 'Z')
				c += 'a' - 'A';
		return result;
	}
	static uint32_t Little32(const unsigned char *p) {
		return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) |
			   (uint32_t(p[3]) << 24);
	}
	bool ReadAt(uint64_t offset, void *data, size_t size) {
		if (offset > m_size || size > m_size - offset)
			return false;
		m_stream.clear();
		m_stream.seekg(static_cast<std::streamoff>(offset));
		m_stream.read(static_cast<char *>(data),
					  static_cast<std::streamsize>(size));
		return m_stream.good();
	}
	bool Decode(const unsigned char *record, size_t available,
				Node &node) const {
		if (available < 34 || record[0] < 34 || record[0] > available ||
			record[32] == 0 || 33u + record[32] > record[0] || record[1] != 0 ||
			(record[25] & 0x80) != 0 || record[26] != 0 || record[27] != 0)
			return false;
		node = {uint64_t(Little32(record + 2)) * 2048, Little32(record + 10),
				(record[25] & 2) == 0};
		return node.offset <= m_size && node.size <= m_size - node.offset;
	}
	bool Index(const std::string &parent, Node directory, unsigned depth) {
		if (depth > 64 || directory.size > (uint64_t{32} << 20) ||
			directory.size > (uint64_t{64} << 20) - m_metadata_size)
			return false;
		m_metadata_size += directory.size;
		// Each directory extent may be visited once: reject cycles and aliases.
		if (!m_directories.emplace(directory.offset, true).second)
			return false;
		std::vector<unsigned char> bytes(directory.size);
		if (!ReadAt(directory.offset, bytes.data(), bytes.size()))
			return false;
		std::vector<std::pair<std::string, Node>> subdirs;
		for (size_t pos = 0; pos < bytes.size();) {
			if (bytes[pos] == 0) {
				pos = (pos / 2048 + 1) * 2048;
				continue;
			}
			const auto *record = bytes.data() + pos;
			Node node{};
			if (!Decode(record, std::min(bytes.size() - pos, 2048 - pos % 2048),
						node))
				return false;
			pos += record[0];
			if (record[32] == 1 && (record[33] == 0 || record[33] == 1))
				continue;
			std::string name(reinterpret_cast<const char *>(record + 33),
							 record[32]);
			if (const auto version = name.find(';');
				version != std::string::npos)
				name.resize(version);
			if (node.is_file && !name.empty() && name.back() == '.')
				name.pop_back();
			name = Key(name);
			if (name.empty() || name == "." || name == ".." ||
				name.find_first_of("/\\\0", 0, 3) != std::string::npos)
				return false;
			const auto path = parent.empty() ? name : parent + "/" + name;
			if (m_nodes.size() >= (1u << 20) ||
				!m_nodes.emplace(path, node).second)
				return false;
			m_children[parent].push_back({name, node.is_file});
			if (!node.is_file)
				subdirs.emplace_back(path, node);
		}
		for (const auto &[path, node] : subdirs)
			if (!Index(path, node, depth + 1))
				return false;
		return true;
	}
	std::ifstream m_stream;
	uint64_t m_size = 0;
	uint64_t m_metadata_size = 0;
	std::mutex m_mutex;
	std::unordered_map<std::string, Node> m_nodes;
	std::unordered_map<std::string, std::vector<File::DirEntry>> m_children;
	std::unordered_map<uint64_t, bool> m_directories;
};

std::shared_ptr<ArchiveReader>
OpenIsoArchive(const std::filesystem::path &path) {
	auto reader = std::make_shared<IsoArchiveReader>(path);
	return reader->Initialize() ? reader : nullptr;
}

struct ArchiveFormat {
	std::string_view extension;
	std::shared_ptr<ArchiveReader> (*open)(const std::filesystem::path &);
};

constexpr ArchiveFormat Formats[] = {{".zar", OpenZArchive}, {".iso", OpenIsoArchive}};
using NativeView                  = std::basic_string_view<std::filesystem::path::value_type>;

const ArchiveFormat* FindFormat(NativeView path) {
	for (const auto& format: Formats) {
		if (path.size() < format.extension.size()) {
			continue;
		}
		const auto extension = path.substr(path.size() - format.extension.size());
		if (std::equal(
		        extension.begin(), extension.end(), format.extension.begin(),
		        [](auto a, char b) { return (a >= 'A' && a <= 'Z' ? a + ('a' - 'A') : a) == b; })) {
			return &format;
		}
	}
	return nullptr;
}

size_t FindMarker(NativeView path) {
	// Native files use this check on every operation. The usual path has no marker and allocates
	// nothing.
	for (auto marker = path.find('!'); marker != NativeView::npos;
	     marker      = path.find('!', marker + 1)) {
		if ((marker + 1 == path.size() || path[marker + 1] == '/' || path[marker + 1] == '\\') &&
		    FindFormat(path.substr(0, marker)) != nullptr) {
			return marker;
		}
	}
	return NativeView::npos;
}

struct ParsedPath {
	std::filesystem::path archive;
	std::string           member;
};

std::optional<ParsedPath> ParsePath(const std::filesystem::path& path) {
	const auto& native = path.native();
	const auto  marker = FindMarker(native);
	if (marker == NativeView::npos) {
		return {};
	}
	ParsedPath parsed {.archive = std::filesystem::path(native.substr(0, marker))};
	auto       member = PathToGenericString(std::filesystem::path(native.substr(marker + 1)));
	std::replace(member.begin(), member.end(), '\\', '/');
	std::string_view remaining = member;
	while (!remaining.empty()) {
		const auto separator = remaining.find('/');
		const auto component = remaining.substr(0, separator);
		remaining.remove_prefix(separator == std::string_view::npos ? remaining.size() : separator + 1);
		if (component.empty() || component == ".") {
			continue;
		}
		if (component == "..") {
			if (parsed.member.empty()) {
				return {};
			}
			const auto separator = parsed.member.rfind('/');
			parsed.member.resize(separator == std::string::npos ? 0 : separator);
		} else {
			if (!parsed.member.empty()) {
				parsed.member += '/';
			}
			parsed.member += component;
		}
	}
	return parsed;
}

std::mutex                                                              g_readers_mutex;
std::unordered_map<std::filesystem::path, std::weak_ptr<ArchiveReader>> g_readers;

// Guest stacks can be only 64 KiB. Decompress on an ordinary host stack, then copy into guest
// memory on the caller so GPU-tracked writes use its fault handler. A single worker bounds host
// threads and avoids duplicating the archive's own block cache. Requests live on waiting callers.
class ArchiveIoThread {
public:
	static ArchiveIoThread& Instance() {
		// Guest exit can run static destructors while other guest threads are still reading.
		// Keep the worker alive until process termination, as with the guest threads themselves.
		static auto* instance = new ArchiveIoThread;
		return *instance;
	}

	uint64_t Read(ArchiveReader& reader, uint64_t id, uint64_t offset, uint32_t size, void* data) {
		Request          request {reader, id, offset, size, data};
		std::unique_lock lock(m_mutex);
		if (m_last != nullptr) {
			m_last->next = &request;
		} else {
			m_first = &request;
		}
		m_last = &request;
		m_wake.notify_one();
		request.ready.wait(lock, [&] { return request.done; });
		return request.result;
	}

	KYTY_CLASS_NO_COPY(ArchiveIoThread);

private:
	struct Request {
		ArchiveReader&          reader;
		uint64_t                id;
		uint64_t                offset;
		uint32_t                size;
		void*                   data;
		Request*                next   = nullptr;
		uint64_t                result = 0;
		bool                    done   = false;
		std::condition_variable ready {};
	};

	ArchiveIoThread() {
		std::thread([this] { Loop(); }).detach();
	}

	void Loop() {
		for (;;) {
			Request* request;
			{
				std::unique_lock lock(m_mutex);
				m_wake.wait(lock, [this] { return m_first != nullptr; });
				request = m_first;
				m_first = request->next;
				if (m_first == nullptr) {
					m_last = nullptr;
				}
			}
			const auto result =
			    request->reader.Read(request->id, request->offset, request->size, request->data);
			{
				std::lock_guard lock(m_mutex);
				request->result = result;
				request->done   = true;
				// Notify before releasing the mutex: the waiting caller owns the condition
				// variable.
				request->ready.notify_one();
			}
		}
	}

	std::mutex              m_mutex;
	std::condition_variable m_wake;
	Request*                m_first = nullptr;
	Request*                m_last  = nullptr;
};

} // namespace

struct ArchiveFile::Private {
	std::shared_ptr<ArchiveReader> reader;
	ArchiveReader::Entry           entry;
	uint64_t                       position = 0;
};

ArchiveFile::ArchiveFile(std::unique_ptr<Private> p): m_p(std::move(p)) {}
ArchiveFile::~ArchiveFile() = default;

uint64_t ArchiveFile::Size() const {
	return m_p->entry.size;
}
uint64_t ArchiveFile::Tell() const {
	return m_p->position;
}
bool ArchiveFile::Seek(uint64_t offset) {
	m_p->position = offset;
	return true;
}

void ArchiveFile::Read(void* data, uint32_t size, uint32_t* bytes_read) {
#if defined(KYTY_ARCHIVE_PTHREAD_CANCELLATION)
	// A pending cancellation must not destroy the waiting request or its TLS buffer while the
	// worker still uses them. Restore the caller's state only after the copy is complete.
	int cancellation_state = PTHREAD_CANCEL_ENABLE;
	pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &cancellation_state);
#endif
	constexpr uint32_t                chunk_size = 1u << 20u;
	thread_local std::vector<uint8_t> chunk;
	uint32_t                          read = 0;
	if (data != nullptr && size != 0 && m_p->position < Size()) {
		const auto requested =
		    static_cast<uint32_t>(std::min<uint64_t>(size, Size() - m_p->position));
		if (chunk.size() < std::min(chunk_size, requested)) {
			chunk.resize(std::min(chunk_size, requested));
		}
		while (read < requested) {
			const auto step = std::min(chunk_size, requested - read);
			const auto got  = ArchiveIoThread::Instance().Read(*m_p->reader, m_p->entry.id,
			                                                   m_p->position, step, chunk.data());
			if (got == 0 || got > step) {
				break;
			}
			std::memcpy(static_cast<uint8_t*>(data) + read, chunk.data(), got);
			read += static_cast<uint32_t>(got);
			m_p->position += got;
			if (got < step) {
				break;
			}
		}
	}
	if (bytes_read != nullptr) {
		*bytes_read = read;
	}
#if defined(KYTY_ARCHIVE_PTHREAD_CANCELLATION)
	pthread_setcancelstate(cancellation_state, nullptr);
#endif
}

std::filesystem::path MakeArchivePath(const std::filesystem::path& archive,
                                      const std::filesystem::path& member) {
	auto root = archive;
	root += "!";
	return member.empty() ? root : root / member.relative_path();
}

bool IsSupportedArchive(const std::filesystem::path& path) {
	return FindFormat(path.native()) != nullptr;
}

bool IsArchivePath(const std::filesystem::path& path) {
	return FindMarker(path.native()) != NativeView::npos;
}

std::filesystem::path GetArchiveHostPath(const std::filesystem::path& path) {
	const auto marker = FindMarker(path.native());
	return marker == NativeView::npos ? std::filesystem::path {}
	                                  : std::filesystem::path(path.native().substr(0, marker));
}

std::shared_ptr<ArchiveReader> OpenArchive(const std::filesystem::path& path) {
	const auto  archive = IsArchivePath(path) ? GetArchiveHostPath(path) : path;
	const auto* format  = FindFormat(archive.native());
	if (format == nullptr) {
		return {};
	}
	std::error_code error;
	auto            key = std::filesystem::absolute(archive, error);
	if (error) {
		return {};
	}
	{
		std::lock_guard lock(g_readers_mutex);
		if (auto it = g_readers.find(key); it != g_readers.end()) {
			if (auto reader = it->second.lock()) {
				return reader;
			}
		}
	}
	// Do not hold the cache mutex while opening an unrelated archive from disk.
	auto reader = format->open(archive);
	if (reader == nullptr) {
		return {};
	}
	std::lock_guard lock(g_readers_mutex);
	std::erase_if(g_readers, [](const auto& entry) { return entry.second.expired(); });
	auto& cached = g_readers[key];
	if (auto existing = cached.lock()) {
		return existing;
	}
	cached = reader;
	return reader;
}

std::optional<File::Info> GetArchiveInfo(const std::filesystem::path& path) {
	const auto parsed = ParsePath(path);
	if (parsed) {
		if (auto reader = OpenArchive(parsed->archive)) {
			if (auto entry = reader->Find(parsed->member)) {
				return File::Info {entry->is_file, entry->size};
			}
		}
	}
	return {};
}

std::vector<File::DirEntry> GetArchiveDirEntries(const std::filesystem::path& path) {
	const auto parsed = ParsePath(path);
	if (parsed) {
		if (auto reader = OpenArchive(parsed->archive)) {
			return reader->List(parsed->member);
		}
	}
	return {};
}

std::unique_ptr<ArchiveFile> OpenArchiveFile(const std::filesystem::path& path) {
	auto parsed = ParsePath(path);
	if (!parsed) {
		return {};
	}
	auto       reader = OpenArchive(parsed->archive);
	const auto entry  = reader ? reader->Find(parsed->member) : std::nullopt;
	if (!entry || !entry->is_file) {
		return {};
	}
	auto p     = std::make_unique<ArchiveFile::Private>();
	p->reader  = std::move(reader);
	p->entry   = *entry;
	return std::unique_ptr<ArchiveFile>(new ArchiveFile(std::move(p)));
}

} // namespace Common
