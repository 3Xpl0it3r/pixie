const AUTH0_DOMAIN = '__CONFIG_AUTH0_DOMAIN__';
const AUTH0_CLIENT_ID = '__CONFIG_AUTH0_CLIENT_ID__';
const DOMAIN_NAME = '__CONFIG_DOMAIN_NAME__';
const SEGMENT_UI_WRITE_KEY = '__SEGMENT_UI_WRITE_KEY__';

// There is a bug with using esnext + webpack-replace-plugin, where
// lines with an export and replacement will not compile properly.
// eslint-disable-next-line no-underscore-dangle
window.__PIXIE_FLAGS__ = {
  AUTH0_DOMAIN, AUTH0_CLIENT_ID, DOMAIN_NAME, SEGMENT_UI_WRITE_KEY,
};
