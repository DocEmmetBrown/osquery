/**
 * Copyright (c) 2014-present, The osquery authors
 *
 * This source code is licensed as defined by the LICENSE file found in the
 * root directory of this source tree.
 *
 * SPDX-License-Identifier: (Apache-2.0 OR GPL-2.0-only)
 */

#include <boost/algorithm/string.hpp>
#include <boost/algorithm/string/join.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>
#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>

#include <boost/filesystem.hpp>

#include <iostream>
#include <tuple>

#include <osquery/core/core.h>
#include <osquery/core/tables.h>
#include <osquery/filesystem/filesystem.h>
#include <osquery/logger/logger.h>
#include <osquery/utils/conversions/split.h>
#include <regex>

namespace fs = boost::filesystem;
namespace pt = boost::property_tree;

namespace osquery {
namespace tables {

const std::set<std::string> kHomebrewPrefixes = {
    "/usr/local",
    "/opt/homebrew",
};

bool isSymlink(const std::string& path) {
  fs::path p(path);
  return fs::is_symlink(p);
}

std::string getSymlinkTarget(const std::string& path) {
  if (!isSymlink(path)) {
    return path;
  }
  fs::path p(path);
  return fs::read_symlink(p).string();
}

std::vector<std::string> getHomebrewAppInfoPlistPaths(const std::string& root) {
  std::vector<std::string> results;
  auto status = osquery::listDirectoriesInDirectory(root, results);
  if (!status.ok()) {
    TLOG << "Error listing " << root << ": " << status.toString();
  }

  return results;
}

std::string getHomebrewNameFromInfoPlistPath(const std::string& path) {
  auto bits = osquery::split(path, "/");
  return bits[bits.size() - 1];
}

// Homebrew formulas are under a path like
// /usr/local/Cellar/<formula>/<version>/* passing the formula path will
// return the versions
std::vector<std::string> getHomebrewVersionsFromInfoPlistPath(
    const std::string& path) {
  std::vector<std::string> results;
  std::vector<std::string> app_versions;
  auto status = osquery::listDirectoriesInDirectory(path, app_versions);
  if (status.ok()) {
    for (const auto& versionDir : app_versions) {
      std::string version = fs::path(versionDir).filename().string();
      if (version != ".metadata") {
        results.push_back(version);
      }
    }
  } else {
    TLOG << "Error listing " << path << ": " << status.toString();
  }

  return results;
}

// Looking for a "auto_updates true" line in a .rb file
bool checkAutoUpdatesInFile(const std::string& file_path) {
  std::ifstream file(file_path);
  if (!file.is_open()) {
    TLOG << "Error opening file " << file_path;
    return false;
  }

  std::string line;
  std::regex auto_updates_regex(R"(^\s*auto_updates\s*true\s*$)");
  while (std::getline(file, line)) {
    if (std::regex_match(line, auto_updates_regex)) {
      return true;
    }
  }

  return false;
}

// Looking for the value of "auto_updates" in a .json file
// Defaults to false if the key is not found
bool getBooleanValueFromJsonFile(const std::string& file_path,
                                 const std::string& key = "auto_updates") {
  std::string content;
  auto status = osquery::readFile(file_path, content);
  if (!status.ok()) {
    TLOG << "Error reading file " << file_path << ": " << status.toString();
    return false;
  }

  pt::ptree tree;
  std::istringstream is(content);
  try {
    pt::read_json(is, tree);
  } catch (const pt::json_parser_error& e) {
    TLOG << "Error parsing JSON from file " << file_path << ": " << e.what();
    return false;
  }

  return tree.get<bool>(key, false);
}

std::vector<std::string> getHomebrewCaskVersions(const std::string& path) {
  // a Cask full path looks typically like
  // /opt/homebrew/Caskroom/iterm2/3.5.9/iTerm.app -> /Applications/iTerm.app
  //
  // The path passed to this function would be /opt/homebrew/Caskroom/iterm2
  //
  // Auto update date generally exists in the .metadata directory
  // Under a json OR an .rb file with the cask name:
  // /opt/homebrew/Caskroom//iterm2/.metadata/3.5.9/20241116155943.669/Casks/iterm2.json
  // /opt/homebrew/Caskroom//vlc/.metadata/3.0.18/20230607170348.510/Casks/vlc.rb

  // https://github.com/Homebrew/brew/blob/41be66fc2a3facc5a575f98ffa2a17323ef30c22/Library/Homebrew/cask/cask.rb#L175
  // (cask.rb#L175)
  // https://github.com/Homebrew/brew/blob/41be66fc2a3facc5a575f98ffa2a17323ef30c22/Library/Homebrew/cask/installer.rb#L402-L403
  // (installer.rb#L402-L403)

  std::vector<std::string> results;
  std::vector<std::string> files;

  std::string app_name = fs::path(path).filename().string();

  auto metadata_path = path + "/.metadata";
  // Looking for the config file in the .metadata directory
  auto status =
      osquery::listFilesInDirectory(metadata_path, files, true /* recursive */);
  if (!status.ok()) {
    TLOG << "Error listing files in " << metadata_path << ": "
         << status.toString();
    return results;
  }
  // If the config file contains "auto_updates true", we can return early
  // as we know the cask auto updates
  for (const auto& file : files) {
    std::string filename = fs::path(file).filename().string();
    if (filename == app_name + ".json") {
      if (getBooleanValueFromJsonFile(file)) {
        results.push_back("auto_updates");
        return results;
      }
    }
    if (filename == app_name + ".rb") {
      if (checkAutoUpdatesInFile(file)) {
        results.push_back("auto_updates");
        return results;
      }
    }
  }
  // otherwise, we can look for the versions with the "old" method even if the
  // version would be present in the config file because we would need to
  // implement a proper ruby parser to get the version from the `.rb` config
  // file.
  // Fetching it from the json file would be fairly easy though, since
  // it's a first level key.
  return getHomebrewVersionsFromInfoPlistPath(path);
}

void computeVersionsForFormulas(QueryData& results,
                                const std::string& prefix,
                                bool userRequested) {
  fs::path formulaDirPath = fs::path(prefix) / "Cellar";
  std::string type = "formula";

  if (!pathExists(formulaDirPath).ok()) {
    if (userRequested) {
      LOG(WARNING) << "Error reading homebrew " << type << " path "
                   << formulaDirPath.native();
    }
    return;
  }

  for (const auto& path :
       getHomebrewAppInfoPlistPaths(fs::canonical(formulaDirPath).string())) {
    auto versions = getHomebrewVersionsFromInfoPlistPath(path);
    auto name = getHomebrewNameFromInfoPlistPath(path);
    for (const auto& version : versions) {
      // Support a many to one version to package name.
      Row r;
      r["name"] = name;
      r["path"] = path;
      r["version"] = version;
      r["type"] = type;
      r["prefix"] = prefix;

      results.push_back(r);
    }
  }
}

void computeVersionsForCasks(QueryData& results,
                             const std::string& prefix,
                             bool userRequested) {
  fs::path caskDirPath = fs::path(prefix) / "Caskroom";
  std::string type = "cask";

  if (!pathExists(caskDirPath).ok()) {
    if (userRequested) {
      LOG(WARNING) << "Error reading homebrew " << type << " path "
                   << caskDirPath.native();
    }
    return;
  }

  for (const auto& path :
       getHomebrewAppInfoPlistPaths(fs::canonical(caskDirPath).string())) {
    auto versions = getHomebrewCaskVersions(path);
    auto name = getHomebrewNameFromInfoPlistPath(path);
    for (const auto& version : versions) {
      // Support a many to one version to package name.
      Row r;
      r["name"] = name;
      r["path"] = path;
      r["version"] = version;
      r["type"] = type;
      r["prefix"] = prefix;

      results.push_back(r);
    }
  }
}

void packagesFromPrefix(QueryData& results,
                        const std::string& prefix,
                        bool userRequested) {
  // The Homebrew wrapper script finds the Library directory by taking the
  // directory that it is located in and concatenating `/../Library`:
  //   BREW_FILE_DIRECTORY=$(chdir "${0%/*}" && pwd -P)
  //   export HOMEBREW_BREW_FILE="$BREW_FILE_DIRECTORY/${0##*/}"
  // Note that the `-P` flag to pwd resolves all symlinks.
  //
  // Next, it will use given filename to find the prefix:
  //   HOMEBREW_PREFIX = Pathname.new(HOMEBREW_BREW_FILE).dirname.parent

  if (!pathExists(prefix).ok()) {
    if (userRequested) {
      LOG(WARNING) << "Error reading homebrew prefix " << prefix;
    }
    return;
  }

  computeVersionsForFormulas(results, prefix, userRequested);
  computeVersionsForCasks(results, prefix, userRequested);
}

QueryData genHomebrewPackages(QueryContext& context) {
  QueryData results;

  if (context.constraints.count("prefix") > 0 &&
      context.constraints.at("prefix").exists(EQUALS)) {
    std::set<std::string> prefixes =
        context.constraints["prefix"].getAll(EQUALS);
    for (const auto& prefix : prefixes) {
      packagesFromPrefix(results, prefix, true);
    }
  } else {
    // No prefixes requested, fall back to the system ones.
    for (const auto& prefix : kHomebrewPrefixes) {
      packagesFromPrefix(results, prefix, false);
    }
  }
  return results;
}
} // namespace tables
} // namespace osquery
