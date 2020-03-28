#ifdef __linux__

#include <system_error>

#include "src/common/fs/fs_wrapper.h"

namespace pl {
namespace fs {

Status CreateSymlink(std::filesystem::path target, std::filesystem::path link) {
  std::error_code ec;
  std::filesystem::create_symlink(target, link, ec);
  if (ec) {
    if (ec.value() == EEXIST) {
      return error::AlreadyExists(
          "Failed to create symlink $0 -> $1. The link already exists. "
          "Message: $2",
          link.string(), target.string(), ec.message());
    }
    return error::System("Failed to create symlink $0 -> $1. Message: $2", link.string(),
                         target.string(), ec.message());
  }
  return Status::OK();
}

Status CreateDirectories(std::filesystem::path dir) {
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  if (ec) {
    return error::System("Failed to create directory $0. Message: $1", dir.string(), ec.message());
  }
  return Status::OK();
}

pl::StatusOr<std::filesystem::path> ReadSymlink(std::filesystem::path symlink) {
  std::error_code ec;
  std::filesystem::path res = std::filesystem::read_symlink(symlink, ec);
  if (ec) {
    return error::System("Could not read symlink: $0. Message: $1", symlink.string(), ec.message());
  }
  return res;
}

std::filesystem::path JoinPath(const std::vector<const std::filesystem::path*>& paths) {
  std::filesystem::path res;
  for (const auto& p : paths) {
    if (p->empty()) {
      continue;
    }
    if (res.empty()) {
      res = *p;
    } else {
      res /= p->relative_path();
    }
  }
  return res;
}

Status CreateSymlinkIfNotExists(std::filesystem::path target, std::filesystem::path link) {
  PL_RETURN_IF_ERROR(fs::CreateDirectories(link.parent_path()));

  // Attempt to create the symlink, but ignore the return status.
  // Why? Because if multiple instances are running in parallel, this CreateSymlink could fail.
  // That's okay. The real check to make sure the link is created is below.
  Status s = fs::CreateSymlink(target, link);
  PL_UNUSED(s);

  PL_ASSIGN_OR_RETURN(std::filesystem::path actual_target, fs::ReadSymlink(link));
  if (target != actual_target) {
    return error::Internal("Symlink not as expected [desired=$0, actual=$1]", target.c_str(),
                           actual_target.c_str());
  }
  return Status::OK();
}

Status Exists(std::filesystem::path path) {
  std::error_code ec;
  if (std::filesystem::exists(path, ec)) {
    return Status::OK();
  }
  if (ec) {
    return error::System(ec.message());
  }
  return error::InvalidArgument("Does not exist");
}

StatusOr<std::filesystem::path> Relative(const std::filesystem::path& path,
                                         const std::filesystem::path& base) {
  std::error_code ec;
  auto res = std::filesystem::relative(path, base, ec);
  if (ec) {
    return error::System(ec.message());
  }
  return res;
}

namespace {

bool IsParent(const std::filesystem::path& child, const std::filesystem::path& parent) {
  auto c_iter = child.begin();
  auto p_iter = parent.begin();
  for (; c_iter != child.end() && p_iter != parent.end(); ++c_iter, ++p_iter) {
    if (*c_iter != *p_iter) {
      break;
    }
  }
  return p_iter == parent.end();
}

}  // namespace

StatusOr<std::filesystem::path> GetChildRelPath(std::filesystem::path child,
                                                std::filesystem::path parent) {
  if (child.empty() || parent.empty()) {
    return error::InvalidArgument("Both paths must not be empty, child=$0, parent=$1",
                                  child.string(), parent.string());
  }
  // Relative() returns ".." when child is a sibling of parent. IsParent() rules out such cases.
  if (!IsParent(child, parent)) {
    return error::InvalidArgument("Path=$0 is not parent of child=$1", parent.string(),
                                  child.string());
  }
  PL_ASSIGN_OR_RETURN(std::filesystem::path res, Relative(child, parent));
  // Relative() returns "." when child and parent are the same. "." complicates the path joining.
  if (res == ".") {
    res.clear();
  }
  return res;
}

std::vector<PathSplit> EnumerateParentPaths(const std::filesystem::path& path) {
  std::vector<PathSplit> res;

  std::filesystem::path child;
  std::filesystem::path parent = path;
  while (parent != parent.parent_path()) {
    res.push_back(PathSplit{parent, child});
    if (child.empty()) {
      child = parent.filename();
    } else {
      child = parent.filename() / child;
    }
    parent = parent.parent_path();
  }
  if (path.is_absolute()) {
    res.push_back(PathSplit{"/", path.relative_path()});
  }
  return res;
}

}  // namespace fs
}  // namespace pl

#endif
