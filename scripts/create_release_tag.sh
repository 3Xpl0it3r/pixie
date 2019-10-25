#!/bin/bash -ex

# This creates a new tag in git for the current commit.

usage() {
    echo "Usage: echo <changelog message> | $0 <artifact_type> [-p] [-r] [-m] [-n]"
    echo " -p : Push the tag to Github."
    echo " -r : Create a release."
    echo " -m : Increment the major version."
    echo " -n : Increment the minor version."
    echo "Example: echo 'this is a change' | $0 cli -p -r -m"
}

parse_args() {
  if [ $# -lt 1 ]; then
    usage
  fi

  ARTIFACT_TYPE=$1
  shift

  while test $# -gt 0; do
      case "$1" in
        -p) PUSH=true
            shift
            ;;
        -r) RELEASE=true
            shift
            ;;
        -m) BUMP_MAJOR=true
            shift
            ;;
        -n) BUMP_MINOR=true
            shift
            ;;
        *)  usage ;;
      esac
  done
}

check_args() {
    if [ "$BUMP_MAJOR" = "true" ] || [ "$BUMP_MINOR" = "true" ]; then
        if [ "$RELEASE" != "true" ]; then
          echo "Cannot bump major and minor version in non-release."
          exit
        fi
    fi

    if [ "$BUMP_MAJOR" = "true" ] && [ "$BUMP_MINOR" = "true" ]; then
        echo "Cannot bump both major and minor."
        exit
    fi

    if [ "$ARTIFACT_TYPE" != "cli" ] && [ "$ARTIFACT_TYPE" != "vizier" ]; then
        echo "Unsupported artifact type."
        exit
    fi
}

get_bazel_target() {
    case "$ARTIFACT_TYPE" in
        cli) BAZEL_TARGET=//src/utils/pixie_cli:pixie;;
        vizier) BAZEL_TARGET=:vizier_images_bundle;;
    esac
}

function semver {
    v=$1
    v=${v//\+*/}
    echo "$v"
}

function parse {
    s=$(semver "$1")
    # shellcheck disable=SC2001
    pre=$(echo "$s" | sed 's|.*\-\(.*\)|\1|')
    s=${s//-*/}

    split=$(echo "$s" | tr '.' ' ')
    echo "${split} ${pre}"
}

function bump_patch {
    read -r major minor patch pre <<< "$(parse "$1")"
    echo "$major.$minor.$((patch +1))"
}

function bump_major {
    read -r major minor patch pre <<< "$(parse "$1")"
    echo "$((major +1)).0.0"
}

function bump_minor {
    read -r major minor patch pre <<< "$(parse "$1")"
    echo "$major.$((minor +1)).0"
}

function update_pre {
    read -r major minor patch pre <<< "$(parse "$1")"
    commits=$2
    echo "$major.$minor.$patch-pre.$commits"
}

parse_args "$@"
check_args
get_bazel_target

# Get input from stdin.
CHANGELOG=''
while IFS= read -r line; do
    CHANGELOG="${CHANGELOG}${line}\n"
done

# Get the latest release tag.
tags=$(git for-each-ref --sort='-*authordate' --format '%(refname:short)' refs/tags \
    | grep "release/$ARTIFACT_TYPE" | grep -v "\-pre")

# Get the most recent tag.
prev_tag=$(echo "$tags" | head -1)

# Parse the tag.
version_str=${prev_tag//*\/v/}

new_version_str=""
if [ "$RELEASE" = "true" ]; then
    if [ "$BUMP_MAJOR" = "true" ]; then
        new_version_str=$(bump_major "$version_str")
    elif [ "$BUMP_MINOR" = "true" ]; then
        new_version_str=$(bump_minor "$version_str")
    else
        new_version_str=$(bump_patch "$version_str")
    fi
else
    # Find the number all the commits between now and the last release.
    commits=$(git log HEAD..."$prev_tag" --pretty=format:"%H")

    # Find all file dependencies of the bazel target.
    bazel query 'kind("source file", deps('"$BAZEL_TARGET"'))' | sed  -e 's/:/\//' -e 's/^\/\+//' > target_files.txt
    trap "rm -f target_files.txt" EXIT

    commit_count=0
    # For each commit, check if it has modified a file in the bazel target's dependencies.
    for commit in $commits
    do
        files=$(git show --name-only --format=oneline "$commit" | tail -n +2)
        for file in $files
        do
            if grep -iq "$file" target_files.txt; then
                commit_count=$((commit_count + 1))
                break
            fi
        done
    done

    new_version_str=$(update_pre "$version_str" "$commit_count")
fi

new_tag="release/$ARTIFACT_TYPE/v"$new_version_str
git tag -a "$new_tag" -m "$CHANGELOG"

if [ "$PUSH" = "true" ]; then
  git push origin "$new_tag"
fi
