/*
 * Copyright (C) 2014  Xiao-Long Chen <chenxiaolong@cxl.epac.to>
 *
 * This file is part of MultiBootPatcher
 *
 * MultiBootPatcher is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * MultiBootPatcher is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with MultiBootPatcher.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "cpiofile.h"

#include <cstring>

#include <algorithm>
#include <utility>
#include <vector>

#include <archive.h>
#include <archive_entry.h>

#include "private/fileutils.h"
#include "private/logging.h"


typedef std::pair<archive_entry *, std::vector<unsigned char>> FilePair;

class CpioFile::Impl
{
public:
    ~Impl();

    std::vector<FilePair> files;

    PatcherError error;
};


CpioFile::Impl::~Impl()
{
    for (auto &p : files) {
        archive_entry_free(p.first);
    }
}


/*!
 * \class CpioFile
 *
 * This is a very simple class that allows a cpio archive to be manipulated. It
 * is meant for use with ramdisk cpio archives. The only supported operations
 * are:
 *
 * - Adding regular files
 * - Adding symlinks
 * - Checking existence of files
 * - Removing files
 */


CpioFile::CpioFile() : m_impl(new Impl())
{
}

CpioFile::~CpioFile()
{
}

/*!
 * \brief Get error information
 *
 * \note The returned PatcherError contains valid information only if an
 *       operation has failed.
 *
 * \return PatcherError containing information about the error
 */
PatcherError CpioFile::error() const
{
    return m_impl->error;
}

/*!
 * \brief Load a cpio archive from binary data
 *
 * This function loads a cpio archive from a vector containing the binary data.
 * Each file's metadata and contents will be copied and stored.
 *
 * \warning If the cpio archive cannot be loaded, this CpioFile object may be
 *          left in an inconsistent state. Create a new CpioFile to load another
 *          cpio archive.
 *
 * \return Whether the cpio archive was successfully read
 */
bool CpioFile::load(const std::vector<unsigned char> &data)
{
    archive *a;
    archive_entry *entry;

    a = archive_read_new();

    // Allow gzip-compressed cpio files to work as well
    // (libarchive is awesome)
    archive_read_support_filter_gzip(a);
    archive_read_support_format_cpio(a);

    int ret = archive_read_open_memory(a,
            const_cast<unsigned char *>(data.data()), data.size());
    if (ret != ARCHIVE_OK) {
        Log::log(Log::Warning, "libarchive: %s", archive_error_string(a));
        archive_read_free(a);

        m_impl->error = PatcherError::createArchiveError(
                PatcherError::ArchiveReadOpenError, "<memory>");
        return false;
    }

    while ((ret = archive_read_next_header(a, &entry)) == ARCHIVE_OK) {
        // Read the data for the entry
        std::vector<unsigned char> entryData;
        entryData.reserve(archive_entry_size(entry));

        int r;
        __LA_INT64_T offset;
        const void *buff;
        size_t bytes_read;

        while ((r = archive_read_data_block(a, &buff,
                &bytes_read, &offset)) == ARCHIVE_OK) {
            entryData.insert(entryData.end(),
                             reinterpret_cast<const char *>(buff),
                             reinterpret_cast<const char *>(buff) + bytes_read);
        }

        if (r < ARCHIVE_WARN) {
            Log::log(Log::Warning, "libarchive: %s", archive_error_string(a));
            archive_read_free(a);

            m_impl->error = PatcherError::createArchiveError(
                    PatcherError::ArchiveReadDataError,
                    archive_entry_pathname(entry));
            return false;
        }

        // Save the header and data
        archive_entry *cloned = archive_entry_clone(entry);
        m_impl->files.push_back(std::make_pair(cloned, std::move(entryData)));
    }

    if (ret < ARCHIVE_WARN) {
        Log::log(Log::Warning, "libarchive: %s", archive_error_string(a));
        archive_read_free(a);

        m_impl->error = PatcherError::createArchiveError(
                PatcherError::ArchiveReadHeaderError, std::string());
        return false;
    }

    ret = archive_read_free(a);
    if (ret != ARCHIVE_OK) {
        Log::log(Log::Warning, "libarchive: %s", archive_error_string(a));

        m_impl->error = PatcherError::createArchiveError(
                PatcherError::ArchiveFreeError, std::string());
        return false;
    }

    return true;
}

static int archiveOpenCallback(archive *a, void *clientData)
{
    (void) a;
    (void) clientData;
    return ARCHIVE_OK;
}

static __LA_SSIZE_T archiveWriteCallback(archive *, void *clientData,
                                         const void *buffer, size_t length)
{
    auto data = reinterpret_cast<std::vector<unsigned char> *>(clientData);
    data->insert(data->end(),
                 reinterpret_cast<const char *>(buffer),
                 reinterpret_cast<const char *>(buffer) + length);
    return length;
}

static int archiveCloseCallback(archive *a, void *clientData)
{
    (void) a;
    (void) clientData;
    return ARCHIVE_OK;
}

static bool sortByName(const FilePair &p1, const FilePair &p2)
{
    const char *cname1 = archive_entry_pathname(p1.first);
    const char *cname2 = archive_entry_pathname(p2.first);

    return std::strcmp(cname1, cname2) < 0;
}

/*!
 * \brief Constructs the cpio archive
 *
 * This function builds the `.cpio` file, optionally compressing it with gzip.
 * The archive uses the `newc` format and files are written in lexographical
 * order.
 *
 * \return Cpio archive binary data
 */
std::vector<unsigned char> CpioFile::createData(bool gzip)
{
    archive *a;
    std::vector<unsigned char> data;

    a = archive_write_new();

    archive_write_set_format_cpio_newc(a);

    if (gzip) {
        archive_write_add_filter_gzip(a);
    } else {
        archive_write_add_filter_none(a);
    }

    archive_write_set_bytes_per_block(a, 512);

    int ret = archive_write_open(a, reinterpret_cast<void *>(&data),
                                 &archiveOpenCallback,
                                 &archiveWriteCallback,
                                 &archiveCloseCallback);
    if (ret != ARCHIVE_OK) {
        Log::log(Log::Warning, "libarchive: %s", archive_error_string(a));
        archive_write_fail(a);
        archive_write_free(a);

        m_impl->error = PatcherError::createArchiveError(
                PatcherError::ArchiveWriteOpenError, "<memory>");
        return std::vector<unsigned char>();
    }

    for (auto const &p : m_impl->files) {
        if (archive_write_header(a, p.first) != ARCHIVE_OK) {
            Log::log(Log::Warning, "libarchive: %s : %s",
                     archive_error_string(a),
                     archive_entry_pathname(p.first));

            archive_write_fail(a);
            archive_write_free(a);

            m_impl->error = PatcherError::createArchiveError(
                    PatcherError::ArchiveWriteHeaderError,
                    archive_entry_pathname(p.first));
            return std::vector<unsigned char>();
        }

        if (archive_write_data(a, p.second.data(), p.second.size())
                != int(p.second.size())) {
            archive_write_fail(a);
            archive_write_free(a);

            m_impl->error = PatcherError::createArchiveError(
                    PatcherError::ArchiveWriteDataError,
                    archive_entry_pathname(p.first));
            return std::vector<unsigned char>();
        }
    }

    ret = archive_write_close(a);

    if (ret != ARCHIVE_OK) {
        Log::log(Log::Warning, "libarchive: %s", archive_error_string(a));
        archive_write_fail(a);
        archive_write_free(a);

        m_impl->error = PatcherError::createArchiveError(
                PatcherError::ArchiveCloseError, std::string());
        return std::vector<unsigned char>();
    }

    archive_write_free(a);

    return data;
}

/*!
 * \brief Check if a file exists in the cpio archive
 *
 * \note A full path within the archive must be specified. For example, if the
 *       archive contains `sbin/busybox`, calling this function with `busybox`
 *       will return false.
 *
 * \return Whether or not the file exists
 */
bool CpioFile::exists(const std::string &name) const
{
    for (auto const &p : m_impl->files) {
        if (name == archive_entry_pathname(p.first)) {
            return true;
        }
    }

    return false;
}

/*!
 * \brief Remove a file from the cpio archive
 *
 * \note The full path to the file within the archive should be specified.
 *
 * \warning This function does not perform any checks on what is being removed.
 *          The behavior when creating an archive, if a directory containing
 *          files is removed, is undefined.
 *
 * \return Whether the file was removed
 */
bool CpioFile::remove(const std::string &name)
{
    for (auto it = m_impl->files.begin(); it != m_impl->files.end(); ++it) {
        archive_entry *entry = (*it).first;
        if (name == archive_entry_pathname(entry)) {
            archive_entry_free(entry);
            m_impl->files.erase(it);
            return true;
        }
    }

    return false;
}

/*!
 * \brief List of files in the cpio archive
 *
 * \return List of filesnames
 */
std::vector<std::string> CpioFile::filenames() const
{
    std::vector<std::string> list;

    for (auto const &p : m_impl->files) {
        list.push_back(archive_entry_pathname(p.first));
    }

    return list;
}

/*!
 * \brief Get contents of a file in the archive
 *
 * \todo This returned value is currently ambiguous as both an empty file and
 *       a non-existant file will return an empty vector.
 *
 * \return Contents of specified file
 */
std::vector<unsigned char> CpioFile::contents(const std::string &name) const
{
    for (auto const &p : m_impl->files) {
        if (name == archive_entry_pathname(p.first)) {
            return p.second;
        }
    }

    return std::vector<unsigned char>();
}

/*!
 * \brief Set contents of a file in the archive
 *
 * \todo No error is reported if the client/user attempts to set the contents
 *       for a file that doesn't exist in the archive.
 *
 * \note The contents of a symbolic link is the link's target path.
 */
void CpioFile::setContents(const std::string &name,
                           std::vector<unsigned char> data)
{
    for (auto &p : m_impl->files) {
        if (name == archive_entry_pathname(p.first)) {
            archive_entry_set_size(p.first, data.size());
            p.second = std::move(data);
            break;
        }
    }
}

/*!
 * \brief Add a symbolic link to the archive
 *
 * \note This function will not overwrite an existing file.
 *
 * \param source Source path
 * \param target Target path
 *
 * \return Whether the symlink was added
 */
bool CpioFile::addSymlink(const std::string &source, const std::string &target)
{
    if (exists(target)) {
        m_impl->error = PatcherError::createCpioError(
                PatcherError::CpioFileAlreadyExistsError, target);
        return false;
    }

    archive_entry *entry = archive_entry_new();

    archive_entry_set_uid(entry, 0);
    archive_entry_set_gid(entry, 0);
    archive_entry_set_nlink(entry, 1);
    archive_entry_set_mtime(entry, 0, 0);
    archive_entry_set_devmajor(entry, 0);
    archive_entry_set_devminor(entry, 0);
    archive_entry_set_rdevmajor(entry, 0);
    archive_entry_set_rdevminor(entry, 0);

    archive_entry_set_pathname(entry, target.c_str());
    archive_entry_set_size(entry, 0);
    archive_entry_set_symlink(entry, source.c_str());

    archive_entry_set_filetype(entry, AE_IFLNK);
    archive_entry_set_perm(entry, 0777);

    m_impl->files.push_back(std::make_pair(entry, std::vector<unsigned char>()));

    std::sort(m_impl->files.begin(), m_impl->files.end(), sortByName);

    return true;
}

/*!
 * \brief Add a file to the archive
 *
 * \note This function will not overwrite an existing file.
 *
 * \param path Path to file to add
 * \param name Target path in archive
 * \param perms Octal unix permissions
 *
 * \return Whether the file was added
 */
bool CpioFile::addFile(const std::string &path, const std::string &name,
                       unsigned int perms)
{
    if (exists(name)) {
        m_impl->error = PatcherError::createCpioError(
                PatcherError::CpioFileAlreadyExistsError, name);
        return false;
    }

    std::vector<unsigned char> contents;
    auto ret = FileUtils::readToMemory(path, &contents);
    if (ret.errorCode() != PatcherError::NoError) {
        m_impl->error = ret;
        return false;
    }

    return addFile(std::move(contents), name, perms);
}

/*!
 * \brief Add a file (from binary data) to the archive
 *
 * \note This function will not overwrite an existing file.
 *
 * \param contents Binary contents of file
 * \param name Target path in archive
 * \param perms Octal unix permissions
 *
 * \return Whether the file was added
 */
bool CpioFile::addFile(std::vector<unsigned char> contents,
                       const std::string &name, unsigned int perms)
{
    if (exists(name)) {
        m_impl->error = PatcherError::createCpioError(
                PatcherError::CpioFileAlreadyExistsError, name);
        return false;
    }

    archive_entry *entry = archive_entry_new();

    archive_entry_set_uid(entry, 0);
    archive_entry_set_gid(entry, 0);
    archive_entry_set_nlink(entry, 1);
    archive_entry_set_mtime(entry, 0, 0);
    archive_entry_set_devmajor(entry, 0);
    archive_entry_set_devminor(entry, 0);
    archive_entry_set_rdevmajor(entry, 0);
    archive_entry_set_rdevminor(entry, 0);

    archive_entry_set_pathname(entry, name.c_str());
    archive_entry_set_size(entry, contents.size());

    archive_entry_set_filetype(entry, AE_IFREG);
    archive_entry_set_perm(entry, perms);

    m_impl->files.push_back(std::make_pair(entry, std::move(contents)));

    std::sort(m_impl->files.begin(), m_impl->files.end(), sortByName);

    return true;
}
