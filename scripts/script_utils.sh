retry() {
  cmd=$1
  try=${2:-15}       # 15 by default
  sleep_time=${3:-3} # 3 seconds by default

  # Show help if a command to retry is not specified.
  [ -z "$1" ] && echo 'Usage: retry cmd [try=15 sleep_time=3]' && return 1

  # The unsuccessful recursion termination condition (if no retries left)
  [ $try -lt 1 ] && echo 'All retries failed.' && return 1

  # The successful recursion termination condition (if the function succeeded)
  $cmd && return 0

  echo "Execution of '$cmd' failed."

  # Inform that all is not lost if at least one more retry is available.
  # $attempts include current try, so tries left is $attempts-1.
  if [ $((try-1)) -gt 0 ]; then
    echo "There are still $((try-1)) retrie(s) left."
    echo "Waiting for $sleep_time seconds..." && sleep $sleep_time
  fi

  # Recurse
  retry $cmd $((try-1)) $sleep_time
}

ensure_namespace() {
  ns=$1
  kubectl get namespaces ${ns} > /dev/null 2> /dev/null
  if [ $? -ne 0 ]; then
    kubectl create namespace ${ns}
  fi
}
