/**
 *  Sar.cpp
 *  ONScripter-RU
 *
 *  SAR archive game resources reader.
 *
 *  Consult LICENSE file for licensing terms and copyright holders.
 */

#include "Engine/Readers/Sar.hpp"
#include "Engine/Readers/ArchiveParser.hpp"
#include "Engine/Components/Async.hpp"
#include "Support/FileIO.hpp"

#include <algorithm>
#include <limits>
#include <memory>
#include <sys/stat.h>

namespace {
std::string normalizeArchiveLookupName(const char *file_name) {
	std::string normalized(file_name ? file_name : "");
	for (char &ch : normalized) {
		if ('a' <= ch && ch <= 'z')
			ch += 'A' - 'a';
		else if (ch == '/')
			ch = '\\';
	}
	return normalized;
}

void buildArchiveIndex(BaseReader::ArchiveInfo *ai) {
	if (!ai || !ai->fi_list)
		return;

	ai->file_index.clear();
	ai->file_index.reserve(ai->num_of_files);
	for (size_t i = 0; i < ai->num_of_files; ++i)
		ai->file_index.emplace(ai->fi_list[i].name, i);
}

bool archiveFileSize(FILE *fp, size_t &size) {
	struct stat info{};
	if (!fp || fstat(fileno(fp), &info) != 0 || info.st_size < 0 ||
	    static_cast<uintmax_t>(info.st_size) > std::numeric_limits<size_t>::max())
		return false;
	size = static_cast<size_t>(info.st_size);
	return true;
}
} // namespace

SarReader::SarReader(DirPaths &path)
    : DirectReader(path) {
	root_archive_info = last_archive_info = &archive_info;
	num_of_sar_archives                   = 0;
}

SarReader::~SarReader() {
	close();
}

int SarReader::open(const char *name) {
	ArchiveInfo *info = new ArchiveInfo();

	if ((info->file_handle = lookupFile(name, "rb")) == nullptr) {
		delete info;
		return -1;
	}

	info->file_name = new char[std::strlen(name) + 1];
	std::memcpy(info->file_name, name, strlen(name) + 1);

	if (readArchive(info) != 0) {
		delete info;
		return -1;
	}

	last_archive_info->next = info;
	last_archive_info       = last_archive_info->next;
	num_of_sar_archives++;

	return 0;
}

int SarReader::readArchive(ArchiveInfo *ai, int archive_type, size_t offset) {
	if (!ai || !ai->file_handle)
		return -1;

	size_t archiveSize = 0;
	if (!archiveFileSize(ai->file_handle, archiveSize))
		return -1;

	ArchiveParser::Type type;
	switch (archive_type) {
		case ARCHIVE_TYPE_SAR: type = ArchiveParser::Type::Sar; break;
		case ARCHIVE_TYPE_NSA: type = ArchiveParser::Type::Nsa; break;
		case ARCHIVE_TYPE_NS2: type = ArchiveParser::Type::Ns2; break;
		default: return -1;
	}

	ArchiveParser::Result parsed;
	std::string parseError;
	if (!ArchiveParser::parse(ai->file_handle, archiveSize, type, offset,
	                          sizeof(FileInfo{}.name) - 1, parsed, parseError)) {
		sendToLog(LogLevel::Error, "Invalid or truncated archive %s: %s\n",
		          ai->file_name ? ai->file_name : "(unnamed)", parseError.c_str());
		ai->file_index.clear();
		delete[] ai->fi_list;
		ai->fi_list = nullptr;
		ai->num_of_files = 0;
		ai->base_offset = 0;
		return -1;
	}

	ai->base_offset = parsed.baseOffset;
	ai->num_of_files = parsed.entries.size();
	ai->fi_list = ai->num_of_files ? new FileInfo[ai->num_of_files] : nullptr;
	for (size_t i = 0; i < ai->num_of_files; ++i) {
		const auto &source = parsed.entries[i];
		auto &destination = ai->fi_list[i];
		std::memcpy(destination.name, source.name.c_str(), source.name.size() + 1);
		destination.offset = source.offset;
		destination.length = source.length;
		destination.original_length = source.originalLength;
		if (source.compressed) {
			sendToLog(LogLevel::Error, "Reading of %s might fail due to compression.\n"
			                           "Refrain from using any compression on media files!\n",
			          destination.name);
		}
	}
	buildArchiveIndex(ai);
	return 0;
}

int SarReader::close() {
	ArchiveInfo *info = archive_info.next;

	for (size_t i = 0; i < num_of_sar_archives; i++) {
		last_archive_info = info;
		info              = info->next;
		delete last_archive_info;
	}
	num_of_sar_archives = 0;

	return 0;
}

const char *SarReader::getArchiveName() const {
	return "sar";
}

size_t SarReader::getNumFiles() {
	ArchiveInfo *info = archive_info.next;
	size_t num        = 0;

	for (size_t i = 0; i < num_of_sar_archives; i++) {
		num += info->num_of_files;
		info = info->next;
	}

	return num;
}

size_t SarReader::getIndexFromFile(ArchiveInfo *ai, const char *file_name) {
	if (!ai)
		return 0;

	const auto normalized = normalizeArchiveLookupName(file_name);
	const auto entry = ai->file_index.find(normalized);
	return entry == ai->file_index.end() ? ai->num_of_files : entry->second;
}

bool SarReader::getFileSub(ArchiveInfo *ai, const char *file_name, size_t &len, uint8_t **buffer) {
	if (!ai)
		return false;
	size_t i = getIndexFromFile(ai, file_name);
	if (i == ai->num_of_files)
		return false;

	len = ai->fi_list[i].length;
	if (len == std::numeric_limits<size_t>::max())
		return false;

	if (buffer && len > 0) {
		auto data = std::make_unique<uint8_t[]>(len + 1);
		Lock lock(ai->file_handle);
		FileIO::seekFile(ai->file_handle, ai->fi_list[i].offset, SEEK_SET);
		if (std::fread(data.get(), 1, len, ai->file_handle) != len)
			throw std::runtime_error("Error reading file");
		data[len] = 0x00;
		*buffer = data.release();
	} else if (buffer) {
		*buffer = nullptr;
	}

	return true;
}

bool SarReader::getFileSub(ArchiveInfo *ai, const char *file_name, size_t &len, std::vector<uint8_t> &buffer) {
	if (!ai)
		return false;
	size_t i = getIndexFromFile(ai, file_name);
	if (i == ai->num_of_files)
		return false;

	len = ai->fi_list[i].length;
	if (len == std::numeric_limits<size_t>::max())
		return false;
	if (buffer.size() < len + 1)
		buffer.resize(len + 1);
	if (len > 0) {
		Lock lock(ai->file_handle);
		FileIO::seekFile(ai->file_handle, ai->fi_list[i].offset, SEEK_SET);
		if (std::fread(buffer.data(), len, 1, ai->file_handle) != 1)
			throw std::runtime_error("Error reading file");
	}
	buffer[len] = 0x00;
	return true;
}

bool SarReader::getFile(const char *file_name, size_t &len, uint8_t **buffer) {
	if (DirectReader::getFile(file_name, len, buffer))
		return true;

	ArchiveInfo *info = archive_info.next;

	for (size_t i = 0; i < num_of_sar_archives; i++) {
		if (getFileSub(info, file_name, len, buffer))
			return true;
		info = info->next;
	}

	return false;
}

bool SarReader::getFile(const char *file_name, size_t &len, std::vector<uint8_t> &buffer) {
	if (DirectReader::getFile(file_name, len, buffer))
		return true;

	ArchiveInfo *info = archive_info.next;

	for (size_t i = 0; i < num_of_sar_archives; i++) {
		if (getFileSub(info, file_name, len, buffer))
			return true;
		info = info->next;
	}

	return false;
}

bool SarReader::updateVector(std::vector<uint8_t> &buffer, uint8_t *tmp, size_t len) {
	// We should not really need this, so let's just have a low-speed version for completeness.
	if (tmp) {
		if (buffer.size() < len + 1)
			buffer.resize(len + 1);
		std::memcpy(buffer.data(), tmp, len);
		buffer[len] = 0x00;
		freearr(&tmp);
	}
	return true;
}
