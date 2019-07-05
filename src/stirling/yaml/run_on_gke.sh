#!/bin/bash -e

usage() {
  echo "This script runs stirling_wrapper on GKE for a specified amount of time."
  echo "It then collects logs and deletes the stirling_wrapper pods."
  echo ""
  echo "Usage: $0 [<duration>]"
  exit
}

parse_args() {
  # Set default values.
  T=30 # Time spent running stirling on the cluster.

  # Grab arguments.
  if [ $# -gt 0 ]; then
    T=$1
  fi

  # Make sure the time argument is a number.
  # This will also cause usage to be printed if -h or any flag is passed in.
  re='^[0-9]+$'
  if ! [[ $T =~ $re ]] ; then
    usage
  fi
}

# Script execution starts here

# Always run in the script directory, regardless of where the script is called from.
cd $(dirname $0)

NAMESPACE=pl-${USER}

parse_args "$@"

echo ""
echo "-------------------------------------------"
echo "Building and pushing stirling_wrapper image"
echo "-------------------------------------------"

bazel build //src/stirling:stirling_wrapper_image
bazel run //src/stirling:push_stirling_wrapper_image

echo ""
echo "-------------------------------------------"
echo "Delete any old instances"
echo "-------------------------------------------"

if [ $(kubectl get pods -n ${NAMESPACE} | grep ^stirling-wrapper | wc -l) -ne 0 ]; then
  make delete_stirling_daemonset
  sleep 5
fi

echo ""
echo "-------------------------------------------"
echo "Deploying stirling_wrapper"
echo "-------------------------------------------"

make deploy_stirling_daemonset

echo ""
echo "-------------------------------------------"
echo "Waiting ${T} seconds to collect data"
echo "-------------------------------------------"

sleep $T

echo ""
echo "-------------------------------------------"
echo "Listing pods"
echo "-------------------------------------------"

kubectl get pods -n ${NAMESPACE} | grep ^stirling-wrapper
pods=$(kubectl get pods -n ${NAMESPACE} | grep ^stirling-wrapper | grep Running | cut -f1 -d' ')

echo ""
echo "-------------------------------------------"
echo "Collecting logs"
echo "-------------------------------------------"

timestamp=$(date +%s)
for pod in $pods; do
  filename=log$timestamp.$pod
  kubectl logs -n ${NAMESPACE} $pod > $filename
  echo $filename
done

echo ""
echo "-------------------------------------------"
echo "Cleaning up (deleting pods)"
echo "-------------------------------------------"

make delete_stirling_daemonset
