//===- VirtualFileSystem.cpp - Virtual File System Layer ------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file implements the VirtualFileSystem interface.
//
//===----------------------------------------------------------------------===//

#include "clang/Basic/VirtualFileSystem.h"
#include "clang/Basic/LLVM.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/IntrusiveRefCntPtr.h"
#include "llvm/ADT/Optional.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/ADT/Twine.h"
#include "llvm/ADT/iterator_range.h"
#include "llvm/Config/llvm-config.h"
#include "llvm/Support/Compiler.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/Chrono.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/Errc.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/ErrorOr.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/Process.h"
#include "llvm/Support/SMLoc.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/YAMLParser.h"
#include "llvm/Support/raw_ostream.h"
#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

using namespace clang;
using namespace vfs;
using namespace llvm;

using llvm::sys::fs::file_status;
using llvm::sys::fs::file_type;
using llvm::sys::fs::perms;
using llvm::sys::fs::UniqueID;

Status::Status(const file_status &Status)
    : UID(Status.getUniqueID()), MTime(Status.getLastModificationTime()),
      User(Status.getUser()), Group(Status.getGroup()), Size(Status.getSize()),
      Type(Status.type()), Perms(Status.permissions())  {}

Status::Status(StringRef Name, UniqueID UID, sys::TimePoint<> MTime,
               uint32_t User, uint32_t Group, uint64_t Size, file_type Type,
               perms Perms)
    : Name(Name), UID(UID), MTime(MTime), User(User), Group(Group), Size(Size),
      Type(Type), Perms(Perms) {}

Status Status::copyWithNewName(const Status &In, StringRef NewName) {
  return Status(NewName, In.getUniqueID(), In.getLastModificationTime(),
                In.getUser(), In.getGroup(), In.getSize(), In.getType(),
                In.getPermissions());
}

Status Status::copyWithNewName(const file_status &In, StringRef NewName) {
  return Status(NewName, In.getUniqueID(), In.getLastModificationTime(),
                In.getUser(), In.getGroup(), In.getSize(), In.type(),
                In.permissions());
}

bool Status::equivalent(const Status &Other) const {
  assert(isStatusKnown() && Other.isStatusKnown());
  return getUniqueID() == Other.getUniqueID();
}

bool Status::isDirectory() const {
  return Type == file_type::directory_file;
}

bool Status::isRegularFile() const {
  return Type == file_type::regular_file;
}

bool Status::isOther() const {
  return exists() && !isRegularFile() && !isDirectory() && !isSymlink();
}

bool Status::isSymlink() const {
  return Type == file_type::symlink_file;
}

bool Status::isStatusKnown() const {
  return Type != file_type::status_error;
}

bool Status::exists() const {
  return isStatusKnown() && Type != file_type::file_not_found;
}

File::~File() = default;

FileSystem::~FileSystem() = default;

ErrorOr<std::unique_ptr<MemoryBuffer>>
FileSystem::getBufferForFile(const llvm::Twine &Name, int64_t FileSize,
                             bool RequiresNullTerminator, bool IsVolatile) {
  auto F = openFileForRead(Name);
  if (!F)
    return F.getError();

  return (*F)->getBuffer(Name, FileSize, RequiresNullTerminator, IsVolatile);
}

std::error_code FileSystem::makeAbsolute(SmallVectorImpl<char> &Path) const {
  if (llvm::sys::path::is_absolute(Path))
    return {};

  auto WorkingDir = getCurrentWorkingDirectory();
  if (!WorkingDir)
    return WorkingDir.getError();

  return llvm::sys::fs::make_absolute(WorkingDir.get(), Path);
}

std::error_code FileSystem::getRealPath(const Twine &Path,
                                        SmallVectorImpl<char> &Output) const {
  return errc::operation_not_permitted;
}

bool FileSystem::exists(const Twine &Path) {
  auto Status = status(Path);
  return Status && Status->exists();
}

#ifndef NDEBUG
static bool isTraversalComponent(StringRef Component) {
  return Component.equals("..") || Component.equals(".");
}

static bool pathHasTraversal(StringRef Path) {
  using namespace llvm::sys;

  for (StringRef Comp : llvm::make_range(path::begin(Path), path::end(Path)))
    if (isTraversalComponent(Comp))
      return true;
  return false;
}
#endif

//===-----------------------------------------------------------------------===/
// RealFileSystem implementation
//===-----------------------------------------------------------------------===/

namespace {

/// Wrapper around a raw file descriptor.
class RealFile : public File {
  friend class RealFileSystem;

  int FD;
  Status S;
  std::string RealName;

  RealFile(int FD, StringRef NewName, StringRef NewRealPathName)
      : FD(FD), S(NewName, {}, {}, {}, {}, {},
                  llvm::sys::fs::file_type::status_error, {}),
        RealName(NewRealPathName.str()) {
    assert(FD >= 0 && "Invalid or inactive file descriptor");
  }

public:
  ~RealFile() override;

  ErrorOr<Status> status() override;
  ErrorOr<std::string> getName() override;
  ErrorOr<std::unique_ptr<MemoryBuffer>> getBuffer(const Twine &Name,
                                                   int64_t FileSize,
                                                   bool RequiresNullTerminator,
                                                   bool IsVolatile) override;
  std::error_code close() override;
};

} // namespace

RealFile::~RealFile() { close(); }

ErrorOr<Status> RealFile::status() {
  assert(FD != -1 && "cannot stat closed file");
  if (!S.isStatusKnown()) {
    file_status RealStatus;
    if (std::error_code EC = sys::fs::status(FD, RealStatus))
      return EC;
    S = Status::copyWithNewName(RealStatus, S.getName());
  }
  return S;
}

ErrorOr<std::string> RealFile::getName() {
  return RealName.empty() ? S.getName().str() : RealName;
}

ErrorOr<std::unique_ptr<MemoryBuffer>>
RealFile::getBuffer(const Twine &Name, int64_t FileSize,
                    bool RequiresNullTerminator, bool IsVolatile) {
  assert(FD != -1 && "cannot get buffer for closed file");
  return MemoryBuffer::getOpenFile(FD, Name, FileSize, RequiresNullTerminator,
                                   IsVolatile);
}

std::error_code RealFile::close() {
  std::error_code EC = sys::Process::SafelyCloseFileDescriptor(FD);
  FD = -1;
  return EC;
}

namespace {

/// The file system according to your operating system.
class RealFileSystem : public FileSystem {
public:
  ErrorOr<Status> status(const Twine &Path) override;
  ErrorOr<std::unique_ptr<File>> openFileForRead(const Twine &Path) override;
  directory_iterator dir_begin(const Twine &Dir, std::error_code &EC) override;

  llvm::ErrorOr<std::string> getCurrentWorkingDirectory() const override;
  std::error_code setCurrentWorkingDirectory(const Twine &Path) override;
  std::error_code getRealPath(const Twine &Path,
                              SmallVectorImpl<char> &Output) const override;
};

} // namespace

ErrorOr<Status> RealFileSystem::status(const Twine &Path) {
  sys::fs::file_status RealStatus;
  if (std::error_code EC = sys::fs::status(Path, RealStatus))
    return EC;
  return Status::copyWithNewName(RealStatus, Path.str());
}

ErrorOr<std::unique_ptr<File>>
RealFileSystem::openFileForRead(const Twine &Name) {
  int FD;
  SmallString<256> RealName;
  if (std::error_code EC =
          sys::fs::openFileForRead(Name, FD, sys::fs::OF_None, &RealName))
    return EC;
  return std::unique_ptr<File>(new RealFile(FD, Name.str(), RealName.str()));
}

llvm::ErrorOr<std::string> RealFileSystem::getCurrentWorkingDirectory() const {
  SmallString<256> Dir;
  if (std::error_code EC = llvm::sys::fs::current_path(Dir))
    return EC;
  return Dir.str().str();
}

std::error_code RealFileSystem::setCurrentWorkingDirectory(const Twine &Path) {
  // FIXME: chdir is thread hostile; on the other hand, creating the same
  // behavior as chdir is complex: chdir resolves the path once, thus
  // guaranteeing that all subsequent relative path operations work
  // on the same path the original chdir resulted in. This makes a
  // difference for example on network filesystems, where symlinks might be
  // switched during runtime of the tool. Fixing this depends on having a
  // file system abstraction that allows openat() style interactions.
  return llvm::sys::fs::set_current_path(Path);
}

std::error_code
RealFileSystem::getRealPath(const Twine &Path,
                            SmallVectorImpl<char> &Output) const {
  return llvm::sys::fs::real_path(Path, Output);
}

IntrusiveRefCntPtr<FileSystem> vfs::getRealFileSystem() {
  static IntrusiveRefCntPtr<FileSystem> FS = new RealFileSystem();
  return FS;
}

namespace {

class RealFSDirIter : public clang::vfs::detail::DirIterImpl {
  llvm::sys::fs::directory_iterator Iter;

public:
  RealFSDirIter(const Twine &Path, std::error_code &EC) : Iter(Path, EC) {
    if (Iter != llvm::sys::fs::directory_iterator()) {
      llvm::sys::fs::file_status S;
      std::error_code ErrorCode = llvm::sys::fs::status(Iter->path(), S, true);
      CurrentEntry = Status::copyWithNewName(S, Iter->path());
      if (!EC)
        EC = ErrorCode;
    }
  }

  std::error_code increment() override {
    std::error_code EC;
    Iter.increment(EC);
    if (Iter == llvm::sys::fs::directory_iterator()) {
      CurrentEntry = Status();
    } else {
      llvm::sys::fs::file_status S;
      std::error_code ErrorCode = llvm::sys::fs::status(Iter->path(), S, true);
      CurrentEntry = Status::copyWithNewName(S, Iter->path());
      if (!EC)
        EC = ErrorCode;
    }
    return EC;
  }
};

} // namespace

directory_iterator RealFileSystem::dir_begin(const Twine &Dir,
                                             std::error_code &EC) {
  return directory_iterator(std::make_shared<RealFSDirIter>(Dir, EC));
}

//===-----------------------------------------------------------------------===/
// OverlayFileSystem implementation
//===-----------------------------------------------------------------------===/

OverlayFileSystem::OverlayFileSystem(IntrusiveRefCntPtr<FileSystem> BaseFS) {
  FSList.push_back(std::move(BaseFS));
}

void OverlayFileSystem::pushOverlay(IntrusiveRefCntPtr<FileSystem> FS) {
  FSList.push_back(FS);
  // Synchronize added file systems by duplicating the working directory from
  // the first one in the list.
  FS->setCurrentWorkingDirectory(getCurrentWorkingDirectory().get());
}

ErrorOr<Status> OverlayFileSystem::status(const Twine &Path) {
  // FIXME: handle symlinks that cross file systems
  for (iterator I = overlays_begin(), E = overlays_end(); I != E; ++I) {
    ErrorOr<Status> Status = (*I)->status(Path);
    if (Status || Status.getError() != llvm::errc::no_such_file_or_directory)
      return Status;
  }
  return make_error_code(llvm::errc::no_such_file_or_directory);
}

ErrorOr<std::unique_ptr<File>>
OverlayFileSystem::openFileForRead(const llvm::Twine &Path) {
  // FIXME: handle symlinks that cross file systems
  for (iterator I = overlays_begin(), E = overlays_end(); I != E; ++I) {
    auto Result = (*I)->openFileForRead(Path);
    if (Result || Result.getError() != llvm::errc::no_such_file_or_directory)
      return Result;
  }
  return make_error_code(llvm::errc::no_such_file_or_directory);
}

llvm::ErrorOr<std::string>
OverlayFileSystem::getCurrentWorkingDirectory() const {
  // All file systems are synchronized, just take the first working directory.
  return FSList.front()->getCurrentWorkingDirectory();
}

std::error_code
OverlayFileSystem::setCurrentWorkingDirectory(const Twine &Path) {
  for (auto &FS : FSList)
    if (std::error_code EC = FS->setCurrentWorkingDirectory(Path))
      return EC;
  return {};
}

std::error_code
OverlayFileSystem::getRealPath(const Twine &Path,
                               SmallVectorImpl<char> &Output) const {
  for (auto &FS : FSList)
    if (FS->exists(Path))
      return FS->getRealPath(Path, Output);
  return errc::no_such_file_or_directory;
}

clang::vfs::detail::DirIterImpl::~DirIterImpl() = default;

namespace {

class OverlayFSDirIterImpl : public clang::vfs::detail::DirIterImpl {
  OverlayFileSystem &Overlays;
  std::string Path;
  OverlayFileSystem::iterator CurrentFS;
  directory_iterator CurrentDirIter;
  llvm::StringSet<> SeenNames;

  std::error_code incrementFS() {
    assert(CurrentFS != Overlays.overlays_end() && "incrementing past end");
    ++CurrentFS;
    for (auto E = Overlays.overlays_end(); CurrentFS != E; ++CurrentFS) {
      std::error_code EC;
      CurrentDirIter = (*CurrentFS)->dir_begin(Path, EC);
      if (EC && EC != errc::no_such_file_or_directory)
        return EC;
      if (CurrentDirIter != directory_iterator())
        break; // found
    }
    return {};
  }

  std::error_code incrementDirIter(bool IsFirstTime) {
    assert((IsFirstTime || CurrentDirIter != directory_iterator()) &&
           "incrementing past end");
    std::error_code EC;
    if (!IsFirstTime)
      CurrentDirIter.increment(EC);
    if (!EC && CurrentDirIter == directory_iterator())
      EC = incrementFS();
    return EC;
  }

  std::error_code incrementImpl(bool IsFirstTime) {
    while (true) {
      std::error_code EC = incrementDirIter(IsFirstTime);
      if (EC || CurrentDirIter == directory_iterator()) {
        CurrentEntry = Status();
        return EC;
      }
      CurrentEntry = *CurrentDirIter;
      StringRef Name = llvm::sys::path::filename(CurrentEntry.getName());
      if (SeenNames.insert(Name).second)
        return EC; // name not seen before
    }
    llvm_unreachable("returned above");
  }

public:
  OverlayFSDirIterImpl(const Twine &Path, OverlayFileSystem &FS,
                       std::error_code &EC)
      : Overlays(FS), Path(Path.str()), CurrentFS(Overlays.overlays_begin()) {
    CurrentDirIter = (*CurrentFS)->dir_begin(Path, EC);
    EC = incrementImpl(true);
  }

  std::error_code increment() override { return incrementImpl(false); }
};

} // namespace

directory_iterator OverlayFileSystem::dir_begin(const Twine &Dir,
                                                std::error_code &EC) {
  return directory_iterator(
      std::make_shared<OverlayFSDirIterImpl>(Dir, *this, EC));
}

namespace clang {
namespace vfs {

namespace detail {

enum InMemoryNodeKind { IME_File, IME_Directory };

/// The in memory file system is a tree of Nodes. Every node can either be a
/// file or a directory.
class InMemoryNode {
  Status Stat;
  InMemoryNodeKind Kind;

public:
  InMemoryNode(Status Stat, InMemoryNodeKind Kind)
      : Stat(std::move(Stat)), Kind(Kind) {}
  virtual ~InMemoryNode() = default;

  const Status &getStatus() const { return Stat; }
  InMemoryNodeKind getKind() const { return Kind; }
  virtual std::string toString(unsigned Indent) const = 0;
};

namespace {

class InMemoryFile : public InMemoryNode {
  std::unique_ptr<llvm::MemoryBuffer> Buffer;

public:
  InMemoryFile(Status Stat, std::unique_ptr<llvm::MemoryBuffer> Buffer)
      : InMemoryNode(std::move(Stat), IME_File), Buffer(std::move(Buffer)) {}

  llvm::MemoryBuffer *getBuffer() { return Buffer.get(); }

  std::string toString(unsigned Indent) const override {
    return (std::string(Indent, ' ') + getStatus().getName() + "\n").str();
  }

  static bool classof(const InMemoryNode *N) {
    return N->getKind() == IME_File;
  }
};

/// Adapt a InMemoryFile for VFS' File interface.
class InMemoryFileAdaptor : public File {
  InMemoryFile &Node;

public:
  explicit InMemoryFileAdaptor(InMemoryFile &Node) : Node(Node) {}

  llvm::ErrorOr<Status> status() override { return Node.getStatus(); }

  llvm::ErrorOr<std::unique_ptr<llvm::MemoryBuffer>>
  getBuffer(const Twine &Name, int64_t FileSize, bool RequiresNullTerminator,
            bool IsVolatile) override {
    llvm::MemoryBuffer *Buf = Node.getBuffer();
    return llvm::MemoryBuffer::getMemBuffer(
        Buf->getBuffer(), Buf->getBufferIdentifier(), RequiresNullTerminator);
  }

  std::error_code close() override { return {}; }
};

} // namespace

class InMemoryDirectory : public InMemoryNode {
  std::map<std::string, std::unique_ptr<InMemoryNode>> Entries;

public:
  InMemoryDirectory(Status Stat)
      : InMemoryNode(std::move(Stat), IME_Directory) {}

  InMemoryNode *getChild(StringRef Name) {
    auto I = Entries.find(Name);
    if (I != Entries.end())
      return I->second.get();
    return nullptr;
  }

  InMemoryNode *addChild(StringRef Name, std::unique_ptr<InMemoryNode> Child) {
    return Entries.insert(make_pair(Name, std::move(Child)))
        .first->second.get();
  }

  using const_iterator = decltype(Entries)::const_iterator;

  const_iterator begin() const { return Entries.begin(); }
  const_iterator end() const { return Entries.end(); }

  std::string toString(unsigned Indent) const override {
    std::string Result =
        (std::string(Indent, ' ') + getStatus().getName() + "\n").str();
    for (const auto &Entry : Entries)
      Result += Entry.second->toString(Indent + 2);
    return Result;
  }

  static bool classof(const InMemoryNode *N) {
    return N->getKind() == IME_Directory;
  }
};

} // namespace detail

InMemoryFileSystem::InMemoryFileSystem(bool UseNormalizedPaths)
    : Root(new detail::InMemoryDirectory(
          Status("", getNextVirtualUniqueID(), llvm::sys::TimePoint<>(), 0, 0,
                 0, llvm::sys::fs::file_type::directory_file,
                 llvm::sys::fs::perms::all_all))),
      UseNormalizedPaths(UseNormalizedPaths) {}

InMemoryFileSystem::~InMemoryFileSystem() = default;

std::string InMemoryFileSystem::toString() const {
  return Root->toString(/*Indent=*/0);
}

bool InMemoryFileSystem::addFile(const Twine &P, time_t ModificationTime,
                                 std::unique_ptr<llvm::MemoryBuffer> Buffer,
                                 Optional<uint32_t> User,
                                 Optional<uint32_t> Group,
                                 Optional<llvm::sys::fs::file_type> Type,
                                 Optional<llvm::sys::fs::perms> Perms) {
  SmallString<128> Path;
  P.toVector(Path);

  // Fix up relative paths. This just prepends the current working directory.
  std::error_code EC = makeAbsolute(Path);
  assert(!EC);
  (void)EC;

  if (useNormalizedPaths())
    llvm::sys::path::remove_dots(Path, /*remove_dot_dot=*/true);

  if (Path.empty())
    return false;

  detail::InMemoryDirectory *Dir = Root.get();
  auto I = llvm::sys::path::begin(Path), E = sys::path::end(Path);
  const auto ResolvedUser = User.getValueOr(0);
  const auto ResolvedGroup = Group.getValueOr(0);
  const auto ResolvedType = Type.getValueOr(sys::fs::file_type::regular_file);
  const auto ResolvedPerms = Perms.getValueOr(sys::fs::all_all);
  // Any intermediate directories we create should be accessible by
  // the owner, even if Perms says otherwise for the final path.
  const auto NewDirectoryPerms = ResolvedPerms | sys::fs::owner_all;
  while (true) {
    StringRef Name = *I;
    detail::InMemoryNode *Node = Dir->getChild(Name);
    ++I;
    if (!Node) {
      if (I == E) {
        // End of the path, create a new file or directory.
        Status Stat(P.str(), getNextVirtualUniqueID(),
                    llvm::sys::toTimePoint(ModificationTime), ResolvedUser,
                    ResolvedGroup, Buffer->getBufferSize(), ResolvedType,
                    ResolvedPerms);
        std::unique_ptr<detail::InMemoryNode> Child;
        if (ResolvedType == sys::fs::file_type::directory_file) {
          Child.reset(new detail::InMemoryDirectory(std::move(Stat)));
        } else {
          Child.reset(new detail::InMemoryFile(std::move(Stat),
                                               std::move(Buffer)));
        }
        Dir->addChild(Name, std::move(Child));
        return true;
      }

      // Create a new directory. Use the path up to here.
      Status Stat(
          StringRef(Path.str().begin(), Name.end() - Path.str().begin()),
          getNextVirtualUniqueID(), llvm::sys::toTimePoint(ModificationTime),
          ResolvedUser, ResolvedGroup, Buffer->getBufferSize(),
          sys::fs::file_type::directory_file, NewDirectoryPerms);
      Dir = cast<detail::InMemoryDirectory>(Dir->addChild(
          Name, llvm::make_unique<detail::InMemoryDirectory>(std::move(Stat))));
      continue;
    }

    if (auto *NewDir = dyn_cast<detail::InMemoryDirectory>(Node)) {
      Dir = NewDir;
    } else {
      assert(isa<detail::InMemoryFile>(Node) &&
             "Must be either file or directory!");

      // Trying to insert a directory in place of a file.
      if (I != E)
        return false;

      // Return false only if the new file is different from the existing one.
      return cast<detail::InMemoryFile>(Node)->getBuffer()->getBuffer() ==
             Buffer->getBuffer();
    }
  }
}

bool InMemoryFileSystem::addFileNoOwn(const Twine &P, time_t ModificationTime,
                                      llvm::MemoryBuffer *Buffer,
                                      Optional<uint32_t> User,
                                      Optional<uint32_t> Group,
                                      Optional<llvm::sys::fs::file_type> Type,
                                      Optional<llvm::sys::fs::perms> Perms) {
  return addFile(P, ModificationTime,
                 llvm::MemoryBuffer::getMemBuffer(
                     Buffer->getBuffer(), Buffer->getBufferIdentifier()),
                 std::move(User), std::move(Group), std::move(Type),
                 std::move(Perms));
}

static ErrorOr<detail::InMemoryNode *>
lookupInMemoryNode(const InMemoryFileSystem &FS, detail::InMemoryDirectory *Dir,
                   const Twine &P) {
  SmallString<128> Path;
  P.toVector(Path);

  // Fix up relative paths. This just prepends the current working directory.
  std::error_code EC = FS.makeAbsolute(Path);
  assert(!EC);
  (void)EC;

  if (FS.useNormalizedPaths())
    llvm::sys::path::remove_dots(Path, /*remove_dot_dot=*/true);

  if (Path.empty())
    return Dir;

  auto I = llvm::sys::path::begin(Path), E = llvm::sys::path::end(Path);
  while (true) {
    detail::InMemoryNode *Node = Dir->getChild(*I);
    ++I;
    if (!Node)
      return errc::no_such_file_or_directory;

    // Return the file if it's at the end of the path.
    if (auto File = dyn_cast<detail::InMemoryFile>(Node)) {
      if (I == E)
        return File;
      return errc::no_such_file_or_directory;
    }

    // Traverse directories.
    Dir = cast<detail::InMemoryDirectory>(Node);
    if (I == E)
      return Dir;
  }
}

llvm::ErrorOr<Status> InMemoryFileSystem::status(const Twine &Path) {
  auto Node = lookupInMemoryNode(*this, Root.get(), Path);
  if (Node)
    return (*Node)->getStatus();
  return Node.getError();
}

llvm::ErrorOr<std::unique_ptr<File>>
InMemoryFileSystem::openFileForRead(const Twine &Path) {
  auto Node = lookupInMemoryNode(*this, Root.get(), Path);
  if (!Node)
    return Node.getError();

  // When we have a file provide a heap-allocated wrapper for the memory buffer
  // to match the ownership semantics for File.
  if (auto *F = dyn_cast<detail::InMemoryFile>(*Node))
    return std::unique_ptr<File>(new detail::InMemoryFileAdaptor(*F));

  // FIXME: errc::not_a_file?
  return make_error_code(llvm::errc::invalid_argument);
}

namespace {

/// Adaptor from InMemoryDir::iterator to directory_iterator.
class InMemoryDirIterator : public clang::vfs::detail::DirIterImpl {
  detail::InMemoryDirectory::const_iterator I;
  detail::InMemoryDirectory::const_iterator E;

public:
  InMemoryDirIterator() = default;

  explicit InMemoryDirIterator(detail::InMemoryDirectory &Dir)
      : I(Dir.begin()), E(Dir.end()) {
    if (I != E)
      CurrentEntry = I->second->getStatus();
  }

  std::error_code increment() override {
    ++I;
    // When we're at the end, make CurrentEntry invalid and DirIterImpl will do
    // the rest.
    CurrentEntry = I != E ? I->second->getStatus() : Status();
    return {};
  }
};

} // namespace

directory_iterator InMemoryFileSystem::dir_begin(const Twine &Dir,
                                                 std::error_code &EC) {
  auto Node = lookupInMemoryNode(*this, Root.get(), Dir);
  if (!Node) {
    EC = Node.getError();
    return directory_iterator(std::make_shared<InMemoryDirIterator>());
  }

  if (auto *DirNode = dyn_cast<detail::InMemoryDirectory>(*Node))
    return directory_iterator(std::make_shared<InMemoryDirIterator>(*DirNode));

  EC = make_error_code(llvm::errc::not_a_directory);
  return directory_iterator(std::make_shared<InMemoryDirIterator>());
}

std::error_code InMemoryFileSystem::setCurrentWorkingDirectory(const Twine &P) {
  SmallString<128> Path;
  P.toVector(Path);

  // Fix up relative paths. This just prepends the current working directory.
  std::error_code EC = makeAbsolute(Path);
  assert(!EC);
  (void)EC;

  if (useNormalizedPaths())
    llvm::sys::path::remove_dots(Path, /*remove_dot_dot=*/true);

  if (!Path.empty())
    WorkingDirectory = Path.str();
  return {};
}

std::error_code
InMemoryFileSystem::getRealPath(const Twine &Path,
                                SmallVectorImpl<char> &Output) const {
  auto CWD = getCurrentWorkingDirectory();
  if (!CWD || CWD->empty())
    return errc::operation_not_permitted;
  Path.toVector(Output);
  if (auto EC = makeAbsolute(Output))
    return EC;
  llvm::sys::path::remove_dots(Output, /*remove_dot_dot=*/true);
  return {};
}

} // namespace vfs
} // namespace clang

//===-----------------------------------------------------------------------===/
// RedirectingFileSystem implementation
//===-----------------------------------------------------------------------===/

namespace {

enum EntryKind {
  EK_Directory,
  EK_File
};

/// A single file or directory in the VFS.
class Entry {
  EntryKind Kind;
  std::string Name;

public:
  Entry(EntryKind K, StringRef Name) : Kind(K), Name(Name) {}
  virtual ~Entry() = default;

  StringRef getName() const { return Name; }
  EntryKind getKind() const { return Kind; }
};

class RedirectingDirectoryEntry : public Entry {
  std::vector<std::unique_ptr<Entry>> Contents;
  Status S;

public:
  RedirectingDirectoryEntry(StringRef Name,
                            std::vector<std::unique_ptr<Entry>> Contents,
                            Status S)
      : Entry(EK_Directory, Name), Contents(std::move(Contents)),
        S(std::move(S)) {}
  RedirectingDirectoryEntry(StringRef Name, Status S)
      : Entry(EK_Directory, Name), S(std::move(S)) {}

  Status getStatus() { return S; }

  void addContent(std::unique_ptr<Entry> Content) {
    Contents.push_back(std::move(Content));
  }

  Entry *getLastContent() const { return Contents.back().get(); }

  using iterator = decltype(Contents)::iterator;

  iterator contents_begin() { return Contents.begin(); }
  iterator contents_end() { return Contents.end(); }

  static bool classof(const Entry *E) { return E->getKind() == EK_Directory; }
};

class RedirectingFileEntry : public Entry {
public:
  enum NameKind {
    NK_NotSet,
    NK_External,
    NK_Virtual
  };

private:
  std::string ExternalContentsPath;
  NameKind UseName;

public:
  RedirectingFileEntry(StringRef Name, StringRef ExternalContentsPath,
                       NameKind UseName)
      : Entry(EK_File, Name), ExternalContentsPath(ExternalContentsPath),
        UseName(UseName) {}

  StringRef getExternalContentsPath() const { return ExternalContentsPath; }

  /// whether to use the external path as the name for this file.
  bool useExternalName(bool GlobalUseExternalName) const {
    return UseName == NK_NotSet ? GlobalUseExternalName
                                : (UseName == NK_External);
  }

  NameKind getUseName() const { return UseName; }

  static bool classof(const Entry *E) { return E->getKind() == EK_File; }
};

class RedirectingFileSystem;

// FIXME: reuse implementation common with OverlayFSDirIterImpl as these
// iterators are conceptually similar.
class VFSFromYamlDirIterImpl : public clang::vfs::detail::DirIterImpl {
  std::string Dir;
  RedirectingFileSystem &FS;
  RedirectingDirectoryEntry::iterator Current, End;

  // To handle 'fallthrough' mode we need to iterate at first through
  // RedirectingDirectoryEntry and then through ExternalFS. These operations are
  // done sequentially, we just need to keep a track of what kind of iteration
  // we are currently performing.

  /// Flag telling if we should iterate through ExternalFS or stop at the last
  /// RedirectingDirectoryEntry::iterator.
  bool IterateExternalFS;
  /// Flag telling if we have switched to iterating through ExternalFS.
  bool IsExternalFSCurrent = false;
  FileSystem &ExternalFS;
  directory_iterator ExternalDirIter;
  llvm::StringSet<> SeenNames;

  /// To combine multiple iterations, different methods are responsible for
  /// different iteration steps.
  /// @{

  /// Responsible for dispatching between RedirectingDirectoryEntry iteration
  /// and ExternalFS iteration.
  std::error_code incrementImpl(bool IsFirstTime);
  /// Responsible for RedirectingDirectoryEntry iteration.
  std::error_code incrementContent(bool IsFirstTime);
  /// Responsible for ExternalFS iteration.
  std::error_code incrementExternal();
  /// @}

public:
  VFSFromYamlDirIterImpl(const Twine &Path, RedirectingFileSystem &FS,
                         RedirectingDirectoryEntry::iterator Begin,
                         RedirectingDirectoryEntry::iterator End,
                         bool IterateExternalFS, FileSystem &ExternalFS,
                         std::error_code &EC);

  std::error_code increment() override;
};

/// A virtual file system parsed from a YAML file.
///
/// Currently, this class allows creating virtual directories and mapping
/// virtual file paths to existing external files, available in \c ExternalFS.
///
/// The basic structure of the parsed file is:
/// \verbatim
/// {
///   'version': <version number>,
///   <optional configuration>
///   'roots': [
///              <directory entries>
///            ]
/// }
/// \endverbatim
///
/// All configuration options are optional.
///   'case-sensitive': <boolean, default=true>
///   'use-external-names': <boolean, default=true>
///   'overlay-relative': <boolean, default=false>
///   'ignore-non-existent-contents': <boolean, default=true>
///   'fallthrough': <boolean, default=true>
///
/// Virtual directories are represented as
/// \verbatim
/// {
///   'type': 'directory',
///   'name': <string>,
///   'contents': [ <file or directory entries> ]
/// }
/// \endverbatim
///
/// The default attributes for virtual directories are:
/// \verbatim
/// MTime = now() when created
/// Perms = 0777
/// User = Group = 0
/// Size = 0
/// UniqueID = unspecified unique value
/// \endverbatim
///
/// Re-mapped files are represented as
/// \verbatim
/// {
///   'type': 'file',
///   'name': <string>,
///   'use-external-name': <boolean> # Optional
///   'external-contents': <path to external file>)
/// }
/// \endverbatim
///
/// and inherit their attributes from the external contents.
///
/// In both cases, the 'name' field may contain multiple path components (e.g.
/// /path/to/file). However, any directory that contains more than one child
/// must be uniquely represented by a directory entry.
class RedirectingFileSystem : public vfs::FileSystem {
  friend class RedirectingFileSystemParser;

  /// The root(s) of the virtual file system.
  std::vector<std::unique_ptr<Entry>> Roots;

  /// The file system to use for external references.
  IntrusiveRefCntPtr<FileSystem> ExternalFS;

  /// If IsRelativeOverlay is set, this represents the directory
  /// path that should be prefixed to each 'external-contents' entry
  /// when reading from YAML files.
  std::string ExternalContentsPrefixDir;

  /// @name Configuration
  /// @{

  /// Whether to perform case-sensitive comparisons.
  ///
  /// Currently, case-insensitive matching only works correctly with ASCII.
  bool CaseSensitive = true;

  /// IsRelativeOverlay marks whether a IsExternalContentsPrefixDir path must
  /// be prefixed in every 'external-contents' when reading from YAML files.
  bool IsRelativeOverlay = false;

  /// Whether to use to use the value of 'external-contents' for the
  /// names of files.  This global value is overridable on a per-file basis.
  bool UseExternalNames = true;

  /// Whether an invalid path obtained via 'external-contents' should
  /// cause iteration on the VFS to stop. If 'true', the VFS should ignore
  /// the entry and continue with the next. Allows YAML files to be shared
  /// across multiple compiler invocations regardless of prior existent
  /// paths in 'external-contents'. This global value is overridable on a
  /// per-file basis.
  bool IgnoreNonExistentContents = true;

  /// Whether to attempt a file lookup in external file system after it wasn't
  /// found in VFS.
  bool IsFallthrough = true;
  /// @}

  /// Virtual file paths and external files could be canonicalized without "..",
  /// "." and "./" in their paths. FIXME: some unittests currently fail on
  /// win32 when using remove_dots and remove_leading_dotslash on paths.
  bool UseCanonicalizedPaths =
#ifdef _WIN32
      false;
#else
      true;
#endif

private:
  RedirectingFileSystem(IntrusiveRefCntPtr<FileSystem> ExternalFS)
      : ExternalFS(std::move(ExternalFS)) {}

  /// Looks up the path <tt>[Start, End)</tt> in \p From, possibly
  /// recursing into the contents of \p From if it is a directory.
  ErrorOr<Entry *> lookupPath(sys::path::const_iterator Start,
                              sys::path::const_iterator End, Entry *From) const;

  /// Get the status of a given an \c Entry.
  ErrorOr<Status> status(const Twine &Path, Entry *E);
  ErrorOr<Status> status(const Twine &Path, bool ShouldCheckExternalFS);

public:
  /// Looks up \p Path in \c Roots.
  ErrorOr<Entry *> lookupPath(const Twine &Path) const;
  /// Allows directory iterator to iterate only through the virtual paths
  /// without falling back to external file system.
  ErrorOr<Status> statusVirtualOnly(const Twine &Path);

  /// Parses \p Buffer, which is expected to be in YAML format and
  /// returns a virtual file system representing its contents.
  static RedirectingFileSystem *
  create(std::unique_ptr<MemoryBuffer> Buffer,
         SourceMgr::DiagHandlerTy DiagHandler, StringRef YAMLFilePath,
         void *DiagContext, IntrusiveRefCntPtr<FileSystem> ExternalFS);

  ErrorOr<Status> status(const Twine &Path) override;
  ErrorOr<std::unique_ptr<File>> openFileForRead(const Twine &Path) override;

  std::error_code getRealPath(const Twine &Path,
                              SmallVectorImpl<char> &Output) const override;

  llvm::ErrorOr<std::string> getCurrentWorkingDirectory() const override {
    return ExternalFS->getCurrentWorkingDirectory();
  }

  std::error_code setCurrentWorkingDirectory(const Twine &Path) override {
    return ExternalFS->setCurrentWorkingDirectory(Path);
  }

  directory_iterator dir_begin(const Twine &Dir, std::error_code &EC) override {
    ErrorOr<Entry *> E = lookupPath(Dir);
    if (!E) {
      EC = E.getError();
      if (IsFallthrough && EC == errc::no_such_file_or_directory)
        return ExternalFS->dir_begin(Dir, EC);
      return {};
    }
    ErrorOr<Status> S = status(Dir, *E);
    if (!S) {
      EC = S.getError();
      return {};
    }
    if (!S->isDirectory()) {
      EC = std::error_code(static_cast<int>(errc::not_a_directory),
                           std::system_category());
      return {};
    }

    auto *D = cast<RedirectingDirectoryEntry>(*E);
    return directory_iterator(std::make_shared<VFSFromYamlDirIterImpl>(
        Dir, *this, D->contents_begin(), D->contents_end(),
        /*IterateExternalFS=*/IsFallthrough, *ExternalFS, EC));
  }

  void setExternalContentsPrefixDir(StringRef PrefixDir) {
    ExternalContentsPrefixDir = PrefixDir.str();
  }

  StringRef getExternalContentsPrefixDir() const {
    return ExternalContentsPrefixDir;
  }

  bool ignoreNonExistentContents() const {
    return IgnoreNonExistentContents;
  }

#if !defined(NDEBUG) || defined(LLVM_ENABLE_DUMP)
LLVM_DUMP_METHOD void dump() const {
    for (const auto &Root : Roots)
      dumpEntry(Root.get());
  }

LLVM_DUMP_METHOD void dumpEntry(Entry *E, int NumSpaces = 0) const {
    StringRef Name = E->getName();
    for (int i = 0, e = NumSpaces; i < e; ++i)
      dbgs() << " ";
    dbgs() << "'" << Name.str().c_str() << "'" << "\n";

    if (E->getKind() == EK_Directory) {
      auto *DE = dyn_cast<RedirectingDirectoryEntry>(E);
      assert(DE && "Should be a directory");

      for (std::unique_ptr<Entry> &SubEntry :
           llvm::make_range(DE->contents_begin(), DE->contents_end()))
        dumpEntry(SubEntry.get(), NumSpaces+2);
    }
  }
#endif
};

/// A helper class to hold the common YAML parsing state.
class RedirectingFileSystemParser {
  yaml::Stream &Stream;

  void error(yaml::Node *N, const Twine &Msg) {
    Stream.printError(N, Msg);
  }

  // false on error
  bool parseScalarString(yaml::Node *N, StringRef &Result,
                         SmallVectorImpl<char> &Storage) {
    const auto *S = dyn_cast<yaml::ScalarNode>(N);

    if (!S) {
      error(N, "expected string");
      return false;
    }
    Result = S->getValue(Storage);
    return true;
  }

  // false on error
  bool parseScalarBool(yaml::Node *N, bool &Result) {
    SmallString<5> Storage;
    StringRef Value;
    if (!parseScalarString(N, Value, Storage))
      return false;

    if (Value.equals_lower("true") || Value.equals_lower("on") ||
        Value.equals_lower("yes") || Value == "1") {
      Result = true;
      return true;
    } else if (Value.equals_lower("false") || Value.equals_lower("off") ||
               Value.equals_lower("no") || Value == "0") {
      Result = false;
      return true;
    }

    error(N, "expected boolean value");
    return false;
  }

  struct KeyStatus {
    bool Required;
    bool Seen = false;

    KeyStatus(bool Required = false) : Required(Required) {}
  };

  using KeyStatusPair = std::pair<StringRef, KeyStatus>;

  // false on error
  bool checkDuplicateOrUnknownKey(yaml::Node *KeyNode, StringRef Key,
                                  DenseMap<StringRef, KeyStatus> &Keys) {
    if (!Keys.count(Key)) {
      error(KeyNode, "unknown key");
      return false;
    }
    KeyStatus &S = Keys[Key];
    if (S.Seen) {
      error(KeyNode, Twine("duplicate key '") + Key + "'");
      return false;
    }
    S.Seen = true;
    return true;
  }

  // false on error
  bool checkMissingKeys(yaml::Node *Obj, DenseMap<StringRef, KeyStatus> &Keys) {
    for (const auto &I : Keys) {
      if (I.second.Required && !I.second.Seen) {
        error(Obj, Twine("missing key '") + I.first + "'");
        return false;
      }
    }
    return true;
  }

  Entry *lookupOrCreateEntry(RedirectingFileSystem *FS, StringRef Name,
                             Entry *ParentEntry = nullptr) {
    if (!ParentEntry) { // Look for a existent root
      for (const auto &Root : FS->Roots) {
        if (Name.equals(Root->getName())) {
          ParentEntry = Root.get();
          return ParentEntry;
        }
      }
    } else { // Advance to the next component
      auto *DE = dyn_cast<RedirectingDirectoryEntry>(ParentEntry);
      for (std::unique_ptr<Entry> &Content :
           llvm::make_range(DE->contents_begin(), DE->contents_end())) {
        auto *DirContent = dyn_cast<RedirectingDirectoryEntry>(Content.get());
        if (DirContent && Name.equals(Content->getName()))
          return DirContent;
      }
    }

    // ... or create a new one
    std::unique_ptr<Entry> E = llvm::make_unique<RedirectingDirectoryEntry>(
        Name,
        Status("", getNextVirtualUniqueID(), std::chrono::system_clock::now(),
               0, 0, 0, file_type::directory_file, sys::fs::all_all));

    if (!ParentEntry) { // Add a new root to the overlay
      FS->Roots.push_back(std::move(E));
      ParentEntry = FS->Roots.back().get();
      return ParentEntry;
    }

    auto *DE = dyn_cast<RedirectingDirectoryEntry>(ParentEntry);
    DE->addContent(std::move(E));
    return DE->getLastContent();
  }

  void uniqueOverlayTree(RedirectingFileSystem *FS, Entry *SrcE,
                         Entry *NewParentE = nullptr) {
    StringRef Name = SrcE->getName();
    switch (SrcE->getKind()) {
    case EK_Directory: {
      auto *DE = dyn_cast<RedirectingDirectoryEntry>(SrcE);
      assert(DE && "Must be a directory");
      // Empty directories could be present in the YAML as a way to
      // describe a file for a current directory after some of its subdir
      // is parsed. This only leads to redundant walks, ignore it.
      if (!Name.empty())
        NewParentE = lookupOrCreateEntry(FS, Name, NewParentE);
      for (std::unique_ptr<Entry> &SubEntry :
           llvm::make_range(DE->contents_begin(), DE->contents_end()))
        uniqueOverlayTree(FS, SubEntry.get(), NewParentE);
      break;
    }
    case EK_File: {
      auto *FE = dyn_cast<RedirectingFileEntry>(SrcE);
      assert(FE && "Must be a file");
      assert(NewParentE && "Parent entry must exist");
      auto *DE = dyn_cast<RedirectingDirectoryEntry>(NewParentE);
      DE->addContent(llvm::make_unique<RedirectingFileEntry>(
          Name, FE->getExternalContentsPath(), FE->getUseName()));
      break;
    }
    }
  }

  std::unique_ptr<Entry> parseEntry(yaml::Node *N, RedirectingFileSystem *FS,
                                    bool IsRootEntry) {
    auto *M = dyn_cast<yaml::MappingNode>(N);
    if (!M) {
      error(N, "expected mapping node for file or directory entry");
      return nullptr;
    }

    KeyStatusPair Fields[] = {
      KeyStatusPair("name", true),
      KeyStatusPair("type", true),
      KeyStatusPair("contents", false),
      KeyStatusPair("external-contents", false),
      KeyStatusPair("use-external-name", false),
    };

    DenseMap<StringRef, KeyStatus> Keys(std::begin(Fields), std::end(Fields));

    bool HasContents = false; // external or otherwise
    std::vector<std::unique_ptr<Entry>> EntryArrayContents;
    std::string ExternalContentsPath;
    std::string Name;
    yaml::Node *NameValueNode;
    auto UseExternalName = RedirectingFileEntry::NK_NotSet;
    EntryKind Kind;

    for (auto &I : *M) {
      StringRef Key;
      // Reuse the buffer for key and value, since we don't look at key after
      // parsing value.
      SmallString<256> Buffer;
      if (!parseScalarString(I.getKey(), Key, Buffer))
        return nullptr;

      if (!checkDuplicateOrUnknownKey(I.getKey(), Key, Keys))
        return nullptr;

      StringRef Value;
      if (Key == "name") {
        if (!parseScalarString(I.getValue(), Value, Buffer))
          return nullptr;

        NameValueNode = I.getValue();
        if (FS->UseCanonicalizedPaths) {
          SmallString<256> Path(Value);
          // Guarantee that old YAML files containing paths with ".." and "."
          // are properly canonicalized before read into the VFS.
          Path = sys::path::remove_leading_dotslash(Path);
          sys::path::remove_dots(Path, /*remove_dot_dot=*/true);
          Name = Path.str();
        } else {
          Name = Value;
        }
      } else if (Key == "type") {
        if (!parseScalarString(I.getValue(), Value, Buffer))
          return nullptr;
        if (Value == "file")
          Kind = EK_File;
        else if (Value == "directory")
          Kind = EK_Directory;
        else {
          error(I.getValue(), "unknown value for 'type'");
          return nullptr;
        }
      } else if (Key == "contents") {
        if (HasContents) {
          error(I.getKey(),
                "entry already has 'contents' or 'external-contents'");
          return nullptr;
        }
        HasContents = true;
        auto *Contents = dyn_cast<yaml::SequenceNode>(I.getValue());
        if (!Contents) {
          // FIXME: this is only for directories, what about files?
          error(I.getValue(), "expected array");
          return nullptr;
        }

        for (auto &I : *Contents) {
          if (std::unique_ptr<Entry> E =
                  parseEntry(&I, FS, /*IsRootEntry*/ false))
            EntryArrayContents.push_back(std::move(E));
          else
            return nullptr;
        }
      } else if (Key == "external-contents") {
        if (HasContents) {
          error(I.getKey(),
                "entry already has 'contents' or 'external-contents'");
          return nullptr;
        }
        HasContents = true;
        if (!parseScalarString(I.getValue(), Value, Buffer))
          return nullptr;

        SmallString<256> FullPath;
        if (FS->IsRelativeOverlay) {
          FullPath = FS->getExternalContentsPrefixDir();
          assert(!FullPath.empty() &&
                 "External contents prefix directory must exist");
          llvm::sys::path::append(FullPath, Value);
        } else {
          FullPath = Value;
        }

        if (FS->UseCanonicalizedPaths) {
          // Guarantee that old YAML files containing paths with ".." and "."
          // are properly canonicalized before read into the VFS.
          FullPath = sys::path::remove_leading_dotslash(FullPath);
          sys::path::remove_dots(FullPath, /*remove_dot_dot=*/true);
        }
        ExternalContentsPath = FullPath.str();
      } else if (Key == "use-external-name") {
        bool Val;
        if (!parseScalarBool(I.getValue(), Val))
          return nullptr;
        UseExternalName = Val ? RedirectingFileEntry::NK_External
                              : RedirectingFileEntry::NK_Virtual;
      } else {
        llvm_unreachable("key missing from Keys");
      }
    }

    if (Stream.failed())
      return nullptr;

    // check for missing keys
    if (!HasContents) {
      error(N, "missing key 'contents' or 'external-contents'");
      return nullptr;
    }
    if (!checkMissingKeys(N, Keys))
      return nullptr;

    // check invalid configuration
    if (Kind == EK_Directory &&
        UseExternalName != RedirectingFileEntry::NK_NotSet) {
      error(N, "'use-external-name' is not supported for directories");
      return nullptr;
    }

    if (IsRootEntry && !sys::path::is_absolute(Name)) {
      assert(NameValueNode && "Name presence should be checked earlier");
      error(NameValueNode,
            "entry with relative path at the root level is not discoverable");
      return nullptr;
    }

    // Remove trailing slash(es), being careful not to remove the root path
    StringRef Trimmed(Name);
    size_t RootPathLen = sys::path::root_path(Trimmed).size();
    while (Trimmed.size() > RootPathLen &&
           sys::path::is_separator(Trimmed.back()))
      Trimmed = Trimmed.slice(0, Trimmed.size()-1);
    // Get the last component
    StringRef LastComponent = sys::path::filename(Trimmed);

    std::unique_ptr<Entry> Result;
    switch (Kind) {
    case EK_File:
      Result = llvm::make_unique<RedirectingFileEntry>(
          LastComponent, std::move(ExternalContentsPath), UseExternalName);
      break;
    case EK_Directory:
      Result = llvm::make_unique<RedirectingDirectoryEntry>(
          LastComponent, std::move(EntryArrayContents),
          Status("", getNextVirtualUniqueID(), std::chrono::system_clock::now(),
                 0, 0, 0, file_type::directory_file, sys::fs::all_all));
      break;
    }

    StringRef Parent = sys::path::parent_path(Trimmed);
    if (Parent.empty())
      return Result;

    // if 'name' contains multiple components, create implicit directory entries
    for (sys::path::reverse_iterator I = sys::path::rbegin(Parent),
                                     E = sys::path::rend(Parent);
         I != E; ++I) {
      std::vector<std::unique_ptr<Entry>> Entries;
      Entries.push_back(std::move(Result));
      Result = llvm::make_unique<RedirectingDirectoryEntry>(
          *I, std::move(Entries),
          Status("", getNextVirtualUniqueID(), std::chrono::system_clock::now(),
                 0, 0, 0, file_type::directory_file, sys::fs::all_all));
    }
    return Result;
  }

public:
  RedirectingFileSystemParser(yaml::Stream &S) : Stream(S) {}

  // false on error
  bool parse(yaml::Node *Root, RedirectingFileSystem *FS) {
    auto *Top = dyn_cast<yaml::MappingNode>(Root);
    if (!Top) {
      error(Root, "expected mapping node");
      return false;
    }

    KeyStatusPair Fields[] = {
      KeyStatusPair("version", true),
      KeyStatusPair("case-sensitive", false),
      KeyStatusPair("use-external-names", false),
      KeyStatusPair("overlay-relative", false),
      KeyStatusPair("ignore-non-existent-contents", false),
      KeyStatusPair("fallthrough", false),
      KeyStatusPair("roots", true),
    };

    DenseMap<StringRef, KeyStatus> Keys(std::begin(Fields), std::end(Fields));
    std::vector<std::unique_ptr<Entry>> RootEntries;

    // Parse configuration and 'roots'
    for (auto &I : *Top) {
      SmallString<10> KeyBuffer;
      StringRef Key;
      if (!parseScalarString(I.getKey(), Key, KeyBuffer))
        return false;

      if (!checkDuplicateOrUnknownKey(I.getKey(), Key, Keys))
        return false;

      if (Key == "roots") {
        auto *Roots = dyn_cast<yaml::SequenceNode>(I.getValue());
        if (!Roots) {
          error(I.getValue(), "expected array");
          return false;
        }

        for (auto &I : *Roots) {
          if (std::unique_ptr<Entry> E =
                  parseEntry(&I, FS, /*IsRootEntry*/ true))
            RootEntries.push_back(std::move(E));
          else
            return false;
        }
      } else if (Key == "version") {
        StringRef VersionString;
        SmallString<4> Storage;
        if (!parseScalarString(I.getValue(), VersionString, Storage))
          return false;
        int Version;
        if (VersionString.getAsInteger<int>(10, Version)) {
          error(I.getValue(), "expected integer");
          return false;
        }
        if (Version < 0) {
          error(I.getValue(), "invalid version number");
          return false;
        }
        if (Version != 0) {
          error(I.getValue(), "version mismatch, expected 0");
          return false;
        }
      } else if (Key == "case-sensitive") {
        if (!parseScalarBool(I.getValue(), FS->CaseSensitive))
          return false;
      } else if (Key == "overlay-relative") {
        if (!parseScalarBool(I.getValue(), FS->IsRelativeOverlay))
          return false;
      } else if (Key == "use-external-names") {
        if (!parseScalarBool(I.getValue(), FS->UseExternalNames))
          return false;
      } else if (Key == "ignore-non-existent-contents") {
        if (!parseScalarBool(I.getValue(), FS->IgnoreNonExistentContents))
          return false;
      } else if (Key == "fallthrough") {
        if (!parseScalarBool(I.getValue(), FS->IsFallthrough))
          return false;
      } else {
        llvm_unreachable("key missing from Keys");
      }
    }

    if (Stream.failed())
      return false;

    if (!checkMissingKeys(Top, Keys))
      return false;

    // Now that we sucessefully parsed the YAML file, canonicalize the internal
    // representation to a proper directory tree so that we can search faster
    // inside the VFS.
    for (auto &E : RootEntries)
      uniqueOverlayTree(FS, E.get());

    return true;
  }
};

} // namespace

RedirectingFileSystem *
RedirectingFileSystem::create(std::unique_ptr<MemoryBuffer> Buffer,
                              SourceMgr::DiagHandlerTy DiagHandler,
                              StringRef YAMLFilePath, void *DiagContext,
                              IntrusiveRefCntPtr<FileSystem> ExternalFS) {
  SourceMgr SM;
  yaml::Stream Stream(Buffer->getMemBufferRef(), SM);

  SM.setDiagHandler(DiagHandler, DiagContext);
  yaml::document_iterator DI = Stream.begin();
  yaml::Node *Root = DI->getRoot();
  if (DI == Stream.end() || !Root) {
    SM.PrintMessage(SMLoc(), SourceMgr::DK_Error, "expected root node");
    return nullptr;
  }

  RedirectingFileSystemParser P(Stream);

  std::unique_ptr<RedirectingFileSystem> FS(
      new RedirectingFileSystem(std::move(ExternalFS)));

  if (!YAMLFilePath.empty()) {
    // Use the YAML path from -ivfsoverlay to compute the dir to be prefixed
    // to each 'external-contents' path.
    //
    // Example:
    //    -ivfsoverlay dummy.cache/vfs/vfs.yaml
    // yields:
    //  FS->ExternalContentsPrefixDir => /<absolute_path_to>/dummy.cache/vfs
    //
    SmallString<256> OverlayAbsDir = sys::path::parent_path(YAMLFilePath);
    std::error_code EC = llvm::sys::fs::make_absolute(OverlayAbsDir);
    assert(!EC && "Overlay dir final path must be absolute");
    (void)EC;
    FS->setExternalContentsPrefixDir(OverlayAbsDir);
  }

  if (!P.parse(Root, FS.get()))
    return nullptr;

  return FS.release();
}

ErrorOr<Entry *> RedirectingFileSystem::lookupPath(const Twine &Path_) const {
  SmallString<256> Path;
  Path_.toVector(Path);

  // Handle relative paths
  if (std::error_code EC = makeAbsolute(Path))
    return EC;

  // Canonicalize path by removing ".", "..", "./", etc components. This is
  // a VFS request, do bot bother about symlinks in the path components
  // but canonicalize in order to perform the correct entry search.
  if (UseCanonicalizedPaths) {
    Path = sys::path::remove_leading_dotslash(Path);
    sys::path::remove_dots(Path, /*remove_dot_dot=*/true);
  }

  if (Path.empty())
    return make_error_code(llvm::errc::invalid_argument);

  sys::path::const_iterator Start = sys::path::begin(Path);
  sys::path::const_iterator End = sys::path::end(Path);
  for (const auto &Root : Roots) {
    ErrorOr<Entry *> Result = lookupPath(Start, End, Root.get());
    if (Result || Result.getError() != llvm::errc::no_such_file_or_directory)
      return Result;
  }
  return make_error_code(llvm::errc::no_such_file_or_directory);
}

ErrorOr<Entry *>
RedirectingFileSystem::lookupPath(sys::path::const_iterator Start,
                                  sys::path::const_iterator End,
                                  Entry *From) const {
#ifndef _WIN32
  assert(!isTraversalComponent(*Start) &&
         !isTraversalComponent(From->getName()) &&
         "Paths should not contain traversal components");
#else
  // FIXME: this is here to support windows, remove it once canonicalized
  // paths become globally default.
  if (Start->equals("."))
    ++Start;
#endif

  StringRef FromName = From->getName();

  // Forward the search to the next component in case this is an empty one.
  if (!FromName.empty()) {
    if (CaseSensitive ? !Start->equals(FromName)
                      : !Start->equals_lower(FromName))
      // failure to match
      return make_error_code(llvm::errc::no_such_file_or_directory);

    ++Start;

    if (Start == End) {
      // Match!
      return From;
    }
  }

  auto *DE = dyn_cast<RedirectingDirectoryEntry>(From);
  if (!DE)
    return make_error_code(llvm::errc::not_a_directory);

  for (const std::unique_ptr<Entry> &DirEntry :
       llvm::make_range(DE->contents_begin(), DE->contents_end())) {
    ErrorOr<Entry *> Result = lookupPath(Start, End, DirEntry.get());
    if (Result || Result.getError() != llvm::errc::no_such_file_or_directory)
      return Result;
  }
  return make_error_code(llvm::errc::no_such_file_or_directory);
}

static Status getRedirectedFileStatus(const Twine &Path, bool UseExternalNames,
                                      Status ExternalStatus) {
  Status S = ExternalStatus;
  if (!UseExternalNames)
    S = Status::copyWithNewName(S, Path.str());
  S.IsVFSMapped = true;
  return S;
}

ErrorOr<Status> RedirectingFileSystem::status(const Twine &Path, Entry *E) {
  assert(E != nullptr);
  if (auto *F = dyn_cast<RedirectingFileEntry>(E)) {
    ErrorOr<Status> S = ExternalFS->status(F->getExternalContentsPath());
    assert(!S || S->getName() == F->getExternalContentsPath());
    if (S)
      return getRedirectedFileStatus(Path, F->useExternalName(UseExternalNames),
                                     *S);
    return S;
  } else { // directory
    auto *DE = cast<RedirectingDirectoryEntry>(E);
    return Status::copyWithNewName(DE->getStatus(), Path.str());
  }
}

ErrorOr<Status> RedirectingFileSystem::status(const Twine &Path,
                                              bool ShouldCheckExternalFS) {
  ErrorOr<Entry *> Result = lookupPath(Path);
  if (!Result) {
    if (ShouldCheckExternalFS &&
        Result.getError() == llvm::errc::no_such_file_or_directory) {
      return ExternalFS->status(Path);
    }
    return Result.getError();
  }
  return status(Path, *Result);
}

ErrorOr<Status> RedirectingFileSystem::status(const Twine &Path) {
  return status(Path, IsFallthrough);
}

ErrorOr<Status> RedirectingFileSystem::statusVirtualOnly(const Twine &Path) {
  return status(Path, /*ShouldCheckExternalFS=*/false);
}

namespace {

/// Provide a file wrapper with an overriden status.
class FileWithFixedStatus : public File {
  std::unique_ptr<File> InnerFile;
  Status S;

public:
  FileWithFixedStatus(std::unique_ptr<File> InnerFile, Status S)
      : InnerFile(std::move(InnerFile)), S(std::move(S)) {}

  ErrorOr<Status> status() override { return S; }
  ErrorOr<std::unique_ptr<llvm::MemoryBuffer>>

  getBuffer(const Twine &Name, int64_t FileSize, bool RequiresNullTerminator,
            bool IsVolatile) override {
    return InnerFile->getBuffer(Name, FileSize, RequiresNullTerminator,
                                IsVolatile);
  }

  std::error_code close() override { return InnerFile->close(); }
};

} // namespace

ErrorOr<std::unique_ptr<File>>
RedirectingFileSystem::openFileForRead(const Twine &Path) {
  ErrorOr<Entry *> E = lookupPath(Path);
  if (!E) {
    if (IsFallthrough &&
        E.getError() == llvm::errc::no_such_file_or_directory) {
      return ExternalFS->openFileForRead(Path);
    }
    return E.getError();
  }

  auto *F = dyn_cast<RedirectingFileEntry>(*E);
  if (!F) // FIXME: errc::not_a_file?
    return make_error_code(llvm::errc::invalid_argument);

  auto Result = ExternalFS->openFileForRead(F->getExternalContentsPath());
  if (!Result)
    return Result;

  auto ExternalStatus = (*Result)->status();
  if (!ExternalStatus)
    return ExternalStatus.getError();

  // FIXME: Update the status with the name and VFSMapped.
  Status S = getRedirectedFileStatus(Path, F->useExternalName(UseExternalNames),
                                     *ExternalStatus);
  return std::unique_ptr<File>(
      llvm::make_unique<FileWithFixedStatus>(std::move(*Result), S));
}

std::error_code
RedirectingFileSystem::getRealPath(const Twine &Path,
                                   SmallVectorImpl<char> &Output) const {
  ErrorOr<Entry *> Result = lookupPath(Path);
  if (!Result) {
    if (IsFallthrough &&
        Result.getError() == llvm::errc::no_such_file_or_directory) {
      return ExternalFS->getRealPath(Path, Output);
    }
    return Result.getError();
  }

  if (auto *F = dyn_cast<RedirectingFileEntry>(*Result)) {
    return ExternalFS->getRealPath(F->getExternalContentsPath(), Output);
  }
  // Even if there is a directory entry, fall back to ExternalFS if allowed,
  // because directories don't have a single external contents path.
  return IsFallthrough ? ExternalFS->getRealPath(Path, Output)
                       : llvm::errc::invalid_argument;
}

IntrusiveRefCntPtr<FileSystem>
vfs::getVFSFromYAML(std::unique_ptr<MemoryBuffer> Buffer,
                    SourceMgr::DiagHandlerTy DiagHandler,
                    StringRef YAMLFilePath,
                    void *DiagContext,
                    IntrusiveRefCntPtr<FileSystem> ExternalFS) {
  return RedirectingFileSystem::create(std::move(Buffer), DiagHandler,
                                       YAMLFilePath, DiagContext,
                                       std::move(ExternalFS));
}

static void getVFSEntries(Entry *SrcE, SmallVectorImpl<StringRef> &Path,
                          SmallVectorImpl<YAMLVFSEntry> &Entries) {
  auto Kind = SrcE->getKind();
  if (Kind == EK_Directory) {
    auto *DE = dyn_cast<RedirectingDirectoryEntry>(SrcE);
    assert(DE && "Must be a directory");
    for (std::unique_ptr<Entry> &SubEntry :
         llvm::make_range(DE->contents_begin(), DE->contents_end())) {
      Path.push_back(SubEntry->getName());
      getVFSEntries(SubEntry.get(), Path, Entries);
      Path.pop_back();
    }
    return;
  }

  assert(Kind == EK_File && "Must be a EK_File");
  auto *FE = dyn_cast<RedirectingFileEntry>(SrcE);
  assert(FE && "Must be a file");
  SmallString<128> VPath;
  for (auto &Comp : Path)
    llvm::sys::path::append(VPath, Comp);
  Entries.push_back(YAMLVFSEntry(VPath.c_str(), FE->getExternalContentsPath()));
}

void vfs::collectVFSFromYAML(std::unique_ptr<MemoryBuffer> Buffer,
                             SourceMgr::DiagHandlerTy DiagHandler,
                             StringRef YAMLFilePath,
                             SmallVectorImpl<YAMLVFSEntry> &CollectedEntries,
                             void *DiagContext,
                             IntrusiveRefCntPtr<FileSystem> ExternalFS) {
  RedirectingFileSystem *VFS = RedirectingFileSystem::create(
      std::move(Buffer), DiagHandler, YAMLFilePath, DiagContext,
      std::move(ExternalFS));
  ErrorOr<Entry *> RootE = VFS->lookupPath("/");
  if (!RootE)
    return;
  SmallVector<StringRef, 8> Components;
  Components.push_back("/");
  getVFSEntries(*RootE, Components, CollectedEntries);
}

UniqueID vfs::getNextVirtualUniqueID() {
  static std::atomic<unsigned> UID;
  unsigned ID = ++UID;
  // The following assumes that uint64_t max will never collide with a real
  // dev_t value from the OS.
  return UniqueID(std::numeric_limits<uint64_t>::max(), ID);
}

void YAMLVFSWriter::addFileMapping(StringRef VirtualPath, StringRef RealPath) {
  assert(sys::path::is_absolute(VirtualPath) && "virtual path not absolute");
  assert(sys::path::is_absolute(RealPath) && "real path not absolute");
  assert(!pathHasTraversal(VirtualPath) && "path traversal is not supported");
  Mappings.emplace_back(VirtualPath, RealPath);
}

namespace {

class JSONWriter {
  llvm::raw_ostream &OS;
  SmallVector<StringRef, 16> DirStack;

  unsigned getDirIndent() { return 4 * DirStack.size(); }
  unsigned getFileIndent() { return 4 * (DirStack.size() + 1); }
  bool containedIn(StringRef Parent, StringRef Path);
  StringRef containedPart(StringRef Parent, StringRef Path);
  void startDirectory(StringRef Path);
  void endDirectory();
  void writeEntry(StringRef VPath, StringRef RPath);

public:
  JSONWriter(llvm::raw_ostream &OS) : OS(OS) {}

  void write(ArrayRef<YAMLVFSEntry> Entries, Optional<bool> UseExternalNames,
             Optional<bool> IsCaseSensitive, Optional<bool> IsOverlayRelative,
             Optional<bool> IgnoreNonExistentContents, StringRef OverlayDir);
};

} // namespace

bool JSONWriter::containedIn(StringRef Parent, StringRef Path) {
  using namespace llvm::sys;

  // Compare each path component.
  auto IParent = path::begin(Parent), EParent = path::end(Parent);
  for (auto IChild = path::begin(Path), EChild = path::end(Path);
       IParent != EParent && IChild != EChild; ++IParent, ++IChild) {
    if (*IParent != *IChild)
      return false;
  }
  // Have we exhausted the parent path?
  return IParent == EParent;
}

StringRef JSONWriter::containedPart(StringRef Parent, StringRef Path) {
  assert(!Parent.empty());
  assert(containedIn(Parent, Path));
  return Path.slice(Parent.size() + 1, StringRef::npos);
}

void JSONWriter::startDirectory(StringRef Path) {
  StringRef Name =
      DirStack.empty() ? Path : containedPart(DirStack.back(), Path);
  DirStack.push_back(Path);
  unsigned Indent = getDirIndent();
  OS.indent(Indent) << "{\n";
  OS.indent(Indent + 2) << "'type': 'directory',\n";
  OS.indent(Indent + 2) << "'name': \"" << llvm::yaml::escape(Name) << "\",\n";
  OS.indent(Indent + 2) << "'contents': [\n";
}

void JSONWriter::endDirectory() {
  unsigned Indent = getDirIndent();
  OS.indent(Indent + 2) << "]\n";
  OS.indent(Indent) << "}";

  DirStack.pop_back();
}

void JSONWriter::writeEntry(StringRef VPath, StringRef RPath) {
  unsigned Indent = getFileIndent();
  OS.indent(Indent) << "{\n";
  OS.indent(Indent + 2) << "'type': 'file',\n";
  OS.indent(Indent + 2) << "'name': \"" << llvm::yaml::escape(VPath) << "\",\n";
  OS.indent(Indent + 2) << "'external-contents': \""
                        << llvm::yaml::escape(RPath) << "\"\n";
  OS.indent(Indent) << "}";
}

void JSONWriter::write(ArrayRef<YAMLVFSEntry> Entries,
                       Optional<bool> UseExternalNames,
                       Optional<bool> IsCaseSensitive,
                       Optional<bool> IsOverlayRelative,
                       Optional<bool> IgnoreNonExistentContents,
                       StringRef OverlayDir) {
  using namespace llvm::sys;

  OS << "{\n"
        "  'version': 0,\n";
  if (IsCaseSensitive.hasValue())
    OS << "  'case-sensitive': '"
       << (IsCaseSensitive.getValue() ? "true" : "false") << "',\n";
  if (UseExternalNames.hasValue())
    OS << "  'use-external-names': '"
       << (UseExternalNames.getValue() ? "true" : "false") << "',\n";
  bool UseOverlayRelative = false;
  if (IsOverlayRelative.hasValue()) {
    UseOverlayRelative = IsOverlayRelative.getValue();
    OS << "  'overlay-relative': '"
       << (UseOverlayRelative ? "true" : "false") << "',\n";
  }
  if (IgnoreNonExistentContents.hasValue())
    OS << "  'ignore-non-existent-contents': '"
       << (IgnoreNonExistentContents.getValue() ? "true" : "false") << "',\n";
  OS << "  'roots': [\n";

  if (!Entries.empty()) {
    const YAMLVFSEntry &Entry = Entries.front();
    startDirectory(path::parent_path(Entry.VPath));

    StringRef RPath = Entry.RPath;
    if (UseOverlayRelative) {
      unsigned OverlayDirLen = OverlayDir.size();
      assert(RPath.substr(0, OverlayDirLen) == OverlayDir &&
             "Overlay dir must be contained in RPath");
      RPath = RPath.slice(OverlayDirLen, RPath.size());
    }

    writeEntry(path::filename(Entry.VPath), RPath);

    for (const auto &Entry : Entries.slice(1)) {
      StringRef Dir = path::parent_path(Entry.VPath);
      if (Dir == DirStack.back())
        OS << ",\n";
      else {
        while (!DirStack.empty() && !containedIn(DirStack.back(), Dir)) {
          OS << "\n";
          endDirectory();
        }
        OS << ",\n";
        startDirectory(Dir);
      }
      StringRef RPath = Entry.RPath;
      if (UseOverlayRelative) {
        unsigned OverlayDirLen = OverlayDir.size();
        assert(RPath.substr(0, OverlayDirLen) == OverlayDir &&
               "Overlay dir must be contained in RPath");
        RPath = RPath.slice(OverlayDirLen, RPath.size());
      }
      writeEntry(path::filename(Entry.VPath), RPath);
    }

    while (!DirStack.empty()) {
      OS << "\n";
      endDirectory();
    }
    OS << "\n";
  }

  OS << "  ]\n"
     << "}\n";
}

void YAMLVFSWriter::write(llvm::raw_ostream &OS) {
  llvm::sort(Mappings.begin(), Mappings.end(),
             [](const YAMLVFSEntry &LHS, const YAMLVFSEntry &RHS) {
    return LHS.VPath < RHS.VPath;
  });

  JSONWriter(OS).write(Mappings, UseExternalNames, IsCaseSensitive,
                       IsOverlayRelative, IgnoreNonExistentContents,
                       OverlayDir);
}

VFSFromYamlDirIterImpl::VFSFromYamlDirIterImpl(
    const Twine &_Path, RedirectingFileSystem &FS,
    RedirectingDirectoryEntry::iterator Begin,
    RedirectingDirectoryEntry::iterator End, bool IterateExternalFS,
    FileSystem &ExternalFS, std::error_code &EC)
    : Dir(_Path.str()), FS(FS), Current(Begin), End(End),
      IterateExternalFS(IterateExternalFS), ExternalFS(ExternalFS) {
  EC = incrementImpl(/*IsFirstTime=*/true);
}

std::error_code VFSFromYamlDirIterImpl::increment() {
  return incrementImpl(/*IsFirstTime=*/false);
}

std::error_code VFSFromYamlDirIterImpl::incrementExternal() {
  assert(!(IsExternalFSCurrent && ExternalDirIter == directory_iterator()) &&
         "incrementing past end");
  std::error_code EC;
  if (IsExternalFSCurrent) {
    ExternalDirIter.increment(EC);
  } else if (IterateExternalFS) {
    ExternalDirIter = ExternalFS.dir_begin(Dir, EC);
    IsExternalFSCurrent = true;
    if (EC && EC != errc::no_such_file_or_directory)
      return EC;
    EC = {};
  }
  if (EC || ExternalDirIter == directory_iterator()) {
    CurrentEntry = Status();
  } else {
    CurrentEntry = *ExternalDirIter;
  }
  return EC;
}

std::error_code VFSFromYamlDirIterImpl::incrementContent(bool IsFirstTime) {
  assert(IsFirstTime || Current != End && "cannot iterate past end");
  if (!IsFirstTime)
    ++Current;
  while (Current != End) {
    SmallString<128> PathStr(Dir);
    llvm::sys::path::append(PathStr, (*Current)->getName());
    llvm::ErrorOr<vfs::Status> S = FS.statusVirtualOnly(PathStr);
    if (!S) {
      // Skip entries which do not map to a reliable external content.
      if (FS.ignoreNonExistentContents() &&
          S.getError() == llvm::errc::no_such_file_or_directory) {
        ++Current;
        continue;
      } else {
        return S.getError();
      }
    }
    CurrentEntry = *S;
    return {};
  }

  return incrementExternal();
}

std::error_code VFSFromYamlDirIterImpl::incrementImpl(bool IsFirstTime) {
  while (true) {
    std::error_code EC = IsExternalFSCurrent ? incrementExternal()
                                             : incrementContent(IsFirstTime);
    if (EC || !CurrentEntry.isStatusKnown())
      return EC;
    StringRef Name = llvm::sys::path::filename(CurrentEntry.getName());
    if (SeenNames.insert(Name).second)
      return EC; // name not seen before
  }
  llvm_unreachable("returned above");
}

vfs::recursive_directory_iterator::recursive_directory_iterator(FileSystem &FS_,
                                                           const Twine &Path,
                                                           std::error_code &EC)
    : FS(&FS_) {
  directory_iterator I = FS->dir_begin(Path, EC);
  if (I != directory_iterator()) {
    State = std::make_shared<IterState>();
    State->push(I);
  }
}

vfs::recursive_directory_iterator &
recursive_directory_iterator::increment(std::error_code &EC) {
  assert(FS && State && !State->empty() && "incrementing past end");
  assert(State->top()->isStatusKnown() && "non-canonical end iterator");
  vfs::directory_iterator End;
  if (State->top()->isDirectory()) {
    vfs::directory_iterator I = FS->dir_begin(State->top()->getName(), EC);
    if (I != End) {
      State->push(I);
      return *this;
    }
  }

  while (!State->empty() && State->top().increment(EC) == End)
    State->pop();

  if (State->empty())
    State.reset(); // end iterator

  return *this;
}
