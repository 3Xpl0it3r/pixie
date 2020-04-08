#!/bin/bash -e

# TODO(oazizi): Consider moving this to somewhere under common.

# If not root, this command runs the provided command through sudo.
# Meant to be used interactively when not root, since sudo will prompt for password.
run_prompt_sudo() {
  cmd=$1
  shift

  if [[ $EUID -ne 0 ]]; then
    if [[ $(stat -c '%u' "$(command -v sudo)") -eq 0 ]]; then
      sudo "$cmd" "$@"
    else
      echo "ERROR: Cannot run as root"
    fi
  else
    "$cmd" "$@"
  fi
}

# This function builds the bazel target and returns the path to the output, relative to ToT.
function bazel_build() {
  target=$1
  flags=$2
  # shellcheck disable=SC2086
  bazel_out=$(bazel build $flags "$target" 2>&1)

  # This funky command looks for and parses out the binary from a output like the following:
  # ...
  # Target //src/stirling:stirling_wrapper up-to-date:
  #   bazel-bin/src/stirling/stirling_wrapper
  # ...
  echo "$bazel_out" | grep -A1 "^Target" | tail -1 | sed -e 's/^[[:space:]]*//'
}

function docker_stop() {
  container_name=$1
  echo "Stopping container $container_name ..."
  docker container stop "$container_name"
}

function docker_load() {
  image=$1
  docker load -i "$image" | awk '/Loaded image/ {print $NF}'
}
