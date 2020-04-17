#!/usr/bin/env bash
# Script to generate certs.
workspace=$(bazel info workspace 2> /dev/null)
LEGO=${workspace}/lego

if [ $# -ne 3 ]; then
  echo "Expected 3 Arguments: EMAIL and OUTDIR and NUMCERTS. Received $#."
  exit 1
fi
EMAIL=$1
OUTDIR=$2
NUMCERTS=$3

function create_cert() {
  if [ $# -ne 2 ]; then
    echo "Expected 2 Arguments: ID and DOMAIN. Received $#."
    exit 1
  fi

  ID=$1
  MIDDOMAIN=$2
  DOMAIN="*.${ID}.${MIDDOMAIN}"

  FNAME_PREFIX="_.${ID}.${MIDDOMAIN}"

  EXISTING_FILES=$(find "${OUTDIR}"/certificates -name "*${FNAME_PREFIX}*" | wc -l)
  if [ "$EXISTING_FILES" -ne 0 ]; then
    echo "Found ${EXISTING_FILES} files with same ID: ${ID}";
    exit 1;
  fi


  $LEGO -k rsa4096 --email="$EMAIL" --domains="$DOMAIN" --dns='gcloud' --path="${OUTDIR}" -a run
}

function create_uuid_certs() {
  if [ $# -ne 1 ]; then
    echo "Expected 1 Arguments: DOMAIN. Received $#."
    exit 1
  fi
  SUBDOMAIN=$1
  for i in $(seq 1 "${NUMCERTS}")
  do
    echo "Creating $i for $SUBDOMAIN"
    UUID=$(uuidgen)
    ADDRESS_ID=${UUID:0:8}
    create_cert "$ADDRESS_ID" "$SUBDOMAIN"
  done
}

# Prepare the output file.
echo "Creating the dev, testing, and nightly certificates"

# Save the original gcp project.
ORIGINAL_GCP_PROJECT=$(gcloud config get-value project)
gcloud config set project pl-dev-infra
export GCE_PROJECT="pl-dev-infra"
create_cert "default" "clusters.dev.withpixie.dev"
create_cert "default" "clusters.testing.withpixie.dev"


echo "Creating the production certificates."

gcloud config set project pixie-prod
export GCE_PROJECT="pixie-prod"
create_uuid_certs "clusters.withpixie.ai"
create_uuid_certs "clusters.staging.withpixie.dev"


# # Return to the original GCP project.
gcloud config set project "${ORIGINAL_GCP_PROJECT}"
